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
sdcc -mz80 --reserve-regs-iy --opt-code-size --max-allocs-per-node 100000 ^
--nostdlib --nostdinc --no-std-crt0 --code-loc 0x2000 --data-loc 0x2dc0 playmid.c z80.lib -L "C:\Program Files\SDCC\lib\z80"
makebin -s 65535 -p playmid.ihx playmid.bin
dd if=playmid.bin of=PLAYMID bs=1 skip=8192

Con SDCC 4.2 o superior, compilar con la convencion de llamada por defecto (NO usar --sdcccall 0:
z80.lib viene compilada con la convencion por registros y las rutinas de multiplicacion/division
recibirian basura; las funciones con ensamblador incrustado ya van marcadas con __sdcccall(0)):
sdcc -mz80 --reserve-regs-iy --opt-code-size --max-allocs-per-node 100000 \
--nostdlib --nostdinc --no-std-crt0 --code-loc 0x2000 --data-loc 0x2dc0 playmid.c z80.lib -L /path/to/sdcc/lib/z80

OJO con --data-loc. Si el código de este programa crece, habría que mover --data-loc adecuadamente para que no se
solapen codigo y datos. Comprobar en el .map que _CODE+codigo de librerias termina antes de --data-loc, y que
DATA termina antes de 0x3000 (donde empieza el buffer).

MAPA DE MEMORIA (DivMMC RAM, 0x2000-0x3FFF):
  0x2000-0x2C2F : codigo + literales
  0x2C30-0x2FFF : datos (variables globales, estado de pistas) + codigo de libreria (_HOME)
  0x3000-0x33FF : buffer de 1KB: staging de salida, scratch de cabeceras y cachés
                  de lectura repartidas entre las pistas
  0x3400-0x3FFF : NO TOCAR. Lanzadores de comandos como el LNF Browser guardan aqui su
                  propio estado; escribir en esta zona cuelga o resetea al volver.
  La pantalla tampoco se toca: usarla de caché provocaba un reset al salir cuando el
  comando se lanzaba desde el LNF Browser.

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

#define BANKM 23388   // copia en RAM del ultimo valor escrito en el puerto 0x7FFD (es de solo escritura)
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

BYTE i, c;     // contadores de bucle, etc.
BYTE param1;   // tipo de metaevento
WORD ppq;                  // pulsos por negra, de la cabecera (cabe en 16 bits)
DWORD ticks_per_int;       // ticks de reloj MIDI por interrupcion, escalado por PRECISION
__at(0x3000) BYTE buffer[1024];  // Staging para la cabecera y los eventos MIDI salientes. No moverlo de aqui sin tocar SendMIDI
WORD lbytes;             // longitud de metaeventos y sysex
DWORD us_per_quarter;    // ultimo tempo leido con el metaevento Set Tempo

// ---- Soporte para MIDI formato 1 (multipista) ----
// Todo vive DENTRO del buffer de 1KB en 0x3000: los primeros 64 bytes son el staging
// de salida (y scratch de cabeceras), y los 960 restantes se reparten como cachés de
// lectura entre las pistas. OJO: no usar ni la pantalla ni memoria por encima de
// 0x33FF: los lanzadores de comandos (LNF Browser) guardan su estado en la parte alta
// de la pagina DivMMC, y tocar la pantalla provoco resets al volver al lanzador.
// Disposicion del buffer de 0x3000 durante la reproduccion:
//   0x3000-0x302F  staging de salida (48 bytes; los sysex se trocean a 48)
//   0x3030-0x303B  rutina de interrupcion IM2 (copiada aqui en tiempo de ejecucion)
//   0x303C-0x30FF  staging de las recargas L2 (196 bytes por tanda)
//   0x3100-0x3201  tabla de vectores IM2 (257 bytes de 0x30 -> handler en 0x3030)
//   0x3202-0x33FF  cachés de lectura L1 de las pistas
#define MAX_TRACKS   17                   // pista de tempo + 16 canales: el maximo de un format 1 tipico
#define TCACHE_TOTAL 510                  // bytes de buffer disponibles para cachés L1
#define tcaches      (buffer+0x202)       // las cachés van tras la tabla de vectores IM2
#define L2STAGE      (buffer+0x3C)        // staging de recargas L2 (hueco tras la ISR)
#define L2STAGE_SIZE 196

BYTE fhandle;                    // handle del fichero, global para poder recargar cachés desde cualquier rutina

// ---- Caché L2 en los bancos de RAM del 128K ----
// El acceso a la SD (F_SEEK con recorrido de la FAT + F_READ) cuesta milisegundos y
// hecho una vez por cada recarga pequeña de caché se oye como micro-tirones. Por eso
// cada pista tiene una ventana L2 en los bancos 1/3/4/6 (64KB en total, repartidos),
// que se rellena desde la SD en tandas grandes y secuenciales (un solo seek por
// ventana), y las recargas de la caché L1 del buffer pasan a ser copias de RAM.
// OJO: esto sacrifica el RAM-disc de 128 BASIC. Requiere paginado disponible.
// Mientras el nucleo de esxdos trabaja, pagina su propio banco sobre 0x2000-0x3FFF y
// nuestra tabla de vectores IM2 desaparece: una interrupcion en modo IM2 en ese
// momento saltaria a un vector basura. Prohibir las interrupciones del todo (DI)
// tampoco vale: el nucleo puede necesitarlas y la maquina se quedaria colgada dentro
// de la llamada. Solucion: desmontar POR COMPLETO nuestro modo IM2 (incluido I=0x3F)
// alrededor de cada recarga L2, de forma que esxdos trabaje siempre en el estado
// estandar de la maquina, identico al del reproductor original. Reconstruir la tabla
// al volver cuesta ~1ms y las recargas L2 son raras (una por ventana de varios KB).
void bankxfer (BYTE bank, WORD off, BYTE *p, WORD n, BYTE wr) STACKARGS;
void bankmove (WORD woff, BYTE *p, WORD n, BYTE wr);

DWORD muldw (DWORD a, WORD b) STACKARGS;
void settempo (void);
BYTE banknum (BYTE idx);
// ---- Reloj por contador de interrupciones (mecanismo tomado de ZMP) ----
// Contar HALTs pierde tiempo: mientras se procesa un evento o se envian bytes por el
// MIDI pasan frames que el reloj no ve, y en los pasajes densos la musica se arrastra.
// En su lugar instalamos un handler IM2 minusculo que incrementa un contador en CADA
// interrupcion; el bucle de espera se pone al dia con todos los frames transcurridos
// (y se salta los HALT sobrantes), igual que hace zx-midiplayer.
volatile BYTE int_cnt;           // incrementado por la ISR de IM2
BYTE cnt_last;                   // ultimo valor consumido por el reloj
BYTE im2_active;                 // a 1 mientras nuestro modo IM2 esta instalado
void im2_on (void);
void im2_off (void);

BYTE main (char *p) STACKARGS;
BYTE commandlinemode (char *p);

void __sdcc_enter_ix (void) __naked;

#ifdef DEBUG_UTILS
void puts (BYTE *) STACKARGS;
void u16tohex (WORD n, char *s);
void u8tohex (BYTE n, char *s);
void print8bhex (BYTE n);
void print16bhex (WORD n);
#endif

BYTE open (char *filename, BYTE mode) STACKARGS;
void close (BYTE handle) STACKARGS;
WORD read (BYTE handle, BYTE *buffer, WORD nbytes) STACKARGS;
void seekset (BYTE handle, DWORD offset) STACKARGS;

/* --------------------------------------------------------------------------------- */
/* --------------------------------------------------------------------------------- */
/* --------------------------------------------------------------------------------- */
void getfilename (char *p, char *fname);
void playmidi (BYTE f);
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
  BYTE res;

  // Algunos lanzadores (p.ej. el LNF Browser) pueden pasarnos el control con las
  // interrupciones deshabilitadas, y el primer HALT colgaria la maquina para siempre.
  __asm
  ei
  __endasm;

  // Dejamos el puerto MIDI inactivo
  AYREGSELECT = 0x0e;
  AYREGWRITE = 0xfe;

  if (!p)
     return 0;
  res = commandlinemode(p);

  // Apagamos todo en todos los canales: si salimos a mitad de melodía quedan notas
  // sonando, y no todos los sintetizadores hacen caso al reset FF. El mensaje se monta
  // en buffer porque SendMIDI solo puede enviar desde la zona 0x3000-0x33FF.
  buffer[1] = 0x78;            // All Sound Off
  buffer[2] = 0;
  buffer[3] = 0x7B;            // All Notes Off (por running status, sin repetir el estado)
  buffer[4] = 0;
  for (i=0;i<16;i++)
  {
      buffer[0] = 0xB0 | i;
      SendMIDI (buffer, 5);
  }

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

////////////////////////////////////////////////////////////////////////////////

// Multiplica un DWORD por un WORD (suma y desplazamiento clasicos). Evita arrastrar
// __mullong (la rutina generica de 32x32 de la libreria ocupa 272 bytes) y compila
// mucho mas compacto en ensamblador. Solo se usa al cambiar el tempo.
DWORD muldw (DWORD a, WORD b) STACKARGS
{
    __asm
    push bc
    ld hl,#0
    ld d,h
    ld e,l          ;DEHL = resultado = 0
    ld c,8(ix)
    ld b,9(ix)      ;BC = b
mul_loop:
    ld a,b
    or c
    jr z,mul_done
    srl b
    rr c            ;bit 0 de b pasa al carry
    jr nc,mul_shift
    ld a,l          ;DEHL += a (la copia de a vive en el frame, 4..7(ix))
    add a,4(ix)
    ld l,a
    ld a,h
    adc a,5(ix)
    ld h,a
    ld a,e
    adc a,6(ix)
    ld e,a
    ld a,d
    adc a,7(ix)
    ld d,a
mul_shift:
    sla 4(ix)       ;a <<= 1
    rl 5(ix)
    rl 6(ix)
    rl 7(ix)
    jr mul_loop
mul_done:
    pop bc
    __endasm;
}

// Recalcula ticks_per_int a partir del tempo actual.
// ticks_per_int = PRECISION * 20000 * ppq / us_per_quarter, y PRECISION*20000 = 1280000
void settempo (void)
{
    if (us_per_quarter)
        ticks_per_int = muldw (1280000UL, ppq) / us_per_quarter;
}

// Numero de banco 128K para cada 16KB del fichero: 1, 3, 4, 6 (los libres con BASIC
// quieto). OJO: JAMAS los bancos 5 (¡es la pantalla!), 2, 0 ni 7 (pantalla sombra).
BYTE banknum (BYTE idx)
{
    return (idx << 1) | (idx < 2);   // 0,1,2,3 -> 1,3,4,6
}

// Instala el reloj IM2: construye la ISR en 0x3030 (dentro del buffer), rellena la
// tabla de vectores en 0x3100 con 0x30 (cualquier byte del bus da vector 0x3030), y
// activa IM2 con I=0x31. Todo vive en la pagina DivMMC, que esta siempre mapeada
// mientras ejecutamos.
void im2_on (void)
{
    BYTE *p;

    p = buffer + 0x30;
    *p++ = 0xF5;                                        // push af
    *p++ = 0x3A; *(WORD *)p = (WORD)&int_cnt; p += 2;   // ld a,(int_cnt)
    *p++ = 0x3C;                                        // inc a
    *p++ = 0x32; *(WORD *)p = (WORD)&int_cnt; p += 2;   // ld (int_cnt),a
    *p++ = 0xF1;                                        // pop af
    *p++ = 0xFB;                                        // ei
    *p++ = 0xED; *p = 0x4D;                             // reti
    for (p = buffer + 0x100; p != buffer + 0x202; p++)
        *p = 0x30;
    im2_active = 1;
    __asm
    di
    ld a,#0x31
    ld i,a
    im 2
    ei
    __endasm;
}

// Restaura el modo de interrupciones estandar del Spectrum (IM1, I=0x3F)
void im2_off (void)
{
    im2_active = 0;
    __asm
    di
    im 1
    ld a,#0x3F
    ld i,a
    ei
    __endasm;
}

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
// Estado de la pista activa espejado en globales: acceder a arrays DWORD indexados
// en cada byte/recarga es caro en el Z80, asi que set_curtrk carga aqui el estado y
// lo guarda de vuelta al cambiar de pista.
DWORD cur_off;                 // offset en el fichero de la proxima recarga L1
DWORD cur_l2end;               // offset de fichero donde termina la ventana L2
WORD cur_l2bank;               // offset en los bancos del proximo byte L1
BYTE *cptr;                    // puntero a la caché de curtrk
WORD tcsize;                   // tamaño de la caché de cada pista (TCACHE_TOTAL/tracks)
DWORD trk_off[MAX_TRACKS];     // offset en el fichero de la proxima recarga de caché L1
DWORD l2_end[MAX_TRACKS];      // offset de fichero donde termina la ventana L2 de la pista
WORD l2_bank[MAX_TRACKS];      // offset dentro de los bancos del proximo byte L1 a copiar
BYTE l2_eof[MAX_TRACKS];       // a 1 si la ventana ya llega hasta el final del fichero
WORD l2_area;                  // bytes de banco reservados a cada pista
DWORD trk_next[MAX_TRACKS];    // tick absoluto (escalado por PRECISION) del proximo evento
BYTE trk_status[MAX_TRACKS];   // running status propio de cada pista (imprescindible al mezclar)
BYTE trk_end[MAX_TRACKS];      // a 1 cuando la pista ha terminado (FF 2F o EOF)
BYTE trk_cpos[MAX_TRACKS];     // posicion de lectura dentro de la caché (tcsize <= 255)
BYTE trk_clen[MAX_TRACKS];     // bytes válidos en la caché
DWORD now;                     // ticks transcurridos desde el principio (escalado por PRECISION)
BYTE wire_status;              // ultimo byte de estado enviado por el cable (0 = ninguno), para running status de salida
DWORD *pnext;                  // puntero para recorrer trk_next sin indexar
BYTE trkn;                     // indice de pista del barrido (OJO: trk_event machaca la global i)
BYTE fired;                    // a 1 si en este frame ha sonado algun evento
BYTE pick;                     // pista elegida para la precarga en frames vacios
WORD rem, remt;                // bytes restantes de ventana L2 (solo palabra baja: sobra)
BYTE *rdptr, *rdend;           // ventana de lectura de la pista activa: evita indexar arrays en cada byte

// OJO: el contador FRAMES (23672) NO sirve de reloj: mientras se ejecuta un comando
// punto, el manejador de 0x38 de la EPROM de esxdos no encadena con la ISR de la ROM
// y las variables del sistema quedan congeladas. Por eso contamos las interrupciones
// nosotros mismos con la ISR de IM2 (y el teclado se lee de los puertos, no de LASTK).

// Selecciona la pista activa: guarda la ventana de lectura y el estado de la anterior
// y carga los de la nueva en las globales espejo
void set_curtrk (BYTE t)
{
    if (t == curtrk)
        return;
    if (curtrk != 0xFF)
    {
        trk_cpos[curtrk] = rdptr - cptr;
        trk_clen[curtrk] = rdend - cptr;
        trk_off[curtrk] = cur_off;
        l2_end[curtrk] = cur_l2end;
        l2_bank[curtrk] = cur_l2bank;
    }
    curtrk = t;
    cptr = tcaches + (WORD)t * tcsize;
    rdptr = cptr + trk_cpos[t];
    rdend = cptr + trk_clen[t];
    cur_off = trk_off[t];
    cur_l2end = l2_end[t];
    cur_l2bank = l2_bank[t];
}

// Rellena desde la SD la ventana L2 (en los bancos) de la pista activa, con un solo
// seek y lecturas secuenciales grandes a traves del staging L2.
void l2_refill (void)
{
    WORD base, got, chunk, n;
    BYTE was;

    base = (WORD)curtrk * l2_area;
    got = 0;
    was = im2_active;
    if (was)
        im2_off ();        // esxdos trabaja en el estado estandar de interrupciones
    seekset (fhandle, cur_off);
    while (got < l2_area)
    {
        chunk = l2_area - got;
        if (chunk > L2STAGE_SIZE)
            chunk = L2STAGE_SIZE;
        n = read (fhandle, L2STAGE, chunk);
        if (n == 0xFFFF)
            n = 0;
        if (n)
        {
            bankmove (base + got, L2STAGE, n, 1);
            got += n;
        }
        if (n < chunk)
            break;                        // EOF
    }
    if (was)
        im2_on ();
    cur_l2bank = base;
    cur_l2end = cur_off + got;
    l2_eof[curtrk] = (got < l2_area);   // la ventana toca EOF: no hay nada mas que precargar
}

// Recarga la caché L1 de la pista activa desde su ventana L2 (copia de RAM). Si la
// ventana esta agotada, se rellena antes desde la SD. Sin datos -> fin de pista.
void trk_refill (void)
{
    WORD n;

    if (cur_off >= cur_l2end)
        l2_refill ();
    n = (WORD)(cur_l2end - cur_off);
    if (n > tcsize)
        n = tcsize;
    if (n == 0)
        trk_end[curtrk] = 1;
    else
    {
        bankmove (cur_l2bank, cptr, n, 0);
        cur_l2bank += n;
        cur_off += n;
    }
    rdptr = cptr;
    rdend = cptr + n;
}

// Lee y consume el siguiente byte de la pista activa
BYTE trk_get (void)
{
    if (rdptr == rdend)
    {
        trk_refill();
        if (trk_end[curtrk])
            return 0;
    }
    return *rdptr++;
}

// Lee una cantidad de longitud variable (delta o longitud de metaevento/sysex).
// El caso comun (un solo byte) no hace ningun desplazamiento de 32 bits.
DWORD trk_varlen (void)
{
    DWORD v;

    c = trk_get();
    v = c & 0x7F;
    while (c & 0x80)
    {
        c = trk_get();
        v = (v<<7) | (c & 0x7F);
    }
    return v;
}

// Procesa un evento de la pista activa (el delta ya se consumió antes).
// A diferencia del reproductor de formato 0, aqui SIEMPRE se envia el byte de
// estado: el running status de la linea MIDI se rompe al intercalar pistas.
void trk_event (void)
{
    BYTE st, n, b;

    b = trk_get();
    if (b & 0x80)          // byte de estado nuevo
    {
        st = b;
        if (st < 0xF0)     // solo los estados de canal actualizan el running status:
            trk_status[curtrk] = st;   // sysex y metaeventos no lo cancelan (SMF es asi de laxo)
        b = 0xFF;          // señal: el primer byte de datos aun no se ha leido
    }
    else
        st = trk_status[curtrk];   // running status: b ya es el primer byte de datos

    // EVENTOS F0 y F7 (SYSEX). Se envian por trozos usando el buffer como staging.
    // Un sysex cancela el running status del cable.
    if (st == 0xF0 || st == 0xF7)
    {
        wire_status = 0;
        lbytes = trk_varlen();
        if (st == 0xF0)
        {
            buffer[0] = 0xF0;
            SendMIDI (buffer, 1);
        }
        while (lbytes)
        {
            n = (lbytes > 48) ? 48 : lbytes;   // el staging son los primeros 48 bytes del buffer
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
        param1 = trk_get();     // aqui b siempre es 0xFF: FF nunca llega por running status
        lbytes = trk_varlen();
        if (param1 == 0x2F)          // fin de pista
        {
            trk_end[curtrk] = 1;
            return;
        }
        if (param1 == 0x51 && lbytes == 3)   // Set Tempo: se aplica globalmente
        {
            ((BYTE *)&us_per_quarter)[3] = 0;
            ((BYTE *)&us_per_quarter)[2] = trk_get();
            ((BYTE *)&us_per_quarter)[1] = trk_get();
            ((BYTE *)&us_per_quarter)[0] = trk_get();
            settempo ();
            return;
        }
        while (lbytes--)             // el resto de metaeventos se ignora
            trk_get();
        return;
    }

    // EVENTOS de canal: estado + 1 o 2 bytes de datos, via staging. Si el estado
    // coincide con el último enviado por el cable, se omite (running status de salida):
    // a 31250 baudios cada byte ahorrado son 320us, y en los pasajes densos se nota.
    c = st & 0xF0;
    n = 0;
    if (st != wire_status)
    {
        wire_status = st;
        buffer[n++] = st;
    }
    buffer[n++] = (b != 0xFF) ? b : trk_get();
    if (c != 0xC0 && c != 0xD0)
        buffer[n++] = trk_get();
    SendMIDI (buffer, n);
}

// Frame sin eventos: precarga por adelantado la ventana L2 mas gastada, para que
// las recargas de la SD caigan en los huecos de la musica y no encima de los
// pasajes (las ventanas de todas las pistas se llenan a la vez al principio y se
// agotan tambien mas o menos a la vez). El resto de ventana cabe en 16 bits.
void l2_prefetch (void)
{
    rem = tcsize << 1;             // umbral: menos de dos recargas L1 restantes
    pick = 0xFF;
    trk_off[curtrk] = cur_off;     // sincronizamos los espejos para poder comparar
    l2_end[curtrk] = cur_l2end;
    pnext = l2_end;
    for (trkn = 0; trkn < tracks; trkn++, pnext++)
    {
        if (trk_end[trkn] || l2_eof[trkn])
            continue;
        remt = *(WORD *)pnext - *(WORD *)(trk_off + trkn);
        if (remt < rem)
        {
            rem = remt;
            pick = trkn;
        }
    }
    if (pick != 0xFF)
    {
        set_curtrk (pick);
        l2_refill ();
    }
}

// Bucle principal de reproduccion.
// ntrk es el numero de pistas que declara la cabecera MThd.
// Devuelve 0 si se ha reproducido, 1 si no se encontro ninguna pista MTrk.
// Recorre los chunks del fichero construyendo la tabla de offsets de comienzo de
// cada pista. La longitud de cada chunk está en su cabecera.
void scan_tracks (BYTE ntrk)
{
    DWORD len, fpos;

    tracks = 0;
    fpos = 14;
    while (tracks < ntrk && tracks < MAX_TRACKS)
    {
        seekset (fhandle, fpos);
        if (read (fhandle, buffer, 8) != 8)
            break;
        // longitud del chunk (big-endian en el fichero), compuesta byte a byte:
        // mucho mas compacto que desplazamientos de 32 bits (somos little-endian)
        ((BYTE *)&len)[0] = buffer[7];
        ((BYTE *)&len)[1] = buffer[6];
        ((BYTE *)&len)[2] = buffer[5];
        ((BYTE *)&len)[3] = buffer[4];
        fpos += 8;
        if (((WORD *)buffer)[0] == 0x544D && ((WORD *)buffer)[1] == 0x6B72)   // "MTrk"
        {
            trk_off[tracks] = fpos;
            l2_end[tracks] = fpos;         // ventana L2 vacia: el primer uso la rellena
            l2_eof[tracks] = 0;
            trk_status[tracks] = 0;
            trk_end[tracks] = 0;
            trk_cpos[tracks] = 0;
            trk_clen[tracks] = 0;
            tracks++;
        }
        fpos += len;    // chunks desconocidos se saltan sin contarlos
    }
}

BYTE playmidi1 (BYTE ntrk)
{
    BYTE best;

    scan_tracks (ntrk);
    if (tracks == 0)
        return 1;       // el que llama imprime el error

    // Cuantas menos pistas, mas caché por pista (tope: 255, trk_cpos/clen son BYTE)
    tcsize = (WORD)TCACHE_TOTAL / (WORD)tracks;
    if (tcsize > 255)
        tcsize = 255;
    l2_area = 0xFFFF / tracks;             // reparto de los 64KB de bancos entre pistas

    // Leemos el primer delta de cada pista para inicializar su next_tick.
    // Una pista terminada se marca con next_tick = 0xFFFFFFFF (centinela): asi el
    // planificador no necesita consultar trk_end.
    curtrk = 0xFF;
    best = 0;                                  // contador de pistas vivas
    for (trkn = 0; trkn < tracks; trkn++)
    {
        set_curtrk (trkn);
        trk_next[trkn] = trk_varlen() << 6;    // <<6 == * PRECISION
        if (trk_end[trkn])
            trk_next[trkn] = 0xFFFFFFFF;
        else
            best++;
    }
    if (best == 0)
        return 0;

    // Planificador al estilo de zx-midiplayer: una pasada por TODAS las pistas en
    // cada frame ("¿te toca ya?": una sola comparacion por pista), y la pista que
    // esta al dia vacia de golpe todos sus eventos pendientes hasta la siguiente
    // pausa. Nada de buscar el tick minimo ni de reordenar: en los acordes densos
    // el coste por evento se queda en el parseo y el cable, no en el planificador.
    now = 0;
    wire_status = 0;
    im2_on ();         // desde aqui el reloj lo lleva la ISR
    cnt_last = int_cnt;
    while (1)
    {
        // Si pulsamos SPACE, salir
        if ((SEMIFILA8 & 0x1) == 0)
            return 0;

        // Un frame por vuelta: si la ISR no ha contado ninguno pendiente, dormimos.
        // Si vamos con retraso (un pasaje denso tardo mas de un frame), se procesan
        // vueltas seguidas sin dormir hasta ponerse al dia.
        if (cnt_last == int_cnt)
            WAIT_VRETRACE;
        cnt_last++;
        now += ticks_per_int;

        fired = 0;
        pnext = trk_next;
        for (trkn = 0; trkn < tracks; trkn++, pnext++)
        {
            if (*pnext > now)          // aun no le toca (el centinela de pista
                continue;              // terminada, 0xFFFFFFFF, nunca "toca")
            fired = 1;
            set_curtrk (trkn);
            do
            {
                trk_event ();
                if (trk_end[trkn])
                {
                    *pnext = 0xFFFFFFFF;
                    if (--best == 0)   // no quedan pistas vivas
                        return 0;
                    break;
                }
                *pnext += trk_varlen() << 6;    // <<6 == * PRECISION
            }
            while (*pnext <= now);
        }

        // Frame sin eventos: aprovechamos el hueco para precargar la SD
        if (!fired)
            l2_prefetch ();
    }
}

/* ============================ FORMAT1 ENGINE END ============================ */

// Rutina principal de reproducción MIDI. Analiza la cabecera y lanza el motor
// de mezcla, que reproduce tanto formato 1 como formato 0 (caso trivial de una
// sola pista, con toda la caché para ella, asi que las lecturas son secuenciales).
void playmidi (BYTE f)
{
    BYTE res;

    // Leemos la cabecera MIDI (14 bytes)
    read (f, buffer, 14);

    // Comprobamos que realmente es una cabecera MIDI, y si no, retornamos con error
    // Fichero invalido -> borde azul y salir (nada de RST 16: bajo el LNF Browser
    // el canal de pantalla no es valido e imprimir resetea la maquina)
    if (((WORD *)buffer)[0] != 0x544D || ((WORD *)buffer)[1] != 0x6468   // "MThd"
        || buffer[9] > 1)
    {
        ULA = 1;
        return;
    }

    // Leemos el PPQ (partes por quarter, o el numero de ticks del reloj de MIDI que dura una negra
    ppq = ((WORD)buffer[12]<<8) | buffer[13];

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
        ticks_per_int = muldw ((DWORD)(buffer[12] * buffer[13]), 20 * PRECISION);
    }
    else  // habitualmente los MIDs lo calculan de esta otra forma, es decir, habitualmente el bit 7 del byte 12 es 0.
    {
        us_per_quarter = 500000;
        settempo ();
    }

    fhandle = f;
    im2_active = 0;   // sin crt0 las globales arrancan con basura: inicializar explicitamente

    // Borde verde mientras suena la musica. OJO: nada de imprimir por RST 16 en el
    // camino de exito: bajo lanzadores como el LNF Browser el canal de pantalla de
    // BASIC no es valido y la maquina se resetea.
    ULA = 4;

    res = playmidi1 (buffer[10] ? MAX_TRACKS : buffer[11]);   // numero de pistas de la cabecera (topado a MAX_TRACKS)
    im2_off ();     // restauramos IM1 e I=0x3F antes de volver al sistema

    if (res)
        ULA = 1;    // sin pistas MTrk: borde azul
    else
        ULA = (*(BYTE *)BORDR >> 3) & 7;   // borde original (bits 3-5 de BORDCR)
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

#ifdef DEBUG_UTILS
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
#endif

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

// Transfiere n bytes (n>0) entre RAM normal y un banco de 128K paginado en 0xC000.
// wr=0: banco->p (lectura), wr=1: p->banco (escritura). Se ejecuta con interrupciones
// deshabilitadas y sin usar la pila mientras el banco ajeno esta paginado, porque en
// 0xC000-0xFFFF puede vivir la pila de BASIC. El puerto 0x7FFD es de solo escritura,
// asi que el valor a restaurar sale de su copia en BANKM (23388).
void bankxfer (BYTE bank, WORD off, BYTE *p, WORD n, BYTE wr) STACKARGS
{
    __asm
    push bc
    push de
    ;OJO: la pila (y el frame IX) de BASIC pueden estar en 0xC000-0xFFFF, es decir,
    ;dentro de la ventana que vamos a paginar. Hay que leer TODOS los parametros y
    ;dejar la pila en paz ANTES de tocar el puerto, y no usarla hasta restaurarlo.
    ld a,6(ix)
    or #0xC0
    ld h,a
    ld l,5(ix)      ;HL = 0xC000 + off (lado banco)
    ld e,7(ix)
    ld d,8(ix)      ;DE = p (lado RAM normal)
    ld a,11(ix)     ;wr?
    or a
    jr z,bkx_rd
    ex de,hl        ;escritura: origen p, destino banco
bkx_rd:
    ld c,9(ix)
    ld b,10(ix)     ;BC = n
    ld a,(#23388)   ;BANKM
    and #0xF8
    or 4(ix)        ;banco pedido en los bits 0-2
    di
    exx
    push bc         ;salvamos BC alt (la pila aun es la normal: no hemos paginado)
    ld bc,#0x7ffd
    out (c),a       ;banco ajeno paginado: desde aqui ni pila ni frame
    exx
    ldir            ;la copia no usa la pila
    ld a,(#23388)
    exx
    out (c),a       ;restauramos el paginado original (BC alt sigue siendo 0x7ffd)
    pop bc          ;recuperamos BC alt (la pila vuelve a ser visible)
    exx
    ei
    pop de
    pop bc
    __endasm;
}

// Copia n bytes entre el fichero precargado (offset woff) y p, partiendo la copia
// en las fronteras de 16KB entre bancos.
void bankmove (WORD woff, BYTE *p, WORD n, BYTE wr)
{
    WORD chunk;

    while (n)
    {
        chunk = 0x4000 - (woff & 0x3FFF);
        if (chunk > n)
            chunk = n;
        bankxfer (banknum((BYTE)(woff >> 14)), woff & 0x3FFF, p, chunk, wr);
        woff += chunk;
        p += chunk;
        n -= chunk;
    }
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
