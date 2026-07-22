/*
 * PLAYMID. ESXDOS command to play MIDI files in a ZX Spectrum 128K computer
 * Copyright (C) 2019 Miguel Angel Rodriguez Jodar
 *
 * This program is free software: you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation, either version 3 of the License, or (at your option)
 * any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of  MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program.  If not, see <http://www.gnu.org/licenses/>.
 */

/*
Compilar con (SDCC 3.x):
sdcc -mz80 --reserve-regs-iy --opt-code-size --max-allocs-per-node 10000 ^
--nostdlib --nostdinc --no-std-crt0 --code-loc 0x2000 --data-loc 0x2d00 playmid.c z80.lib -L "C:\Program Files\SDCC\lib\z80"
makebin -s 65535 -p playmid.ihx playmid.bin
dd if=playmid.bin of=PLAYMID bs=1 skip=8192

Con SDCC 4.2 o superior, compilar con la convencion de llamada por defecto (NO usar --sdcccall 0:
z80.lib viene compilada con la convencion por registros y las rutinas de multiplicacion/division
recibirian basura; las funciones con ensamblador incrustado ya van marcadas con __sdcccall(0)):
sdcc -mz80 --reserve-regs-iy --opt-code-size --max-allocs-per-node 10000 \
--nostdlib --nostdinc --no-std-crt0 --code-loc 0x2000 --data-loc 0x2d00 playmid.c z80.lib -L /path/to/sdcc/lib/z80

OJO con --data-loc. Si el código de este programa crece, habría que mover --data-loc adecuadamente para que no se
solapen codigo y datos. Comprobar en el .map que _CODE+codigo de librerias termina antes de --data-loc, y que
DATA termina antes de 0x3000 (donde empieza el buffer).

MAPA DE MEMORIA (DivMMC RAM, 0x2000-0x3FFF):
  0x2000-0x2CFF : codigo + literales
  0x2D00-0x2FFF : datos (variables globales, estado de pistas)
  0x3000-0x33FF : buffer de 1KB: 64 bytes de staging de salida + 960 bytes de cachés
                  de lectura repartidos entre las pistas
  0x3400-0x3FFF : NO TOCAR. Lanzadores de comandos como el LNF Browser guardan aqui su
                  propio estado; escribir en esta zona cuelga o resetea al volver.

*/

typedef unsigned char BYTE;
typedef unsigned short WORD;
typedef unsigned long DWORD;

// Las funciones con ensamblador incrustado leen sus parametros de la pila (4(ix), etc),
// es decir, usan la convencion de llamada clasica de SDCC 3.x. En SDCC 4.2+ la convencion
// por defecto es por registros, y OJO: z80.lib viene compilada con ella, asi que hay que
// compilar SIN --sdcccall 0 (o las rutinas de multiplicacion/division de 32 bits reciben
// basura) y marcar solo estas funciones como __sdcccall(0).
#if defined(__SDCC_VERSION_MAJOR) && (__SDCC_VERSION_MAJOR >= 4)
#define STACKARGS __sdcccall(0)
#else
#define STACKARGS
#endif

__sfr __at (0xfe) ULA;
__sfr __at (0xff) ATTR;

__sfr __banked __at (0xf7fe) SEMIFILA1;
__sfr __banked __at (0xeffe) SEMIFILA2;
__sfr __banked __at (0xfbfe) SEMIFILA3;
__sfr __banked __at (0xdffe) SEMIFILA4;
__sfr __banked __at (0xfdfe) SEMIFILA5;
__sfr __banked __at (0xbffe) SEMIFILA6;
__sfr __banked __at (0xfefe) SEMIFILA7;
__sfr __banked __at (0x7ffe) SEMIFILA8;

__sfr __banked __at (0xfffd) AYREGSELECT;
__sfr __banked __at (0xbffd) AYREGWRITE;

__sfr __banked __at (0xfc3b) ZXUNOADDR;
__sfr __banked __at (0xfd3b) ZXUNODATA;

#define ATTRP 23693
#define ATTRT 23695
#define BORDR 23624
#define LASTK 23560

#define WAIT_VRETRACE __asm halt __endasm
#define WAIT_HRETRACE while(ATTR!=0xff)
#define SETCOLOR(x) *(BYTE *)(ATTRP)=(x)
#define LASTKEY *(BYTE *)(LASTK)
#define ATTRPERMANENT *((BYTE *)(ATTRP))
#define ATTRTEMPORARY *((BYTE *)(ATTRT))
#define BORDERCOLOR *((BYTE *)(BORDR))

#define MAKEWORD(d,h,l) { ((BYTE *)&(d))[0] = (l) ; ((BYTE *)&(d))[1] = (h); }

/* Some ESXDOS system calls */
#define HOOK_BASE   128
#define MISC_BASE   (HOOK_BASE+8)
#define FSYS_BASE   (MISC_BASE+16)
#define M_GETSETDRV (MISC_BASE+1)
#define F_OPEN      (FSYS_BASE+2)
#define F_CLOSE     (FSYS_BASE+3)
#define F_READ      (FSYS_BASE+5)
#define F_WRITE     (FSYS_BASE+6)
#define F_SEEK      (FSYS_BASE+7)
#define F_GETPOS    (FSYS_BASE+8)

#define FMODE_READ	     0x1 // Read access
#define FMODE_WRITE      0x2 // Write access
#define FMODE_OPEN_EX    0x0 // Open if exists, else error
#define FMODE_OPEN_AL    0x8 // Open if exists, if not create
#define FMODE_CREATE_NEW 0x4 // Create if not exists, if exists error
#define FMODE_CREATE_AL  0xc // Create if not exists, else open and truncate

#define SEEK_START       0
#define SEEK_CUR         1
#define SEEK_BKCUR       2

BYTE errno;

// Esta precisión la he elegido suponiendo que ppq nunca será mayor en la práctica de 2048, para que no 
// desborde en 32 bits al calcular el valor de ticks_per_int en un evento FF 03 58
#define PRECISION 64

// variables globales en lugar de locales para agilizar su lectura, y no depender de direccionamiento indexado
// que engordaría y enlentecería (más aún) el programa

BYTE formato;  // formato del fichero MIDI: 0, 1 o 2.
BYTE i, c;     // contadores de bucle, etc.
BYTE param1;   // tipo de metaevento
DWORD ppq, ticks_per_int;  // variables que se usan para calcular el tempo de la melodía
__at(0x3000) BYTE buffer[1024];  // Staging para la cabecera y los eventos MIDI salientes. No moverlo de aqui sin tocar SendMIDI
WORD lbytes;             // longitud de metaeventos y sysex
DWORD us_per_quarter;    // ultimo tempo leido con el metaevento Set Tempo

// ---- Soporte para MIDI formato 1 (multipista) ----
// Todo vive DENTRO del buffer de 1KB en 0x3000: los primeros 64 bytes son el staging de
// salida (y scratch para cabeceras), y los 960 restantes se reparten como cachés de
// lectura entre las pistas. OJO: no usar memoria por encima de 0x33FF; lanzadores como
// el LNF Browser guardan su propio estado/pila en la parte alta de la página DivMMC y
// escribir ahí cuelga o resetea la maquina al volver.
#define MAX_TRACKS   24                   // numero maximo de pistas que podemos mezclar (los format 1 tipicos traen 1+16)
#define TCACHE_TOTAL (1024-64)            // bytes de buffer disponibles para cachés de pista
#define tcaches      (buffer+64)          // las cachés empiezan tras el staging

BYTE fhandle;                    // handle del fichero, global para poder recargar cachés desde cualquier rutina

BYTE main (char *p) STACKARGS;
void usage (void);
BYTE commandlinemode (char *p);

void __sdcc_enter_ix (void) __naked;

void puts (BYTE *) STACKARGS;
void u16tohex (WORD n, char *s);
void u8tohex (BYTE n, char *s);
void print8bhex (BYTE n);
void print16bhex (WORD n);

BYTE open (char *filename, BYTE mode) STACKARGS;
void close (BYTE handle) STACKARGS;
WORD read (BYTE handle, BYTE *buffer, WORD nbytes) STACKARGS;
void seekset (BYTE handle, DWORD offset) STACKARGS;

/* --------------------------------------------------------------------------------- */
/* --------------------------------------------------------------------------------- */
/* --------------------------------------------------------------------------------- */
void getfilename (char *p, char *fname);
void playmidi (BYTE f);
int cmp4b (BYTE *a, BYTE *b);
void SendMIDI (BYTE *ev, BYTE lev) STACKARGS;
void SendMIDIByte (void) __naked;

// Rutina inicial. Debe ser el primer código que se encuentre en el fichero. Esta inicialización
// está pensada para ser usada con ficheros .command de ESXDOS.
void init (void) __naked
{
     __asm
     xor a
     ld (#_errno),a
     push hl
     call _main
     inc sp
     inc sp
     ld iy,#23610  ;algunas rutinas de la libreria Z80 tocan IY :(
     ld a,l
     or a
     ret z
     scf
     ret
     __endasm;
}

// Programa principal. Toma como argumento un puntero al comienzo de la linea de comandos. Si es NULL, no hay linea de comandos
BYTE main (char *p) STACKARGS
{
  BYTE res, comando;

  // Algunos lanzadores (p.ej. el LNF Browser) pueden pasarnos el control con las
  // interrupciones deshabilitadas, y el primer HALT colgaria la maquina para siempre.
  __asm
  ei
  __endasm;

  // Dejamos el puerto MIDI inactivo
  AYREGSELECT = 0x0e;
  AYREGWRITE = 0xfe;

  // GM MIDI Reset
  WAIT_VRETRACE;
  comando = 0xFF;   // envío el comando FF para resetear el MIDI
  SendMIDI (&comando, 1);

  // Si no hay fichero para abrir, mostrar ayuda
  if (!p)
  {
     usage();
     return 0;
  }
  else
      res = commandlinemode(p);

  // GM MIDI Reset
  WAIT_VRETRACE;
  comando = 0xFF;   // envío el comando FF para resetear el MIDI
  SendMIDI (&comando, 1);

  return res;
}

// Abre el fichero. Si no existe, retorna con error. Si existe, pasa su handle a la rutina principal.
BYTE commandlinemode (char *p)
{
    char fname[32];
    BYTE handle;

    getfilename (p, fname);
    handle = open (fname, FMODE_READ);
    if (handle==0xff)
       return errno;

    playmidi (handle);

    close (handle);
    return 0;
}

// Muestra ayuda de uso del comando
void usage (void)
{
        // 01234567890123456789012345678901
    puts (" PLAYMID file.mid\xd\xd"
          "Plays a MIDI format 0 or 1 file\xd"
          "thru MIDI OUT connector.\xd");
}

////////////////////////////////////////////////////////////////////////////////

/* ============================ FORMAT1 ENGINE BEGIN ============================
   Motor de reproducción: mezcla N pistas "al vuelo" (formato 1). El formato 0
   es el caso trivial de una sola pista y se reproduce con el mismo motor.
   Cada pista mantiene su propio offset dentro del fichero, su running status,
   el tick absoluto de su siguiente evento, y una caché de lectura propia.
   El bucle principal elige siempre la pista cuyo siguiente evento es el más
   cercano en el tiempo (merge por tick mínimo), espera hasta ese tick, y
   procesa el evento recargando la caché con F_SEEK + F_READ si hace falta.
   Los eventos de tempo (FF 51) se aplican globalmente, lo que con el merge
   por ticks da el resultado correcto sin más esfuerzo. */

BYTE tracks;                   // numero de pistas MTrk encontradas (max MAX_TRACKS)
BYTE curtrk;                   // pista que se está procesando ahora mismo
BYTE *cptr;                    // puntero a la caché de curtrk
WORD tcsize;                   // tamaño de la caché de cada pista (TCACHE_TOTAL/tracks)
DWORD trk_off[MAX_TRACKS];     // offset en el fichero de la proxima recarga de caché
DWORD trk_next[MAX_TRACKS];    // tick absoluto (escalado por PRECISION) del proximo evento
BYTE trk_status[MAX_TRACKS];   // running status propio de cada pista (imprescindible al mezclar)
BYTE trk_end[MAX_TRACKS];      // a 1 cuando la pista ha terminado (FF 2F o EOF)
WORD trk_cpos[MAX_TRACKS];     // posicion de lectura dentro de la caché
WORD trk_clen[MAX_TRACKS];     // bytes válidos en la caché
DWORD now;                     // ticks transcurridos desde el principio (escalado por PRECISION)

// Selecciona la pista activa y apunta cptr a su caché
void set_curtrk (BYTE t)
{
    curtrk = t;
    cptr = tcaches + (WORD)t * tcsize;
}

// Recarga la caché de la pista activa desde su offset en el fichero.
// Si no quedan bytes (EOF o error), da la pista por terminada.
void trk_refill (void)
{
    WORD n;

    seekset (fhandle, trk_off[curtrk]);
    n = read (fhandle, cptr, tcsize);
    if (n == 0 || n == 0xFFFF)
    {
        trk_end[curtrk] = 1;
        trk_clen[curtrk] = 0;
    }
    else
    {
        trk_off[curtrk] += n;
        trk_clen[curtrk] = n;
    }
    trk_cpos[curtrk] = 0;
}

// Lee el siguiente byte de la pista activa, sin consumirlo
BYTE trk_peek (void)
{
    if (trk_cpos[curtrk] >= trk_clen[curtrk])
    {
        trk_refill();
        if (trk_end[curtrk])
            return 0;
    }
    return cptr[trk_cpos[curtrk]];
}

// Lee y consume el siguiente byte de la pista activa
BYTE trk_get (void)
{
    if (trk_cpos[curtrk] >= trk_clen[curtrk])
    {
        trk_refill();
        if (trk_end[curtrk])
            return 0;
    }
    return cptr[trk_cpos[curtrk]++];
}

// Lee una cantidad de longitud variable (delta o longitud de metaevento/sysex)
DWORD trk_varlen (void)
{
    DWORD v = 0;

    do
    {
        c = trk_get();
        v = (v<<7) | (c & 0x7F);
    }
    while (c & 0x80);
    return v;
}

// Procesa un evento de la pista activa (el delta ya se consumió antes).
// A diferencia del reproductor de formato 0, aqui SIEMPRE se envia el byte de
// estado: el running status de la linea MIDI se rompe al intercalar pistas.
void trk_event (void)
{
    BYTE st, n;

    c = trk_peek();
    if (c & 0x80)          // byte de estado nuevo: lo consumimos y actualizamos el running status de ESTA pista
    {
        trk_status[curtrk] = c;
        trk_get();
    }
    st = trk_status[curtrk];

    // EVENTOS F0 y F7 (SYSEX). Se envian por trozos usando el buffer como staging
    if (st == 0xF0 || st == 0xF7)
    {
        lbytes = trk_varlen();
        if (st == 0xF0)
        {
            buffer[0] = 0xF0;
            SendMIDI (buffer, 1);
        }
        while (lbytes)
        {
            n = (lbytes > 64) ? 64 : lbytes;   // el staging son los primeros 64 bytes del buffer
            for (i=0;i<n;i++)
                buffer[i] = trk_get();
            SendMIDI (buffer, n);
            lbytes -= n;
        }
        return;
    }

    // METAEVENTOS FF. Todos tienen la forma FF tipo longitud datos, asi que se
    // pueden saltar de forma generica. Solo tempo y fin de pista nos interesan.
    if (st == 0xFF)
    {
        param1 = trk_get();
        lbytes = trk_varlen();
        if (param1 == 0x2F)          // fin de pista
        {
            trk_end[curtrk] = 1;
            return;
        }
        if (param1 == 0x51 && lbytes == 3)   // Set Tempo: se aplica globalmente
        {
            us_per_quarter = 0;
            for (i=0;i<3;i++)
                us_per_quarter = (us_per_quarter<<8) | trk_get();
            if (us_per_quarter)
                ticks_per_int = PRECISION * 20000L * ppq / us_per_quarter;
            return;
        }
        while (lbytes--)             // el resto de metaeventos se ignora
            trk_get();
        return;
    }

    // EVENTOS de canal: estado + 1 o 2 bytes de datos, via staging
    buffer[0] = st;
    buffer[1] = trk_get();
    c = st & 0xF0;
    if (c == 0xC0 || c == 0xD0)
        n = 2;
    else
    {
        buffer[2] = trk_get();
        n = 3;
    }
    SendMIDI (buffer, n);
}

// Bucle principal de reproduccion de formato 1.
// ntrk es el numero de pistas que declara la cabecera MThd.
void playmidi1 (BYTE ntrk)
{
    BYTE best;
    DWORD len, fpos;

    // Recorremos los chunks del fichero construyendo la tabla de offsets de
    // comienzo de cada pista. La longitud de cada chunk está en su cabecera.
    tracks = 0;
    fpos = 14;
    while (tracks < ntrk && tracks < MAX_TRACKS)
    {
        seekset (fhandle, fpos);
        if (read (fhandle, buffer, 8) != 8)
            break;
        len = ((DWORD)buffer[4]<<24) | ((DWORD)buffer[5]<<16) | ((DWORD)buffer[6]<<8) | buffer[7];
        fpos += 8;
        if (cmp4b (buffer, "MTrk"))
        {
            trk_off[tracks] = fpos;
            trk_status[tracks] = 0;
            trk_end[tracks] = 0;
            trk_cpos[tracks] = 0;
            trk_clen[tracks] = 0;
            tracks++;
        }
        fpos += len;    // chunks desconocidos se saltan sin contarlos
    }
    if (tracks == 0)
    {
        puts ("MTrk chunk expected\xd");
        return;
    }

    // Cuantas menos pistas, mas caché por pista y menos seeks durante la reproduccion
    tcsize = TCACHE_TOTAL / tracks;

    // Leemos el primer delta de cada pista para inicializar su next_tick
    for (best = 0; best < tracks; best++)
    {
        set_curtrk (best);
        trk_next[best] = trk_varlen() * PRECISION;
    }

    now = 0;
    while (1)
    {
        // Si pulsamos SPACE, salir
        if ((SEMIFILA8 & 0x1) == 0)
            return;

        // Elegimos la pista viva cuyo proximo evento tiene el tick minimo
        best = 0xFF;
        for (i = 0; i < tracks; i++)
        {
            if (trk_end[i])
                continue;
            if (best == 0xFF || trk_next[i] < trk_next[best])
                best = i;
        }
        if (best == 0xFF)      // todas las pistas han terminado
            return;

        // Esperamos hasta que el reloj global alcance el tick del evento
        while (now < trk_next[best])
        {
            if ((SEMIFILA8 & 0x1) == 0)
                return;
            WAIT_VRETRACE;
            now += ticks_per_int;
        }

        // Procesamos el evento y programamos el siguiente de esta pista
        set_curtrk (best);
        trk_event ();
        if (!trk_end[best])
            trk_next[best] += trk_varlen() * PRECISION;
    }
}

/* ============================ FORMAT1 ENGINE END ============================ */

// Rutina principal de reproducción MIDI. Analiza la cabecera y lanza el motor
// de mezcla, que reproduce tanto formato 1 como formato 0 (caso trivial de una
// sola pista, con toda la caché para ella, asi que las lecturas son secuenciales).
void playmidi (BYTE f)
{
    // Leemos la cabecera MIDI (14 bytes)
    read (f, buffer, 14);

    // Comprobamos que realmente es una cabecera MIDI, y si no, retornamos con error
    if (cmp4b (buffer, "MThd") == 0)
    {
        puts ("MThd chunk expected\xd");
        return;
    }

    // Leemos el formato del fichero. Aceptamos formatos 0 y 1
    formato = buffer[9];
    if (formato > 1)
    {
        puts ("Only format 0 or 1 MIDI files. Sorry\xd");
        return;
    }

    // Leemos el PPQ (partes por quarter, o el numero de ticks del reloj de MIDI que dura una negra
    ppq = buffer[12]<<8 | buffer[13];

    //ticks_per_quarter = <PPQ from the header>
    //µs_per_quarter = <Tempo in latest Set Tempo event>
    //µs_per_tick = µs_per_quarter / ticks_per_quarter
    //seconds_per_tick = µs_per_tick / 1.000.000
    //seconds = ticks * seconds_per_tick

    // calculamos el numero de ticks MIDI que hay en una interrupción del Spectrum (20ms)
    // Este cálculo es distinto dependiendo del bit 7 del byte 12 de la cabecera
    if (buffer[12]&0x80)
    {
        buffer[12] &= 0x7F;
        ticks_per_int = PRECISION * buffer[12] * buffer[13] * 20;
    }
    else  // habitualmente los MIDs lo calculan de esta otra forma, es decir, habitualmente el bit 7 del byte 12 es 0.
    {
        ticks_per_int = PRECISION * 20 * ppq / 500;
    }

    fhandle = f;
    playmidi1 (buffer[10] ? MAX_TRACKS : buffer[11]);   // numero de pistas de la cabecera (topado a MAX_TRACKS)
}

// Rutina para comparar rapidamente dos bloques de memoria de 4 bytes
int cmp4b (BYTE *a, BYTE *b)
{
    if (a[0] != b[0] || a[1] != b[1] || a[2] != b[2] || a[3] != b[3])
        return 0;
    else
        return 1;
}

// Estos pragmas son para que no se queje el compilador por argumentos aparentemente no usados
#pragma disable_warning 85
#pragma disable_warning 59

// Envia un bloque de memoria a la salida MIDI del Spectrum
void SendMIDI (BYTE *ev, BYTE lev) STACKARGS
{
  BYTE d;

  // Si estamos en el ZXUNO, esta operación debe hacerse a la velocidad estándar (3.5 MHz)
  ZXUNOADDR = 0xb;
  d = ZXUNODATA;
  ZXUNODATA = d & 0x3F;
  __asm
  push bc
  push de
  ld l,4(ix)
  ld h,5(ix)
  ld b,6(ix)

buc_send_midi:
  ld a,(hl)
  push bc
  push hl
  di
  call _SendMIDIByte   ;llamada a la rutina para enviar un byte por MIDI
  ei
  pop hl
  pop bc
  inc hl
  ld a,h
  and #0xF3  ;esto asume que el buffer esta en 0x3000 - 0x33FF
  ld h,a
  djnz buc_send_midi

  pop de
  pop bc
  __endasm;

  // ZXUNO: restauramos la velocidad nominal que hubiera
  ZXUNODATA = d;
}

// Esta es una copia total de la rutina de la ROM 1 del 128K para enviar un byte MIDI.
// El fuente proviene del desensamble que está en la página de Paul Farrow. Si hubiera problemas con este cachito de código
// lo reescribiré.
void SendMIDIByte (void) __naked
{
  __asm
L11A3:  LD   L,A          ; Store the byte to send.

        LD   BC,#0xFFFD     ;
        LD   A,#0x0E        ;
        OUT  (C),A        ; Select register 14 - I/O port.

        LD   BC,#0xBFFD     ;
        LD   A,#0xFA        ; Set RS232 'RXD' transmit line to 0. (Keep KEYPAD 'CTS' output line low to prevent the keypad resetting)
        OUT  (C),A        ; Send out the START bit.

        LD   E,#0x03        ; (7) Introduce delays such that the next bit is output 113 T-states from now.

L11B4:  DEC  E            ; (4)
        JR   NZ,L11B4     ; (12/7)

        NOP               ; (4)
        NOP               ; (4)
        NOP               ; (4)
        NOP               ; (4)

        LD   A,L          ; (4) Retrieve the byte to send.

        LD   D,#0x08        ; (7) There are 8 bits to send.

L11BE:  RRA               ; (4) Rotate the next bit to send into the carry.
        LD   L,A          ; (4) Store the remaining bits.
        JP   NC,L11C9     ; (10) Jump if it is a 0 bit.

        LD   A,#0xFE        ; (7) Set RS232 'RXD' transmit line to 1. (Keep KEYPAD 'CTS' output line low to prevent the keypad resetting)
        OUT  (C),A        ; (11)
        JR   L11CF        ; (12) Jump forward to process the next bit.

L11C9:  LD   A,#0xFA        ; (7) Set RS232 'RXD' transmit line to 0. (Keep KEYPAD 'CTS' output line low to prevent the keypad resetting)
        OUT  (C),A        ; (11)
        JR   L11CF        ; (12) Jump forward to process the next bit.

L11CF:  LD   E,#0x02        ; (7) Introduce delays such that the next data bit is output 113 T-states from now.

L11D1:  DEC  E            ; (4)
        JR   NZ,L11D1     ; (12/7)

        NOP               ; (4)
        ADD  A,#0x00        ; (7)

        LD   A,L          ; (4) Retrieve the remaining bits to send.
        DEC  D            ; (4) Decrement the bit counter.
        JR   NZ,L11BE     ; (12/7) Jump back if there are further bits to send.

        NOP               ; (4) Introduce delays such that the stop bit is output 113 T-states from now.
        NOP               ; (4)
        ADD  A,#0x00        ; (7)
        NOP               ; (4)
        NOP               ; (4)

        LD   A,#0xFE        ; (7) Set RS232 'RXD' transmit line to 1. (Keep KEYPAD 'CTS' output line low to prevent the keypad resetting)
        OUT  (C),A        ; (11) Send out the STOP bit.

        LD   E,#0x06        ; (7) Delay for 101 T-states (28.5us).

L11E7:  DEC  E            ; (4)
        JR   NZ,L11E7     ; (12/7)
        RET
__endasm;
}

////////////////////////////////////////////////////////////////////////////////

// Copia desde una posición de memoria, los bytes que forman el nombre de un fichero hasta encontrar un espacio,
// un retorno de linea, o el simbolo de los dos puntos ":" para indicar fin de una sentencia. Se usa en ESXDOS
void getfilename (char *p, char *fname)
{
    while (*p!=':' && *p!=0xd && *p!=' ')
          *fname++ = *p++;
    *fname = '\0';
    return;
}


/* --------------------------------------------------------------------------------- */
/* --------------------------------------------------------------------------------- */
/* --------------------------------------------------------------------------------- */
// RUTINAS ACCESORIAS QUE SIEMPRE TENGO EN MIS PROGRAMAS ESXDOS PARA LO QUE HAGA FALTA.
// Creo que el compilador no las incluye en el binario final si no se usan.

#pragma disable_warning 85
#pragma disable_warning 59

void puts (BYTE *str) STACKARGS
{
  __asm
  push bc
  push de
  ld a,(#ATTRT)
  push af
  ld a,(#ATTRP)
  ld (#ATTRT),a
  ld l,4(ix)
  ld h,5(ix)
buc_print:
  ld a,(hl)
  or a
  jp z,fin_print
  cp #4
  jr nz,no_attr
  inc hl
  ld a,(hl)
  ld (#ATTRT),a
  inc hl
  jr buc_print
no_attr:
  rst #16
  inc hl
  jp buc_print

fin_print:
  pop af
  ld (#ATTRT),a
  pop de
  pop bc
  __endasm;
}

void u16tohex (WORD n, char *s)
{
  u8tohex((n>>8)&0xFF,s);
  u8tohex(n&0xFF,s+2);
}

void u8tohex (BYTE n, char *s)
{
  BYTE i=1;
  BYTE resto;

  resto=n&0xF;
  s[1]=(resto>9)?resto+55:resto+48;
  resto=n>>4;
  s[0]=(resto>9)?resto+55:resto+48;
  s[2]='\0';
}

void print8bhex (BYTE n)
{
    char s[3];

    u8tohex(n,s);
    puts(s);
}

void print16bhex (WORD n)
{
    char s[5];

    u16tohex(n,s);
    puts(s);
}

// Codigo necesario como prologo de cualquier función C. No se aplica a las __naked.
// Como no estoy incluyendo las librerias estándar ni el crt0 estándar, he de ponerla aqui
void __sdcc_enter_ix (void) __naked
{
    __asm
    pop	hl	; return address
    push ix	; save frame pointer
    ld ix,#0
    add	ix,sp	; set ix to the stack frame
    jp (hl)	; and return
    __endasm;
}

///////////////////////////////////////////////////////////////////////////////////////////
// RUTINAS ESXDOS (sólo las que necesito en este programa, por lo que no está write)
///////////////////////////////////////////////////////////////////////////////////////////

BYTE open (char *filename, BYTE mode) STACKARGS
{
    __asm
    push bc
    push de
    xor a
    rst #8
    .db #M_GETSETDRV   ;Default drive in A
    ld l,4(ix)  ;Filename pointer
    ld h,5(ix)  ;in HL
    ld b,6(ix)  ;Open mode in B
    rst #8
    .db #F_OPEN
    jr nc,open_ok
    ld (#_errno),a
    ld a,#0xff
open_ok:
    ld l,a
    pop de
    pop bc
    __endasm;
}

void close (BYTE handle) STACKARGS
{
    __asm
    push bc
    push de
    ld a,4(ix)  ;Handle
    rst #8
    .db #F_CLOSE
    pop de
    pop bc
    __endasm;
}

// Posiciona el puntero de lectura del fichero en un offset absoluto desde el principio.
// El esxdos original espera el modo de seek en IXL; el API compatible de NextZXOS lo
// espera en L. Ponemos 0 (SEEK_START) en ambos para funcionar en los dos.
void seekset (BYTE handle, DWORD offset) STACKARGS
{
    __asm
    push bc
    push de
    ld a,4(ix)  ;File handle in A
    ld e,5(ix)
    ld d,6(ix)
    ld c,7(ix)
    ld b,8(ix)  ;Offset in BCDE (B=MSB, E=LSB)
    push ix
    ld ix,#0    ;IXL=0: seek from start (esxdos)
    ld l,#0     ;L=0: seek from start (NextZXOS)
    rst #8
    .db #F_SEEK
    pop ix
    jr nc,seek_ok
    ld (#_errno),a
seek_ok:
    pop de
    pop bc
    __endasm;
}

WORD read (BYTE handle, BYTE *buffer, WORD nbytes) STACKARGS
{
    __asm
    push bc
    push de
    ld a,4(ix)  ;File handle in A
    ld l,5(ix)  ;Buffer address
    ld h,6(ix)  ;in HL
    ld c,7(ix)
    ld b,8(ix)  ;Buffer length in BC
    rst #8
    .db #F_READ
    jr nc,read_ok
    ld (#_errno),a
    ld bc,#65535
read_ok:
    ld h,b
    ld l,c
    pop de
    pop bc
    __endasm;
}
