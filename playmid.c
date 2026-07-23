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
Compilar con SDCC 4.x, con la convencion de llamada por defecto (NO usar --sdcccall 0:
z80.lib viene compilada con la convencion por registros y las rutinas de multiplicacion/division
recibirian basura; las funciones con ensamblador incrustado ya van marcadas con __sdcccall(0)):
sdcc -mz80 --reserve-regs-iy --opt-code-size --max-allocs-per-node 100000 \
--nostdlib --nostdinc --no-std-crt0 --code-loc 0x2000 --data-loc 0x2eb0 playmid.c z80.lib -L /path/to/sdcc/lib/z80
makebin -s 65535 -p playmid.ihx playmid.bin
dd if=playmid.bin of=PLAYMID bs=1 skip=8192

OJO con --data-loc. Si el código de este programa crece, habría que mover --data-loc adecuadamente para que no se
solapen codigo y datos. Comprobar en el .map que _CODE+codigo de librerias termina antes de --data-loc, y que
_HOME termina antes de 0x3000 (donde empieza el buffer). El presupuesto esta apurado al limite: parte de los
escalares globales vive dentro del buffer (declaraciones __at mas abajo) para liberar sitio en 0x2000-0x2FFF.

MAPA DE MEMORIA (DivMMC RAM, 0x2000-0x3FFF):
  0x2000-0x2EAF : codigo + literales
  0x2EB0-0x2FFF : datos (variables globales, estado de pistas) + codigo de libreria (_HOME)
  0x3000-0x33FF : buffer de 1KB: staging de salida, ISR y tabla IM2, staging L2,
                  escalares globales y cachés de lectura repartidas entre las pistas
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

// (errno lives in the buffer page: see the __at declarations below)

// Esta precisión la he elegido suponiendo que ppq nunca será mayor en la práctica de 2048, para que no 
// desborde en 32 bits al calcular el valor de ticks_per_int en un evento FF 03 58
#define PRECISION 64

// variables globales en lugar de locales para agilizar su lectura, y no depender de direccionamiento indexado
// que engordaría y enlentecería (más aún) el programa

__at(0x3000) BYTE buffer[1024];  // Staging para la cabecera y los eventos MIDI salientes. No moverlo de aqui sin tocar SendMIDI

// ---- Soporte para MIDI formato 1 (multipista) ----
// Todo vive DENTRO del buffer de 1KB en 0x3000: los primeros 64 bytes son el staging
// de salida (y scratch de cabeceras), y los 960 restantes se reparten como cachés de
// lectura entre las pistas. OJO: no usar ni la pantalla ni memoria por encima de
// 0x33FF: los lanzadores de comandos (LNF Browser) guardan su estado en la parte alta
// de la pagina DivMMC, y tocar la pantalla provoco resets al volver al lanzador.
// Disposicion del buffer de 0x3000 durante la reproduccion:
//   0x3000-0x302F  staging de salida (48 bytes; los sysex se trocean a 48)
//   0x3030-0x303B  rutina de interrupcion IM2 (datos const cargados con el binario)
//   0x303C-0x30BB  staging de las recargas L2 (128 bytes por paso)
//   0x30BC-0x30FF  escalares globales (declaraciones __at mas abajo)
//   0x3100-0x3201  tabla de vectores IM2 (257 bytes de 0x30 -> handler en 0x3030)
//   0x3202-0x3365  cachés de lectura L1 de las pistas
//   0x3366-0x33FF  more globals (see below)
#define MAX_TRACKS   17                   // pista de tempo + 16 canales: el maximo de un format 1 tipico
#define TCACHE_TOTAL 356                  // bytes de buffer disponibles para cachés L1
#define tcaches      (buffer+0x202)       // las cachés van tras la tabla de vectores IM2
#define L2STAGE      (buffer+0x3C)        // staging de recargas L2 (hueco tras la ISR)
#define L2STAGE_SIZE 128                  // power of 2: keeps the ring fill position aligned

// Per-track state, packed in one struct: set_curtrk/scan_tracks walk a single
// pointer instead of indexing five separate arrays (each indexed DWORD access
// costs an address computation on the Z80). 12 bytes, no padding.
typedef struct
{
    DWORD off;                 // offset en el fichero de la proxima recarga de caché L1
    DWORD l2end;               // file offset where the ring's valid data ends
    WORD  bank;                // bank offset of the next L1 byte (ring read pointer)
    BYTE  cpos;                // posicion de lectura dentro de la caché (tcsize <= 255)
    BYTE  clen;                // bytes válidos en la caché
} TRKST;
TRKST tst[MAX_TRACKS];

// More globals squeezed into the buffer page (the 0x2000-0x2FFF code+data budget is
// full). Everything here is engine or program state that the esxdos kernel never
// touches; the page is always mapped while we run.
__at (0x3366) DWORD dtmp;                  // settempo scratch (a stack frame costs more code)
__at (0x336A) TRKST *curstate;             // slot of the active track (spares recomputing t*12)
__at (0x336C) TRKST *pst;                  // walking pointer for scans over tst
__at (0x336E) BYTE tracks;                 // numero de pistas MTrk encontradas (max MAX_TRACKS)
__at (0x336F) BYTE curtrk;                 // pista que se está procesando ahora mismo
__at (0x3370) BYTE wire_status;            // ultimo byte de estado enviado por el cable (0 = ninguno)
__at (0x3371) BYTE trkn;                   // indice de pista del barrido (OJO: trk_event machaca la global i)
__at (0x3372) BYTE fired;                  // a 1 si en este frame ha sonado algun evento
__at (0x3378) TRKST cur;                   // estado de la pista activa espejado aqui: mas rapido
                                           // que indexar tst en cada byte, y el cambio de pista
                                           // es una sola copia de struct
__at (0x3384) DWORD trk_next[MAX_TRACKS];  // tick absoluto (escalado por PRECISION) del proximo evento
__at (0x33C8) BYTE errno;
__at (0x33C9) BYTE fhandle;                // handle del fichero, global para recargar cachés desde cualquier rutina
__at (0x33CA) BYTE i, c;                   // contadores de bucle, etc.
__at (0x33CC) BYTE param1;                 // tipo de metaevento
__at (0x33CD) BYTE l2_eof[MAX_TRACKS];     // a 1 si la ventana ya llega hasta el final del fichero
__at (0x33DE) BYTE trk_status[MAX_TRACKS]; // running status propio de cada pista (imprescindible al mezclar)
__at (0x33EF) BYTE trk_end[MAX_TRACKS];    // a 1 cuando la pista ha terminado (FF 2F o EOF)

// Scalar globals kept in the unused tail of the L2 staging slot (0x30BC-0x30FF):
// that region is always mapped while we run, and it does not eat into the
// 0x2000-0x2FFF code+data budget, which is packed to the last byte.
__at (0x30BC) WORD ppq;          // pulsos por negra, de la cabecera (cabe en 16 bits)
__at (0x30BE) WORD lbytes;       // longitud de metaeventos y sysex
__at (0x30C0) DWORD ticks_per_int;   // ticks de reloj MIDI por interrupcion, escalado por PRECISION
__at (0x30C4) DWORD us_per_quarter;  // ultimo tempo leido con el metaevento Set Tempo
__at (0x30C8) DWORD now;         // ticks transcurridos desde el principio (escalado por PRECISION)
__at (0x30CC) WORD rem, remt;    // bytes restantes de ventana L2 (solo palabra baja: sobra)
__at (0x30D0) DWORD *pnext;      // puntero para recorrer trk_next sin indexar
__at (0x30D2) BYTE *rdptr;       // ventana de lectura de la pista activa:
__at (0x30D4) BYTE *rdend;       // evita indexar arrays en cada byte
__at (0x30D6) BYTE *cptr;        // puntero a la caché de curtrk
__at (0x30D8) WORD tcsize;       // tamaño de la caché de cada pista (TCACHE_TOTAL/tracks)
__at (0x30DA) WORD l2_area;      // bytes de banco reservados a cada pista (potencia de 2)
__at (0x30DC) BYTE pick;         // rotating prefetch candidate (persists across frames)
__at (0x30DE) BYTE tpi_frac;     // fractional ticks per frame, in 256ths of a PRECISION unit
__at (0x30DF) BYTE tfrac;        // accumulator for tpi_frac (carries whole units into now)
__at (0x30E2) WORD sent;         // MIDI bytes sent since the last accounted tick (see tick_guard)
__at (0x30E4) WORD spsave;       // caller SP while bankmove runs on the scratch stack
// 0x30E6-0x30F1: bankmove scratch stack (top at 0x30F2). While a foreign bank is
// paged at 0xC000 the caller's stack may vanish from the map, so interrupts are
// serviced with SP pointing here (everything the ISR and the EPROM's IM1 handler
// touch lives in this page, which is always mapped).
__at (0x30F4) WORD lmask;        // l2_area-1: ring offsets wrap by masking (l2_area is a power of 2)
__at (0x30F6) WORD us_per_int;   // frame length in us, measured at startup (see playmidi)
__at (0x30F8) WORD fillb;        // l2_fillstep scratch: absolute bank offset to write at
__at (0x30FA) WORD xn;           // l2_fillstep/trk_refill scratch: byte count
__at (0x30FC) BYTE sd_trk;       // track of the last SD read (0xFF: none): skips redundant seeks

// ---- L2 cache in the 128K RAM banks ----
// SD access (F_SEEK walking the FAT chain + F_READ) costs milliseconds, and done
// once per small cache refill it is heard as micro-stutter. So each track owns an
// L2 window in banks 1/3/4/6 (64KB total, shared out), managed as a RING: it is
// refilled from the SD in small bounded steps (l2_fillstep, 128 bytes, usually
// with no seek because a track's steps are sequential) slipped into event-less
// frames, and L1 cache refills from it are plain RAM copies. All rings are filled
// completely BEFORE the clock starts; after that a whole window is never reloaded
// in one go: that used to stall the music for several frames in a row and was heard
// as an audible glitch every time a window ran dry.
// NOTE: this sacrifices the 128 BASIC RAM-disc. Working 128K paging is required.
// While the esxdos kernel works it pages its own bank over 0x2000-0x3FFF and our
// IM2 vector table vanishes from the map: an IM2 interrupt at that moment would
// jump through a garbage vector. Disabling interrupts outright (DI) is no good
// either: the kernel may need them and the machine would hang inside the call.
// Solution: switch to IM1/I=0x3F around each SD access, so esxdos always runs in
// the bone-stock machine state. The table is not corrupted (only unmapped), so the
// switch is cheap; however, frames that elapse while our clock is dismounted are
// lost, which is exactly why SD accesses during playback must be short steps, never
// whole windows.
void bankmove (WORD woff, BYTE *p, WORD n, BYTE wr) STACKARGS;

void settempo (void);
// ---- Reloj por contador de interrupciones (mecanismo tomado de ZMP) ----
// Contar HALTs pierde tiempo: mientras se procesa un evento o se envian bytes por el
// MIDI pasan frames que el reloj no ve, y en los pasajes densos la musica se arrastra.
// En su lugar instalamos un handler IM2 minusculo que incrementa un contador en CADA
// interrupcion; el bucle de espera se pone al dia con todos los frames transcurridos
// (y se salta los HALT sobrantes), igual que hace zx-midiplayer.
volatile __at (0x30FE) BYTE int_cnt;   // incrementado por la ISR de IM2 (0x30FE: la ISR const lo lleva cableado)
__at (0x30FD) BYTE cnt_last;           // ultimo valor consumido por el reloj

// La ISR y la tabla de vectores NO se construyen en tiempo de ejecucion: son datos
// constantes en direcciones absolutas dentro del buffer, asi que llegan cargadas
// con el propio binario (el comando punto ocupa 0x2000-0x3201) y no cuestan codigo.
// El nucleo de esxdos desmapea esta pagina mientras trabaja pero no la corrompe.
__at (0x3030) const BYTE im2_isr[12] = {
    0xF5,                    // push af
    0x3A, 0xFE, 0x30,        // ld a,(0x30FE)   ; int_cnt
    0x3C,                    // inc a
    0x32, 0xFE, 0x30,        // ld (0x30FE),a
    0xF1,                    // pop af
    0xFB,                    // ei
    0xED, 0x4D               // reti
};
__at (0x3100) const BYTE im2_tab[258] = {   // cualquier byte del bus -> vector 0x3030
    0x30,0x30,0x30,0x30,0x30,0x30,0x30,0x30,0x30,0x30,0x30,0x30,0x30,0x30,0x30,0x30,0x30,0x30,
    0x30,0x30,0x30,0x30,0x30,0x30,0x30,0x30,0x30,0x30,0x30,0x30,0x30,0x30,0x30,0x30,0x30,0x30,
    0x30,0x30,0x30,0x30,0x30,0x30,0x30,0x30,0x30,0x30,0x30,0x30,0x30,0x30,0x30,0x30,0x30,0x30,
    0x30,0x30,0x30,0x30,0x30,0x30,0x30,0x30,0x30,0x30,0x30,0x30,0x30,0x30,0x30,0x30,0x30,0x30,
    0x30,0x30,0x30,0x30,0x30,0x30,0x30,0x30,0x30,0x30,0x30,0x30,0x30,0x30,0x30,0x30,0x30,0x30,
    0x30,0x30,0x30,0x30,0x30,0x30,0x30,0x30,0x30,0x30,0x30,0x30,0x30,0x30,0x30,0x30,0x30,0x30,
    0x30,0x30,0x30,0x30,0x30,0x30,0x30,0x30,0x30,0x30,0x30,0x30,0x30,0x30,0x30,0x30,0x30,0x30,
    0x30,0x30,0x30,0x30,0x30,0x30,0x30,0x30,0x30,0x30,0x30,0x30,0x30,0x30,0x30,0x30,0x30,0x30,
    0x30,0x30,0x30,0x30,0x30,0x30,0x30,0x30,0x30,0x30,0x30,0x30,0x30,0x30,0x30,0x30,0x30,0x30,
    0x30,0x30,0x30,0x30,0x30,0x30,0x30,0x30,0x30,0x30,0x30,0x30,0x30,0x30,0x30,0x30,0x30,0x30,
    0x30,0x30,0x30,0x30,0x30,0x30,0x30,0x30,0x30,0x30,0x30,0x30,0x30,0x30,0x30,0x30,0x30,0x30,
    0x30,0x30,0x30,0x30,0x30,0x30,0x30,0x30,0x30,0x30,0x30,0x30,0x30,0x30,0x30,0x30,0x30,0x30,
    0x30,0x30,0x30,0x30,0x30,0x30,0x30,0x30,0x30,0x30,0x30,0x30,0x30,0x30,0x30,0x30,0x30,0x30,
    0x30,0x30,0x30,0x30,0x30,0x30,0x30,0x30,0x30,0x30,0x30,0x30,0x30,0x30,0x30,0x30,0x30,0x30,
    0x30,0x30,0x30,0x30,0x30,0x30
};
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

// Recomputes the tick rate from the current tempo, zx-midiplayer style: first the
// integer us-per-MIDI-tick, then ticks-per-frame in 16.16 fixed point from the
// measured frame length. The previous single division at PRECISION=64 truncated
// hard for low-ppq files (ppq 24 at 60bpm: 30.72 -> 30, i.e. 2.3% slow — the exact
// "tempo drags in some MIDIs" symptom, invisible in high-ppq files). Splitting off
// a 10-bit fraction that the frame loop accumulates leaves only the us-per-tick
// truncation, the same sub-0.1% ZMP has.
void settempo (void)
{
    if (us_per_quarter && ppq)
    {
        // us per MIDI tick at double scale, like ZMP's tempo/ppqn but with one
        // extra bit: plain integer truncation biased high-ppq files audibly fast
        // (500000/960 -> 520 was +0.16% of tempo; the extra bit halves that)
        dtmp = us_per_quarter;
        dtmp += dtmp;
        dtmp /= ppq;
        if (!dtmp)
            dtmp = 1;
        dtmp = ((DWORD)us_per_int << 17) / dtmp;   // ticks per frame, 16.16 fixed point
        ticks_per_int = dtmp >> 10;             // integer part at PRECISION=64...
        tpi_frac = (BYTE)((WORD)dtmp >> 2);     // ...plus a fraction in 256ths of a unit
    }
}

// Measures the machine's frame duration: counts 35-T-state iterations across 2
// frames (between int_cnt edges, with our IM2 clock already running) and returns
// count*5 - 19000: the frame length in us if the clock is an exact 3.50MHz
// (35 T / 2 frames / 3.5 MHz = 5 us per iteration), relative to 19.0ms because
// the caller's plausibility windows are expressed that way.
WORD meas (void) STACKARGS
{
    __asm
    push bc
    ld hl,#0
    ld a,(#_int_cnt)
    ld c,a
mea_edge:
    ld a,(#_int_cnt)    ;wait for an edge so the count starts aligned
    cp c
    jr z,mea_edge
    add a,#2            ;target: 2 frames from now
    ld c,a
mea_loop:
    inc hl              ;(6)
    ld a,(#_int_cnt)    ;(13)
    cp c                ;(4)
    jr nz,mea_loop      ;(12)  -> 35 T-states per iteration
    ld d,h
    ld e,l              ;DE = count
    add hl,hl           ;HL = count*2
    add hl,hl           ;HL = count*4
    add hl,de           ;HL = count*5 = us per frame at 3.50MHz
    ld de,#-19000
    add hl,de           ;HL = us-19000 (the caller windows are relative to 19.0ms)
    pop bc
    __endasm;
}

// A MIDI byte occupies the wire for ~371us and is sent with interrupts disabled:
// an INT falling inside is lost forever (the Spectrum's /INT pulse lasts ~32
// T-states and is not latched). In dense passages the send chain spans a whole
// frame and the clock falls short: the music drags — the audible symptom. The
// remedy is self-verifying: if >=64 bytes went out since the last accounted frame
// (>23ms of wire time alone) and int_cnt STILL has not changed, the tick must have
// fallen inside a DI burst, so it is credited by rewinding cnt_last. If the INT
// instead landed in an EI gap, int_cnt already differs and nothing is credited.
void tick_guard (void) __naked
{
    __asm
    ld hl,(#_sent)
    ld de,#-64
    add hl,de           ;carry set iff sent >= 64; HL = sent-64
    jr c,tg_frame
    ld hl,#0            ;less than a frame of wire time: just reset the counter
    ld (#_sent),hl
    ret
tg_frame:
    ld (#_sent),hl
    ld a,(#_cnt_last)
    ld hl,#_int_cnt
    cp (hl)
    ret nz              ;the tick was seen normally: nothing to credit
    dec a
    ld (#_cnt_last),a   ;tick eaten inside a DI burst: credit it
    ret
    __endasm;
}

// Enables the IM2 clock (I=0x31; the ISR and vector table are const data loaded
// with the binary). This is a pure mode switch: cheap enough to wrap around every
// esxdos call, and harmless before the scheduler runs (the ISR just ticks a counter).
void im2_on (void) __naked
{
    __asm
    di
    ld a,#0x31
    ld i,a
    im 2
    ei
    ret
    __endasm;
}

// Restaura el modo de interrupciones estandar del Spectrum (IM1, I=0x3F).
// Tambien es inocuo si ese ya era el modo activo.
void im2_off (void) __naked
{
    __asm
    di
    im 1
    ld a,#0x3F
    ld i,a
    ei
    ret
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

// (All engine state — TRKST/tst/cur, tracks/curtrk/trkn/fired/wire_status,
// trk_next/l2_eof/trk_status/trk_end and the scalar working set — is declared
// next to the buffer, outside the 0x2000-0x2FFF budget: see the declarations
// above the engine section. Only tst itself still lives in the DATA segment.)

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
        cur.cpos = rdptr - cptr;
        cur.clen = rdend - cptr;
        *curstate = cur;
    }
    curtrk = t;
    curstate = tst + t;
    cur = *curstate;
    cptr = tcaches + (WORD)t * tcsize;
    rdptr = cptr + cur.cpos;
    rdend = cptr + cur.clen;
}

// One refill step of the active track's L2 ring: reads at most L2STAGE_SIZE bytes
// from the SD and appends them to the ring. Bounded to a few ms, so the SD cost is
// spread across many frames instead of stalling the music with whole-window reloads
// (frames that esxdos spends with our IM2 clock dismounted are lost, and a whole
// window is several frames in a row: it was heard as a glitch). The seek (a
// FAT-chain walk, the expensive part) is skipped when the last SD read was for this
// same track (sd_trk): reads of one track are sequential. l2_area is a power of
// two, so all ring arithmetic is masking with lmask — no multiplies, no carries.
// Scratch lives in globals: much cheaper than ix-indexed locals on the Z80.
void l2_fillstep (void)
{
    xn = 0;            // callers poll xn to see whether the step landed
    if (l2_eof[curtrk])
        return;
    remt = *(WORD *)&cur.l2end - *(WORD *)&cur.off;   // valid bytes in the ring (< 64K: low words suffice)
    if (remt > (WORD)(lmask - (L2STAGE_SIZE - 1)))
        return;                                       // no room for a whole step
    // Write position: the fill pointer only ever advances in steps of 128, so it
    // stays aligned and a step never straddles the window end. Crossing into the
    // next window shows up in the bits above lmask; the 16-bit wrap of the last
    // window (base 0x8000, l2_area 0x8000) folds correctly through the subtract.
    fillb = cur.bank + remt;
    if ((fillb ^ cur.bank) & ~lmask)
        fillb -= l2_area;
    im2_off ();        // esxdos must run in the bone-stock interrupt state
    if (sd_trk != curtrk)
    {
        seekset (fhandle, cur.l2end);
        sd_trk = curtrk;
    }
    xn = read (fhandle, L2STAGE, L2STAGE_SIZE);
    if (xn == 0xFFFF)
        xn = 0;
    if (xn)
    {
        bankmove (fillb, L2STAGE, xn, 1);
        cur.l2end += xn;
    }
    if (xn < L2STAGE_SIZE)
        l2_eof[curtrk] = 1;    // EOF: nothing left to prefetch for this track
    im2_on ();
}

// Refills the active track's L1 cache from its L2 ring (a RAM copy). If the ring
// is dry (prefetching could not keep up), a single bounded step is read from the
// SD and we move on. No data at all -> end of track.
void trk_refill (void)
{
    xn = *(WORD *)&cur.l2end - *(WORD *)&cur.off;
    if (xn == 0)
        l2_fillstep ();    // ring dry: borrow one bounded step; xn = bytes it read
    if (xn > tcsize)
        xn = tcsize;
    rem = l2_area - (cur.bank & lmask);               // contiguous run up to the window end
    if (xn > rem)
        xn = rem;
    if (xn == 0)
        trk_end[curtrk] = 1;
    else
    {
        bankmove (cur.bank, cptr, xn, 0);
        cur.bank += xn;
        if (!(cur.bank & lmask))                      // hit the window end: wrap to its base
            cur.bank -= l2_area;
        cur.off += xn;
    }
    rdptr = cptr;
    rdend = cptr + xn;
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
    n = 0;
    if (st != wire_status)
    {
        wire_status = st;
        buffer[n++] = st;
    }
    buffer[n++] = (b != 0xFF) ? b : trk_get();
    if ((st & 0xE0) != 0xC0)       // C0 (program) y D0 (pressure) llevan 1 solo dato
        buffer[n++] = trk_get();
    SendMIDI (buffer, n);
}

// One prefetch step for the most depleted L2 ring, so the SD cost lands in the
// quiet gaps of the music instead of on top of dense passages. Remaining-byte
// counts fit in 16 bits.
void l2_prefetch (void)
{
    // Burst behaviour: keep topping up the track of the last SD read while it has
    // room — its steps are sequential, so they need no seek. Only when that ring is
    // full (or its track is dead) consider ONE rotating candidate per frame, and
    // start a new burst only for a ring drained below HALF: an esxdos seek walks
    // the whole FAT chain and costs real milliseconds, so SD work must happen in
    // few long sequential bursts, not round-robin pokes (those lost enough clock
    // frames to audibly drag the tempo).
    // (sd_trk is always valid here: the startup prefill reads every track once.
    // A track that ended must not be topped up — its ring would swallow the next
    // track's file region. An EOF ring is cheaper to let fillstep reject.)
    if (!trk_end[sd_trk])
    {
        set_curtrk (sd_trk);
        l2_fillstep ();
        if (xn)
            return;                // the burst goes on next idle frame
    }
    if (++pick >= tracks)          // pick/pst rotate together, one candidate per frame
    {
        pick = 0;
        pst = tst;
    }
    else
        pst++;
    if (trk_end[pick] || l2_eof[pick])
        return;
    // curtrk's tst slot may be stale (its live state sits in cur between switches).
    // Harmless: at worst its burst starts a frame late, or set_curtrk degrades into
    // its t==curtrk shortcut plus a fillstep that reads the fresh mirrors.
    remt = *(WORD *)&pst->l2end - *(WORD *)&pst->off;
    if (remt < (l2_area >> 1))     // low watermark
    {
        set_curtrk (pick);
        l2_fillstep ();
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
    pst = tst;
    lbytes = 0;        // running ring base (lbytes is free until playback starts)
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
            pst->off = fpos;
            pst->l2end = fpos;             // ring vacio: el primer uso lo rellena
            pst->bank = lbytes;            // each ring starts empty at its base
            pst->cpos = 0;
            pst->clen = 0;
            pst++;
            lbytes += l2_area;
            l2_eof[tracks] = 0;
            trk_status[tracks] = 0;
            trk_end[tracks] = 0;
            tracks++;
        }
        fpos += len;    // chunks desconocidos se saltan sin contarlos
    }
}

BYTE playmidi1 (BYTE ntrk)
{
    BYTE best;

    // L2 ring per track: the largest power of two such that all the tracks fit in
    // the 64KB of banks (a power of two makes every ring wrap a masking operation).
    // Sized from the header's track count, so scan_tracks can hand out ring bases.
    // (The "i &&" guard stops the loop when i wraps to 0 on absurd track counts.)
    l2_area = 0x8000;                      // 1-2 tracks: two 32KB halves
    for (i = 2; i && i < ntrk; i <<= 1)
        l2_area >>= 1;
    lmask = l2_area - 1;
    sd_trk = 0xFF;

    scan_tracks (ntrk);
    if (tracks == 0)
        return 1;       // el que llama imprime el error

    // Cuantas menos pistas, mas caché por pista (tope: 255, cpos/clen son BYTE)
    tcsize = (WORD)TCACHE_TOTAL / (WORD)tracks;
    if (tcsize > 255)
        tcsize = 255;

    // Leemos el primer delta de cada pista para inicializar su next_tick.
    // Una pista terminada se marca con next_tick = 0xFFFFFFFF (centinela): asi el
    // planificador no necesita consultar trk_end. De paso se llena del todo el ring
    // de cada pista: la espera cae aqui, cuando aun no suena nada (un seek por
    // pista y lecturas secuenciales), y con los ficheros que caben en los bancos
    // la SD no se vuelve a tocar durante la musica.
    curtrk = 0xFF;
    best = 0;                                  // contador de pistas vivas
    for (trkn = 0; trkn < tracks; trkn++)
    {
        set_curtrk (trkn);
        do
            l2_fillstep ();
        while (xn);                            // hasta ring lleno o EOF
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
    tfrac = 0;
    wire_status = 0;
    pick = 0;          // rotating prefetch candidate; pst mirrors it
    pst = tst;
    im2_on ();         // desde aqui el reloj lo lleva la ISR
    cnt_last = int_cnt;
    while (1)
    {
        // Si pulsamos SPACE, salir
        if ((SEMIFILA8 & 0x1) == 0)
            return 0;

        tick_guard ();  // recover ticks eaten by DI send bursts (see its comment)

        // Un frame por vuelta: si la ISR no ha contado ninguno pendiente, dormimos.
        // Si vamos con retraso (un pasaje denso tardo mas de un frame), se procesan
        // vueltas seguidas sin dormir hasta ponerse al dia.
        if (cnt_last == int_cnt)
            WAIT_VRETRACE;
        cnt_last++;
        now += ticks_per_int;
        tfrac += tpi_frac;         // accumulate the fractional ticks-per-frame...
        if (tfrac < tpi_frac)      // ...8-bit wrap = a whole PRECISION unit: carry it
            now++;

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

        // Event-less frame: the perfect slot for one prefetch step
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

    // Calibrate the machine's true frame duration, zx-midiplayer style: count
    // 35-T-state iterations across 2 frames. Assuming a fixed 20ms made a Pentagon
    // (71680 T, 20.48ms per frame) play 2.4% slow. At an exact 3.5MHz the count
    // times 5 already IS the frame length in us (48K, Pentagon, Scorpion); the
    // 128K family (3.5469MHz crystal, 70908 T = 19.99ms) shows up as an inflated
    // count in a window of its own and gets snapped to the exact value.
    im2_on ();
    rem = meas ();                      // us per frame minus 19000, at an exact 3.50MHz clock
    im2_off ();
    if (rem > 2000)
        rem = 1000;                     // wild reading (turbo, NTSC...): assume 50Hz as before
    else if (rem > 1100 && rem < 1420)
        rem = 992;                      // 128K/+2/+3: reading inflated by the 1.3% faster crystal
    us_per_int = rem + 19000;
    sent = 0;
    tpi_frac = 0;                       // settempo sets it for real right below

    // Leemos el PPQ (partes por quarter, o el numero de ticks del reloj de MIDI que dura una negra
    ppq = ((WORD)buffer[12]<<8) | buffer[13];

    //ticks_per_quarter = <PPQ from the header>
    //µs_per_quarter = <Tempo in latest Set Tempo event>
    //µs_per_tick = µs_per_quarter / ticks_per_quarter
    //seconds_per_tick = µs_per_tick / 1.000.000
    //seconds = ticks * seconds_per_tick

    // Numero de ticks MIDI por interrupcion; el calculo depende del bit 7 del byte
    // 12 de la cabecera. Ambos casos se modelan con settempo: el caso SMPTE
    // (fps*subframes ticks por segundo) equivale a una "negra" de un segundo.
    if (buffer[12]&0x80)
    {
        buffer[12] &= 0x7F;
        ppq = (WORD)buffer[12] * buffer[13];
        us_per_quarter = 1000000;
    }
    else  // habitualmente los MIDs lo calculan de esta otra forma, es decir, habitualmente el bit 7 del byte 12 es 0.
        us_per_quarter = 500000;
    settempo ();

    fhandle = f;

    // Green border for format 0, yellow for format 1, so the two can be told apart
    // at a glance. NOTE: never print via RST 16 on the success path: under launchers
    // like the LNF Browser the BASIC screen channel is invalid and printing resets
    // the machine.
    ULA = buffer[9] ? 6 : 4;

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

  sent += lev;   // wire-time bookkeeping for tick_guard

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

// Copies n bytes between the preloaded file image in the banks (offset woff) and
// p, splitting the copy at the 16KB bank boundaries. Bank number for each 16KB of
// file: 1, 3, 4, 6 (the ones free while BASIC sits still) — NEVER 5 (the screen!),
// 2, 0 or 7 (shadow screen). wr=0: bank->p (read), wr=1: p->bank (write). Port
// 0x7FFD is write-only, so the value to restore comes from its copy at BANKM.
// Interrupts stay ENABLED throughout: this used to hide behind DI, but every
// L1-refill ldir is ~1ms of blindness and INTs falling inside were lost (the
// Spectrum's /INT pulse lasts ~32 T-states), slowly dragging the clock. Instead SP
// is switched to a scratch stack inside the DivMMC page: the caller's stack may
// live in 0xC000-0xFFFF (it vanishes when a foreign bank is paged there), but the
// scratch one is always mapped, so an INT mid-copy is serviced safely.
void bankmove (WORD woff, BYTE *p, WORD n, BYTE wr) STACKARGS
{
    __asm
    push bc
    push de
    ;NOTE: the caller stack -- and with it the IX parameter frame -- may live in
    ;0xC000-0xFFFF, inside the window being paged. All (ix) accesses therefore
    ;happen only while the ORIGINAL paging is active (before the out / after the
    ;restore); while the foreign bank is in, only the scratch stack is used.
    ld (#_spsave),sp
    ld sp,#0x30F2   ;scratch stack in the DivMMC page: INTs stay serviceable
bkm_loop:
    ld a,8(ix)
    or 9(ix)        ;n == 0? -> done
    jp z,bkm_done
    ld a,5(ix)      ;bank number from woff bits 14-15: 0,1,2,3 -> 1,3,4,6
    rlca
    rlca
    and #0x03
    ld b,a          ;B = 16KB index
    add a,a
    ld c,a          ;C = index*2
    ld a,b
    cp #2
    ld a,c
    jr nc,bkm_bnk   ;index >= 2 -> bank = index*2
    inc a           ;index < 2  -> bank = index*2+1
bkm_bnk:
    ld d,a
    ld a,(#23388)   ;BANKM
    and #0xF8
    or d
    push af         ;paging value for this chunk
    ld e,4(ix)
    ld a,5(ix)
    and #0x3F
    ld d,a          ;DE = woff & 0x3FFF (offset inside the bank)
    ld hl,#0x4000
    or a
    sbc hl,de       ;HL = room up to the bank boundary
    ld c,8(ix)
    ld b,9(ix)      ;BC = n
    or a
    sbc hl,bc
    add hl,bc
    jr c,bkm_chk    ;room < n -> chunk = room (HL)
    ld h,b
    ld l,c          ;chunk = n
bkm_chk:
    ld a,c          ;n -= chunk
    sub l
    ld 8(ix),a
    ld a,b
    sbc a,h
    ld 9(ix),a
    ld b,h
    ld c,l          ;BC = chunk
    set 7,d
    set 6,d         ;DE = 0xC000 + offset (bank side)
    ld l,6(ix)
    ld h,7(ix)      ;HL = p (normal-RAM side)
    ld a,l          ;p += chunk
    add a,c
    ld 6(ix),a
    ld a,h
    adc a,b
    ld 7(ix),a
    ld a,4(ix)      ;woff += chunk
    add a,c
    ld 4(ix),a
    ld a,5(ix)
    adc a,b
    ld 5(ix),a
    ld a,10(ix)     ;wr? p->bank : bank->p
    or a
    jr nz,bkm_dir
    ex de,hl        ;read: source is the bank side
bkm_dir:
    pop af          ;paging value
    exx
    push bc         ;save alt BC on the scratch stack
    ld bc,#0x7ffd
    out (c),a       ;foreign bank paged in: caller stack/frame untouchable
    exx
    ldir
    ld a,(#23388)
    exx
    out (c),a       ;restore the original paging (alt BC still holds 0x7ffd)
    pop bc
    exx
    jp bkm_loop
bkm_done:
    ld sp,(#_spsave)
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
