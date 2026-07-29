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
--nostdlib --nostdinc --no-std-crt0 --code-loc 0x2000 --data-loc 0x2eba playmid.c z80.lib -L /path/to/sdcc/lib/z80
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
//   0x3202-0x3343  cachés de lectura L1 de las pistas
//   0x3344-0x33FF  more globals (see below)
#define MAX_TRACKS   17                   // pista de tempo + 16 canales: el maximo de un format 1 tipico
#define TCACHE_TOTAL 240                  // bytes de buffer disponibles para cachés L1 (la cola
                                          // de la zona de cachés la ocupan txbuf y el horizonte)
#define tcaches      (buffer+0x202)       // las cachés van tras la tabla de vectores IM2
#define L2STAGE      (buffer+0x3C)        // 128-byte bounce buffer for bank-to-bank copies
// SD refill step: one F_READ of 512 bytes into the staging area reserved in the
// top of bank 6 (pinned by sd_enter anyway), then bounced into the target ring
// through L2STAGE. On media whose cost is per-CALL (MiSTer's image-backed path
// stalls the whole machine ~5ms per esxdos call, at any size) this quarters the
// tax versus 128-byte steps. Both sizes are powers of two: every full step
// keeps the ring fill position 512-aligned, so a step never straddles a ring
// end or a 16KB bank boundary.
#define SDSTEP       512
#define SDSTAGE      0xFA80               // bank-6 staging: woff == CPU address while pinned

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
__at (0x3344) WORD trk_steps[MAX_TRACKS];  // 128-byte fill steps left before the track's chunk is
                                           // fully buffered: fetching past its end would only pull
                                           // the NEXT track's bytes into the ring (junk that FF 2F
                                           // never lets be consumed), wasting ever-deeper seeks
                                           // right at the piece's end
__at (0x3366) DWORD sdpos;                 // actual file pointer position: lets seeknext hop
                                           // FORWARD relative to it instead of paying an absolute
                                           // seek (esxdos walks the FAT chain from the file start,
                                           // so absolute seeks get dearer as playback advances)
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
__at (0x33CD) BYTE l2_eof[MAX_TRACKS];     // a 1 cuando ya no queda nada que precargar para la pista
                                           // (su chunk entero esta en el ring, o EOF del fichero)
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
__at (0x30CC) WORD rem;          // bytes restantes de ventana L2 (solo palabra baja: sobra)
__at (0x30CE) WORD remt;         // (NB: a single __at on 'WORD rem, remt' aliased BOTH to
                                 // 0x30CC — a latent bug that bit the moment the two were
                                 // live at once. Keep every __at to one declarator.)
__at (0x30D0) DWORD *pnext;      // puntero para recorrer trk_next sin indexar
__at (0x30D2) BYTE *rdptr;       // ventana de lectura de la pista activa:
__at (0x30D4) BYTE *rdend;       // evita indexar arrays en cada byte
__at (0x30D6) BYTE *cptr;        // puntero a la caché de curtrk
__at (0x30D8) WORD tcsize;       // tamaño de la caché de cada pista (TCACHE_TOTAL/tracks)
__at (0x30DA) WORD l2_area;      // bytes de banco reservados a cada pista (potencia de 2)
__at (0x30DC) BYTE evst;         // trk_event scratch: resolved status byte for this event
__at (0x30DD) BYTE best;        // playmidi1: live-track countdown, must survive trk_event()
                                 // calls (which clobber param1 for its own metaevent scratch)
__at (0x30DE) BYTE tpi_frac;     // fractional ticks per frame, in 256ths of a PRECISION unit
__at (0x30DF) BYTE tfrac;        // accumulator for tpi_frac (carries whole units into now)
__at (0x30E0) BYTE txlen;        // bytes queued in txbuf, waiting for the frame flush
__at (0x30E1) BYTE txsnap;       // int_cnt snapshot taken when a flush starts (see tx_flush)
__at (0x30E2) BYTE sdc0;         // int_cnt snapshot taken by sd_enter (see sd_account)
__at (0x30E3) BYTE txlast;       // wire bytes sent since this frame's halt (saturating):
                                 // ~0.39ms each, sd_account's frame-phase estimate
__at (0x30E4) WORD spsave;       // caller SP while bankmove runs on the scratch stack
// 0x30E6-0x30F1: bankmove scratch stack (top at 0x30F2). While a foreign bank is
// paged at 0xC000 the caller's stack may vanish from the map, so interrupts are
// serviced with SP pointing here (everything the ISR and the EPROM's IM1 handler
// touch lives in this page, which is always mapped).
__at (0x30F4) WORD lmask;        // l2_area-1: ring offsets wrap by masking (l2_area is a power of 2)
// TX queue: events are not sent the moment they are parsed but queued here and
// flushed in one burst right after the frame's event sweep, i.e. always near the
// START of a frame, when the next /INT is a whole frame away (zx-midiplayer does
// the same). Lives in the tail of the L1-cache area of the buffer page
// (TCACHE_TOTAL leaves it free), always mapped while the player runs.
#define TXBUF_CAP  68            // >= 56 (the flush credit quantum) + a channel event
__at (0x32F2) BYTE txbuf[TXBUF_CAP];   // 0x3202 + TCACHE_TOTAL
// Retroactive SD-call time accounting (see sd_account): the frame period in
// 10us units (one 35T spin iteration at 3.5MHz).
__at (0x333A) WORD p10;          // us_per_int / 10
// Event horizon for the SD prefetch gate (see the sweep in playmidi1 and
// l2_prefetch): hznow = now + 2 frames of ticks, and hzbusy is set when any
// track's next event falls inside it — an SD step taken then would audibly
// delay those notes on slow media.
__at (0x333E) DWORD hznow;
__at (0x3342) BYTE hzbusy;
__at (0x3343) BYTE hltf;         // 1 if this scheduler pass began with a real halt
                                 // (frame phase known — see sd_account)
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
void seeknext (void) __naked;

/* --------------------------------------------------------------------------------- */
/* --------------------------------------------------------------------------------- */
/* --------------------------------------------------------------------------------- */
void getfilename (char *p, char *fname);
void playmidi (BYTE f);
void SendMIDI (BYTE *ev, BYTE lev) STACKARGS;
void SendMIDIByte (void) __naked;
void tx_flush (void) __naked;

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
  txlen = 0;         // the TX queue starts empty (playmidi may never run)

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
  tx_flush ();      // SendMIDI only queues: drain whatever is still waiting

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
// C shape of this routine (kept as the host-harness mock; SDCC compiles it 60%
// fatter than the asm below, and the byte budget is packed to the last byte):
//   d = 2 * us_per_quarter / ppq;                us per MIDI tick, double scale:
//                                                like ZMP's tempo/ppqn, but one
//                                                extra bit (plain truncation made
//                                                high-ppq files up to +0.16% fast)
//   if (!d) d = 1;
//   d = ((DWORD)us_per_int << 17) / d;           ticks per frame, 16.16 fixed point
//   ticks_per_int = d >> 10;                     integer part at PRECISION=64...
//   tpi_frac = (BYTE)((WORD)d >> 2);             ...plus a fraction in 256ths
void settempo (void)
{
    __asm
    push bc
    ld hl,(#_us_per_quarter)
    ld a,h
    or a,l
    ld de,(#_us_per_quarter + 2)
    or a,e
    or a,d
    jr z,st_out          ;tempo 0: keep the previous rate
    ld hl,(#_ppq)
    ld a,h
    or a,l
    jr z,st_out          ;ppq 0: broken header, keep the previous rate
    ld bc,#0
    push bc              ;divisor = ppq (32 bits: high word 0, low word ppq)
    push hl
    ld hl,(#_us_per_quarter)
    add hl,hl            ;dividend = 2*us_per_quarter in DE(low) HL(high)
    ex de,hl
    ld hl,(#_us_per_quarter + 2)
    adc hl,hl
    call __divulong      ;DEHL = 2*upq / ppq = us per tick, double scale
    pop bc
    pop bc
    ld a,d
    or a,e
    or a,h
    or a,l
    jr nz,st_dok
    inc de               ;degenerate tempo: avoid dividing by zero below
st_dok:
    push hl              ;divisor = us per tick (high word, then low)
    push de
    ld hl,(#_us_per_int)
    add hl,hl            ;dividend = us_per_int << 17: HL(high) = upi*2, DE(low) = 0
    ld de,#0
    call __divulong      ;DEHL = ticks per frame, 16.16 fixed point
    pop bc
    pop bc
    ld a,d               ;ticks_per_int = DEHL >> 10 (as >>8, then >>2 in place)
    ld (#_ticks_per_int + 0),a
    ld a,l
    ld (#_ticks_per_int + 1),a
    ld a,h
    ld (#_ticks_per_int + 2),a
    xor a,a
    ld (#_ticks_per_int + 3),a
    ld b,#2
st_shift:
    ld hl,#_ticks_per_int + 2
    srl (hl)
    dec hl
    rr (hl)
    dec hl
    rr (hl)
    djnz st_shift
    ld a,d               ;tpi_frac = bits 2-9 of the 16.16 fraction word (DE)
    rrca
    rrca
    and a,#0xC0
    ld b,a
    ld a,e
    srl a
    srl a
    or a,b
    ld (#_tpi_frac),a
st_out:
    pop bc
    __endasm;
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

// Sends the whole TX queue over the wire and accounts for any frame interrupt
// that the burst provably swallowed. A MIDI byte occupies the wire for ~371us and
// is bit-banged with interrupts disabled (an ISR would corrupt the bit timing);
// the Spectrum's /INT pulse lasts only ~32 T-states and is not latched, so an INT
// falling wholly inside a byte's DI window is lost forever — and every lost frame
// delays the whole song by 20ms. Two defenses combine here:
//  - the queue is flushed right after the event sweep, i.e. near the START of a
//    frame, so a typical burst is over long before the next /INT is due;
//  - a burst of >= 56 bytes occupies the wire for longer than one whole frame
//    (56 * ~382us > 21ms > any Spectrum frame), so at least floor(txlen/56)
//    interrupts MUST have struck during the flush; whatever int_cnt did not see
//    of that lower bound was eaten inside DI and is credited back to the clock.
//    (The bound is per-burst and phase-independent, so it can never over-credit;
//    the old 64-byte guard credited at most 1 tick and only when int_cnt had not
//    moved at all, which lost a dozen frames on the initial controller burst.)
void tx_flush (void) __naked
{
    __asm
    ld a,(#_txlen)
    or a,a
    ret z
    push bc
    push de
    ld a,(#_int_cnt)
    ld (#_txsnap),a          ;interrupts seen from here on are "during the flush"
    ld a,#0x0b               ;ZXUNO: bit-bang timing needs the standard 3.5MHz
    ld bc,#0xFC3B            ;ZXUNOADDR
    out (c),a
    ld bc,#0xFD3B            ;ZXUNODATA
    in a,(c)
    push af                  ;previous speed, restored after the burst
    and a,#0x3F
    out (c),a
    ld hl,#_txbuf
    ld a,(#_txlen)
    ld b,a
txf_loop:
    ld a,(hl)
    push bc
    push hl
    di
    call _SendMIDIByte
    ei
    pop hl
    pop bc
    inc hl
    djnz txf_loop
    pop af                   ;ZXUNO: restore whatever speed was set
    ld bc,#0xFD3B
    out (c),a
    ld a,(#_txlen)           ;b = floor(txlen/56): frames provably spanned
    ld b,#0
txf_div:
    sub a,#56
    jr c,txf_divdone
    inc b
    jr txf_div
txf_divdone:
    ld a,(#_txlast)          ;txlast += txlen, saturating: sd_account's estimate
    ld hl,#_txlen            ;of how deep into the frame the wire has taken us
    add a,(hl)
    jr nc,txf_nosat
    ld a,#0xFF
txf_nosat:
    ld (#_txlast),a
    xor a,a
    ld (#_txlen),a
    ld a,(#_int_cnt)
    ld hl,#_txsnap
    sub a,(hl)               ;a = interrupts int_cnt actually saw during the flush
    ld c,a
    ld a,b
    sub a,c                  ;eaten = guaranteed - seen
    jr c,txf_out
    jr z,txf_out
    ld b,a
    ld a,(#_cnt_last)
    sub a,b
    ld (#_cnt_last),a        ;credit the eaten ticks back
txf_out:
    pop de
    pop bc
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

// ---- Bank-resident IM2 clock for esxdos calls ----
// Switching to IM1 around SD accesses keeps the kernel happy but blinds the
// clock: every /INT falling inside the call is silently lost (the EPROM's 0x38
// handler counts nothing), and on real hardware an esxdos 128-byte read or a
// FAT-walk seek takes long enough to eat one nearly every time — measured on a
// MiSTer as ~1.5% tempo drag. But the kernel only ever remaps 0x0000-0x3FFF;
// main RAM and the 128K paging port are untouched. So a SECOND vector table,
// ISR and counter live in the top of bank 6 (space the ring allocator never
// hands out), and around each esxdos call the player pins bank 6 at 0xC000,
// points I at that table and moves SP into it (the caller's stack may live in
// the paged-out bank, and the kernel inherits our SP). Interrupts stay in IM2
// through the whole call, ticking the bank-side counter, which is merged into
// int_cnt on exit: the clock no longer misses SD time at all, on any card.
// Layout in bank 6 (CPU addresses while paged at 0xC000; the ring allocator
// halves l2_area when the rings would fill all 64KB, so allocation always ends
// at or below SDSTAGE):
//   0xFA80-0xFC7F  SD read staging: one F_READ lands a whole SDSTEP here
//   0xFC80-0xFD6F  stack for the esxdos call (top at 0xFD70)
//   0xFD7E         interrupt counter (merged into int_cnt by sd_exit)
//   0xFD80-0xFD8B  ISR: inc the counter, ei, reti
//   0xFDFD-0xFDFF  jp 0xFD80 (the vector 0xFDFD lands here)
//   0xFE00-0xFF00  257-byte vector table, all 0xFD (I=0xFE, any bus byte)
const BYTE sd_isr[12] = {
    0xF5,                    // push af
    0x3A, 0x7E, 0xFD,        // ld a,(0xFD7E)
    0x3C,                    // inc a
    0x32, 0x7E, 0xFD,        // ld (0xFD7E),a
    0xF1,                    // pop af
    0xFB,                    // ei
    0xED, 0x4D               // reti
};

// ---- Init overlay ----
// One-shot startup code, placed in the L1-cache area of the buffer page at
// 0x3202: everything here runs BEFORE the caches are first written (the ring
// prefill) and the memory is then recycled under them. This trades load-image
// bytes (the dot file grows toward 0x3400, still within the esxdos limit and
// below the launcher-owned 0x3400+) for the packed 0x2000-0x2FFF budget.
// Keep the overlay strictly init-time-only, and keep its end below 0x333E
// (hznow and the other runtime globals): check l__OVL in the .map.
// meas: see its prototype above. scan_tracks scratch: fpos lives directly in
// cur.l2end (what the C shape copied it into before each seeknext anyway),
// len in cur.off (the other DWORD field of the same, otherwise-idle struct).
void ovl_holder (void) __naked
{
    __asm
    .area _OVL (ABS)
    .org 0x3202
_scan_tracks::
    call ___sdcc_enter_ix
    xor a,a
    ld (#_tracks),a
    ld hl,#14
    ld (#_cur+4),hl
    ld hl,#0
    ld (#_cur+6),hl           ; fpos = 14
    ld hl,#_tst
    ld (#_pst),hl             ; pst = tst
    ld hl,#0
    ld (#_lbytes),hl          ; lbytes = 0
st_loop:
    ld a,(#_tracks)
    ld b,a
    ld a,4(ix)
    cp a,b
    jp c,st_done               ; ntrk < tracks: can't happen normally, but be safe
    jp z,st_done                ; tracks == ntrk: done
    ld a,(#_tracks)
    cp a,#17
    jp nc,st_done               ; tracks >= MAX_TRACKS: done

    call _seeknext             ; to cur.l2end (fpos)
    ld hl,#8
    push hl
    ld hl,#_buffer
    push hl
    ld a,(#_fhandle)
    push af
    inc sp
    call _read
    pop af
    pop af
    inc sp
    ld de,#8
    or a
    sbc hl,de
    jp nz,st_done               ; read() != 8: break

    ld a,(#_buffer+7)           ; len (big-endian in the file), into cur.off
    ld (#_cur+0),a
    ld a,(#_buffer+6)
    ld (#_cur+1),a
    ld a,(#_buffer+5)
    ld (#_cur+2),a
    ld a,(#_buffer+4)
    ld (#_cur+3),a

    ld hl,(#_cur+4)             ; fpos += 8
    ld de,#8
    add hl,de
    ld (#_cur+4),hl
    jr nc,st_fpos8done
    ld hl,(#_cur+6)
    inc hl
    ld (#_cur+6),hl
st_fpos8done:

    ld hl,(#_buffer+0)          ; "MTrk"?
    ld de,#0x544D
    or a
    sbc hl,de
    jp nz,st_skip_mtrk
    ld hl,(#_buffer+2)
    ld de,#0x6B72
    or a
    sbc hl,de
    jp nz,st_skip_mtrk

    ld hl,(#_pst)
    ld de,(#_cur+4)
    ld (hl),e
    inc hl
    ld (hl),d
    inc hl
    ld de,(#_cur+6)
    ld (hl),e
    inc hl
    ld (hl),d
    inc hl                      ; pst->off = fpos
    ld de,(#_cur+4)
    ld (hl),e
    inc hl
    ld (hl),d
    inc hl
    ld de,(#_cur+6)
    ld (hl),e
    inc hl
    ld (hl),d
    inc hl                      ; pst->l2end = fpos
    ld de,(#_lbytes)
    ld (hl),e
    inc hl
    ld (hl),d
    inc hl                      ; pst->bank = lbytes
    ld (hl),#0
    inc hl
    ld (hl),#0                  ; pst->cpos = pst->clen = 0
    inc hl
    ld (#_pst),hl               ; pst++

    ld hl,(#_lbytes)
    ld de,(#_l2_area)
    add hl,de
    ld (#_lbytes),hl            ; lbytes += l2_area

    ld a,(#_cur+2)               ; trk_steps[tracks] = (WORD)(len>>9) + 1
    srl a
    ld d,a
    ld a,(#_cur+1)
    rra
    ld e,a                       ; de = len >> 9 (tracks are far below 8MB)
    inc de
    ld a,(#_tracks)
    ld l,a
    ld h,#0
    add hl,hl
    ld bc,#_trk_steps
    add hl,bc
    ld (hl),e
    inc hl
    ld (hl),d

    ld a,(#_tracks)
    ld l,a
    ld h,#0
    ld bc,#_l2_eof
    add hl,bc
    ld (hl),#0                   ; l2_eof[tracks] = 0
    ld a,(#_tracks)
    ld l,a
    ld h,#0
    ld bc,#_trk_status
    add hl,bc
    ld (hl),#0                   ; trk_status[tracks] = 0
    ld a,(#_tracks)
    ld l,a
    ld h,#0
    ld bc,#_trk_end
    add hl,bc
    ld (hl),#0                   ; trk_end[tracks] = 0
    ld hl,#_tracks
    inc (hl)                     ; tracks++

st_skip_mtrk:
    ld hl,(#_cur+4)               ; fpos += len (32-bit add)
    ld de,(#_cur+0)
    add hl,de
    ld (#_cur+4),hl
    ld hl,(#_cur+6)
    ld de,(#_cur+2)
    adc hl,de
    ld (#_cur+6),hl
    jp st_loop
st_done:
    pop ix
    ret
    .area _CODE
    __endasm;
}


// Builds the table/ISR/counter in bank 6. Runs once, before playback, with
// interrupts off and NOTHING touching the stack while the bank is paged in
// (the caller's stack may live at 0xC000-0xFFFF in the launcher's bank).
void sd_im2_init (void) __naked
{
    __asm
    di
    ld a,(#23388)
    and a,#0xF8
    or a,#6
    ld bc,#0x7ffd
    out (c),a                ; bank 6 in: straight-line code only from here
    ld hl,#0xFE00            ; vector table: 257 bytes of 0xFD
    ld de,#0xFE01
    ld bc,#256
    ld (hl),#0xFD
    ldir
    ld hl,#_sd_isr           ; the ISR body
    ld de,#0xFD80
    ld bc,#12
    ldir
    ld hl,#0xFDFD            ; the landing vector: jp 0xFD80
    ld (hl),#0xC3
    inc hl
    ld (hl),#0x80
    inc hl
    ld (hl),#0xFD
    xor a,a
    ld (#0xFD7E),a           ; counter = 0
    ld a,(#23388)
    ld bc,#0x7ffd
    out (c),a                ; original bank back
    ei
    ret
    __endasm;
}

// Wraps an esxdos call: bank 6 pinned at 0xC000, I -> the bank-side table, SP
// -> the bank-side stack. IM2 stays enabled through the whole call. spsave is
// shared with bankmove: the two never nest.
void sd_enter (void) __naked
{
    __asm
    di
    ld a,(#_int_cnt)
    ld (#_sdc0),a            ; edges seen from here on belong to the SD call
    pop hl                   ; return address (the caller's stack is about to go)
    ld (#_spsave),sp
    ld a,(#23388)
    and a,#0xF8
    or a,#6
    ld bc,#0x7ffd
    out (c),a                ; bank 6 in (BANKM itself is NOT updated)
    ld sp,#0xFD70
    ld a,#0xFE
    ld i,a                   ; vectors now come from the bank-side table
    im 2                     ; (prefill runs before im2_on: force the mode too)
    ei
    jp (hl)
    __endasm;
}

void sd_exit (void) __naked
{
    __asm
    di
    pop hl                   ; return address (pushed on the bank-side stack)
    ld a,(#0xFD7E)           ; interrupts the bank-side ISR counted for us
    ld d,a
    xor a,a
    ld (#0xFD7E),a
    ld a,(#23388)
    ld bc,#0x7ffd
    out (c),a                ; original bank back
    ld a,#0x31
    ld i,a                   ; our normal table again
    ld sp,(#_spsave)
    ld a,(#_int_cnt)
    add a,d
    ld (#_int_cnt),a         ; merge: the clock saw the whole esxdos call
    ei
    jp (hl)
    __endasm;
}

// Retroactive SD-call time accounting: the bank-resident clock counts every
// interrupt the kernel lets fire, but a kernel (or SD driver) that holds DI
// through the transfer kills the /INT pulse at the source — no ISR anywhere
// can see it (measured on a MiSTer image-backed setup as a steady ~1.4% drag).
// The remedy measures the call's duration AFTER the fact. A prefetch step runs
// on a halted, event-less frame, so the frame phase at its start is known:
// phi ~= the flush's wire time (txlast bytes) plus a small parse fudge. After
// the step, spin-count 35T iterations (~10us each) until int_cnt next changes
// — on an idle frame this replaces the halt, so it costs no wall time at all.
// From edge to edge is a whole number of frames:  phi + D + w = k*p10  exactly,
// so with a running estimate of D (separate ones for read steps and for
// burst-opening seeks) k is recovered, and  k - (edges actually counted)  is
// the number of interrupts the call provably sat on: credit them to the clock.
// The estimate then updates from the exact measurement k*p10 - phi - w, so it
// tracks the medium's real speed within a couple of steps; it only needs to be
// right to within half a frame for k to resolve, and it is seeded mid-range.
void sd_account (void) __naked
{
    __asm
    ld a,(#_hltf)
    or a,a
    ret z                    ; catch-up pass: frame phase unknown, no accounting
    push bc
    ld a,(#_int_cnt)         ; w-spin: 35T = ~10us per iteration, until the next
    ld c,a                   ; visible interrupt edge (replaces the idle halt,
    ld hl,#0                 ; so on an idle frame it costs no wall time at all)
sda_spin:
    inc hl                   ;(6)
    ld a,(#_int_cnt)         ;(13)
    cp a,c                   ;(4)
    jr z,sda_spin            ;(12)
    ex de,hl                 ; de = w
    ld a,(#_txlast)          ; phi = txlast*37 + 30: the flush wire time since
    ld l,a                   ; the halt (371us/byte) plus a small wake fudge,
    ld h,#0                  ; both rounded DOWN — phi must never overshoot, an
    ld c,l                   ; overshoot could turn into a false credit
    ld b,h
    add hl,hl
    add hl,hl                ; *4
    add hl,bc                ; *5
    add hl,hl
    add hl,hl
    add hl,hl                ; *40
    or a
    sbc hl,bc
    sbc hl,bc
    sbc hl,bc                ; *37
    ld bc,#30
    add hl,bc
    add hl,de                ; hl = phi + w
    ; The last edge before the call, the call, the spin: edge to edge is a
    ; whole number of frames, k*P = phi + D + w. The step is shorter than a
    ; frame (0 <= D < P), so k = ceil((phi+w)/P) EXACTLY — no estimate needed,
    ; and for a pathological D >= P this undercounts, never overcounts: a
    ; false credit is impossible. Steps that sat on interrupts no ISR could
    ; see (a kernel holding DI) show up as phi+w > P: credit the difference.
    ld de,(#_p10)
    ld b,#1
sda_kloop:
    or a
    sbc hl,de
    jr c,sda_kdone           ; k = ceil((phi+w)/P)
    ld a,h
    or a,l
    jr z,sda_kdone
    inc b
    jr sda_kloop
sda_kdone:
    ld a,b
    cp a,#5
    jr nc,sda_out            ; > 4 frames: nonsense reading, do not credit
    ld a,(#_int_cnt)
    ld hl,#_sdc0
    sub a,(hl)               ; edges the clock DID see (interior + spin edge)
    ld c,a
    ld a,b
    sub a,c                  ; eaten inside the call, invisible to any ISR
    jr c,sda_out
    jr z,sda_out
    ld c,a
    ld a,(#_cnt_last)
    sub a,c
    ld (#_cnt_last),a        ; credit them: the song does not shift
sda_out:
    pop bc
    xor a,a                  ; the spin consumed this frame's idle wait, ending
    ld (#_txlast),a          ; exactly ON the new frame's edge — flush the queue
    jp _tx_flush             ; here just like the halt path would have
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
// y carga los de la nueva en las globales espejo. Hand asm (STACKARGS, matching
// bankmove/seeknext): this is the busiest call in the engine (every track switch),
// and t*tcsize is done as a 5-iteration shift-add instead of a call to the
// library's generic 16x16 __mulint -- cheaper in both code and cycles for a
// multiplier bounded to MAX_TRACKS-1 (5 bits).
void set_curtrk (BYTE t) STACKARGS
{
    __asm
    ld a,4(ix)
    ld hl,#_curtrk
    cp a,(hl)
    jr z,sc_exit            ; t == curtrk: nothing to do

    ld a,(hl)                ; old curtrk
    inc a
    jr z,sc_noflush          ; old curtrk was 0xFF: skip the flush

    ld hl,(#_rdptr)
    ld de,(#_cptr)
    or a
    sbc hl,de
    ld a,l
    ld (#_cur+10),a          ; cur.cpos = rdptr - cptr
    ld hl,(#_rdend)
    ld de,(#_cptr)
    or a
    sbc hl,de
    ld a,l
    ld (#_cur+11),a          ; cur.clen = rdend - cptr
    ld hl,#_cur
    ld de,(#_curstate)
    ld bc,#12
    ldir                     ; *curstate = cur

sc_noflush:
    ld a,4(ix)
    ld (#_curtrk),a
    ld h,#0                  ; curstate = tst + t*12
    ld l,a
    add hl,hl
    add hl,hl
    ld d,h
    ld e,l
    add hl,hl
    add hl,de
    ld de,#_tst
    add hl,de
    ld (#_curstate),hl
    ld de,#_cur
    ld bc,#12
    ldir                     ; cur = *curstate

    ld a,4(ix)               ; cptr = tcaches + t*tcsize (shift-add, t < 32)
    ld hl,#0
    ld de,(#_tcsize)
    ld b,#5
sc_mul:
    rrca
    jr nc,sc_mul_skip
    add hl,de
sc_mul_skip:
    sla e
    rl d
    djnz sc_mul
    ld de,#(_buffer+514)     ; tcaches
    add hl,de
    ld (#_cptr),hl

    ld a,(#_cur+10)          ; rdptr = cptr + cur.cpos
    ld e,a
    ld d,#0
    add hl,de
    ld (#_rdptr),hl
    ld hl,(#_cptr)           ; rdend = cptr + cur.clen
    ld a,(#_cur+11)
    ld e,a
    ld d,#0
    add hl,de
    ld (#_rdend),hl
sc_exit:
    __endasm;
}

// Valid bytes waiting in a track's L2 ring (< 64K: low words suffice). Hand asm:
// this was factored out of l2_fillstep/trk_refill/l2_prefetch (each used to
// inline its own copy of the subtraction), and is now called from all three.
WORD trk_remaining (TRKST *p) STACKARGS
{
    __asm
    ld l,4(ix)
    ld h,5(ix)               ; HL = p
    push hl
    ld bc,#4
    add hl,bc                ; HL = &p->l2end
    ld e,(hl)
    inc hl
    ld d,(hl)                ; DE = p->l2end (low word)
    pop hl                   ; HL = p
    ld c,(hl)
    inc hl
    ld b,(hl)                ; BC = p->off (low word)
    ld h,d
    ld l,e
    or a
    sbc hl,bc                ; HL = l2end - off
    __endasm;
}

// One refill step of the active track's L2 ring: one F_READ of up to SDSTEP
// bytes into the bank-6 staging (bank 6 is pinned by sd_enter anyway), bounced
// into the target ring through L2STAGE afterwards, with interrupts enabled.
// Bounded, so the SD cost is spread across frames instead of stalling the music
// with whole-window reloads
// (frames that esxdos spends with our IM2 clock dismounted are lost, and a whole
// window is several frames in a row: it was heard as a glitch). The seek (a
// FAT-chain walk, the expensive part) is skipped when the last SD read was for this
// same track (sd_trk): reads of one track are sequential. l2_area is a power of
// two, so all ring arithmetic is masking with lmask — no multiplies, no carries.
// Scratch lives in globals: much cheaper than ix-indexed locals on the Z80.
void l2_fillstep (void) __naked
{
    __asm
    xor a,a
    ld (#_xn),a
    ld (#_xn+1),a           ; xn = 0
    ld a,(#_curtrk)
    ld hl,#_l2_eof
    ld e,a
    ld d,#0
    add hl,de
    ld a,(hl)
    or a,a
    ret nz                  ; l2_eof[curtrk]: nothing to do

    ld hl,#_cur             ; trk_remaining is STACKARGS: push the pointer, don't
    push hl                 ; pass it in a register
    call _trk_remaining
    pop de                  ; caller cleans up; result stays in HL
    ld (#_remt),hl

    ld hl,(#_lmask)
    ld de,#-511             ; -(SDSTEP-1)
    add hl,de
    ex de,hl                ; de = lmask-511 (threshold)
    ld hl,(#_remt)
    or a
    sbc hl,de               ; hl = remt - threshold
    jr c,fls_room_ok        ; remt < threshold: room for a step
    ld a,h
    or l
    jr z,fls_room_ok        ; remt == threshold: exactly enough room
    ret                     ; remt > threshold: no room for a whole step
fls_room_ok:
    ld hl,(#_cur+8)         ; cur.bank
    ld de,(#_remt)
    add hl,de               ; fillb (tentative) = cur.bank + remt
    ld (#_fillb),hl
    ld de,(#_cur+8)
    ld a,h
    xor d
    ld d,a
    ld a,l
    xor e
    ld e,a                  ; de = fillb ^ cur.bank
    ld hl,(#_lmask)
    ld a,h
    cpl
    and d
    ld d,a
    ld a,l
    cpl
    and e
    ld e,a                  ; de = (fillb^cur.bank) & ~lmask
    ld a,d
    or e
    jr z,fls_noadjust
    ld hl,(#_fillb)
    ld de,(#_l2_area)
    or a
    sbc hl,de
    ld (#_fillb),hl
fls_noadjust:
    call _sd_enter          ; esxdos runs with the bank-resident IM2 clock ticking
    ld a,(#_sd_trk)
    ld hl,#_curtrk
    cp a,(hl)
    jr z,fls_noseek
    call _seeknext          ; to cur.l2end: a cheap forward hop from sdpos when possible
    ld a,(#_curtrk)
    ld (#_sd_trk),a
fls_noseek:
    ld hl,#0x0200           ; xn = read(fhandle, SDSTAGE, SDSTEP): one call reads
    push hl                 ; a whole 512-byte step into the bank-6 staging (the
    ld hl,#0xFA80           ; per-call cost dominates on image-backed media)
    push hl
    ld a,(#_fhandle)
    push af
    inc sp
    call _read
    pop af
    pop af
    inc sp
    ld (#_xn),hl
    call _sd_exit           ; esxdos done: merge the interrupts it sat on into
                            ; int_cnt. The ring bankmove below runs on the normal
                            ; clock (it is interrupt-safe)
    ld hl,(#_xn)
    ld a,h                  ; if (xn==0xFFFF) xn=0
    and l
    inc a
    jr nz,fls_xnok
    xor a,a
    ld (#_xn),a
    ld (#_xn+1),a
fls_xnok:
    ld hl,(#_xn)
    ld a,h
    or l
    jp z,fls_noxn
    ld hl,#0                ; bounce staging -> target ring, 128 bytes at a time
    ld (#_rem),hl           ; off = 0 (rem/remt: free scratch by now)
fls_bounce:
    ld hl,(#_xn)
    ld de,(#_rem)
    or a
    sbc hl,de               ; hl = xn - off
    jp z,fls_moved
    ld de,#128
    or a
    sbc hl,de
    jr nc,fls_chunk128
    add hl,de               ; chunk = xn - off (< 128, the short eof read)
    jr fls_chunkset
fls_chunk128:
    ld hl,#128
fls_chunkset:
    ld (#_remt),hl
    xor a,a                 ; bankmove(SDSTAGE+off, L2STAGE, chunk, 0)
    push af
    inc sp
    ld hl,(#_remt)
    push hl
    ld hl,#(_buffer+60)
    push hl
    ld hl,(#_rem)
    ld de,#0xFA80
    add hl,de
    push hl
    call _bankmove
    pop af
    pop af
    pop af
    inc sp
    ld a,#1                 ; bankmove(fillb+off, L2STAGE, chunk, 1)
    push af
    inc sp
    ld hl,(#_remt)
    push hl
    ld hl,#(_buffer+60)
    push hl
    ld hl,(#_rem)
    ld de,(#_fillb)
    add hl,de
    push hl
    call _bankmove
    pop af
    pop af
    pop af
    inc sp
    ld hl,(#_rem)           ; off += chunk
    ld de,(#_remt)
    add hl,de
    ld (#_rem),hl
    jr fls_bounce
fls_moved:
    ld hl,(#_cur+4)         ; cur.l2end += xn (32-bit: propagate the carry)
    ld de,(#_xn)
    add hl,de
    ld (#_cur+4),hl
    jr nc,fls_l2end_done
    ld hl,(#_cur+6)
    inc hl
    ld (#_cur+6),hl
fls_l2end_done:
    ld hl,(#_cur+4)         ; sdpos = cur.l2end
    ld (#_sdpos),hl
    ld hl,(#_cur+6)
    ld (#_sdpos+2),hl
fls_noxn:
    ld hl,(#_xn)            ; a full step consumes one unit of the tracks chunk
    ld de,#512              ; budget; at zero the whole chunk sits in the ring (the
    or a                    ; last step may overshoot its end by <512 bytes: harmless,
    sbc hl,de               ; FF 2F stops consumption before them). A short read is
    jr c,fls_seteof         ; the eof. Either way no more SD time is spent here.
    ld a,(#_curtrk)
    add a,a
    ld e,a
    ld d,#0
    ld hl,#_trk_steps
    add hl,de
    ld e,(hl)
    inc hl
    ld d,(hl)
    dec de
    ld (hl),d
    dec hl
    ld (hl),e
    ld a,d
    or e
    jr nz,fls_noeof
fls_seteof:
    ld a,(#_curtrk)
    ld hl,#_l2_eof
    ld e,a
    ld d,#0
    add hl,de
    ld (hl),#1
fls_noeof:
    ret
    __endasm;
}

// Refills the active track's L1 cache from its L2 ring (a RAM copy). If the ring
// is dry (prefetching could not keep up), a single bounded step is read from the
// SD and we move on. No data at all -> end of track.
void trk_refill (void) __naked
{
    __asm
    ld hl,#_cur
    push hl
    call _trk_remaining
    pop de
    ld (#_xn),hl
    ld a,h
    or l
    call z,_l2_fillstep      ; ring dry: borrow one bounded step; xn = bytes it read
    ld hl,(#_tcsize)
    ld de,(#_xn)
    or a
    sbc hl,de                ; hl = tcsize - xn
    jr nc,trf_t1              ; tcsize >= xn: keep xn as is
    ld hl,(#_tcsize)
    ld (#_xn),hl
trf_t1:
    ld hl,(#_cur+8)          ; cur.bank
    ld de,(#_lmask)
    ld a,h
    and d
    ld h,a
    ld a,l
    and e
    ld l,a                   ; hl = cur.bank & lmask
    ex de,hl
    ld hl,(#_l2_area)
    or a
    sbc hl,de                ; hl = l2_area - (cur.bank & lmask) = rem
    ld (#_rem),hl
    ex de,hl                 ; de = rem
    ld hl,(#_xn)
    or a
    sbc hl,de                ; hl = xn - rem
    jr c,trf_t2               ; xn < rem: keep xn
    ld hl,(#_rem)
    ld (#_xn),hl              ; xn = rem
trf_t2:
    ld hl,(#_xn)
    ld a,h
    or l
    jr nz,trf_have_data
    ld a,(#_curtrk)
    ld hl,#_trk_end
    ld e,a
    ld d,#0
    add hl,de
    ld (hl),#1               ; trk_end[curtrk] = 1
    jr trf_done
trf_have_data:
    xor a,a                  ; bankmove(cur.bank, cptr, xn, 0)
    push af
    inc sp
    ld hl,(#_xn)
    push hl
    ld hl,(#_cptr)
    push hl
    ld hl,(#_cur+8)
    push hl
    call _bankmove
    pop af
    pop af
    pop af
    inc sp
    ld hl,(#_cur+8)          ; cur.bank += xn
    ld de,(#_xn)
    add hl,de
    ld (#_cur+8),hl
    ld de,(#_lmask)
    ld a,h
    and d
    ld d,a
    ld a,l
    and e
    ld e,a                   ; de = cur.bank & lmask
    ld a,d
    or e
    jr nz,trf_no_wrap
    ld hl,(#_cur+8)          ; hit the window end: wrap to its base
    ld de,(#_l2_area)
    or a
    sbc hl,de
    ld (#_cur+8),hl
trf_no_wrap:
    ld hl,(#_cur+0)          ; cur.off += xn (32-bit: propagate the carry)
    ld de,(#_xn)
    add hl,de
    ld (#_cur+0),hl
    jr nc,trf_done
    ld hl,(#_cur+2)
    inc hl
    ld (#_cur+2),hl
trf_done:
    ld hl,(#_cptr)
    ld (#_rdptr),hl          ; rdptr = cptr
    ld de,(#_xn)
    add hl,de
    ld (#_rdend),hl          ; rdend = cptr + xn
    ret
    __endasm;
}

// Lee y consume el siguiente byte de la pista activa
BYTE trk_get (void) __naked
{
    __asm
    ld hl,(#_rdptr)
    ld de,(#_rdend)
    or a
    sbc hl,de
    jr nz,tg_have_data
    call _trk_refill
    ld a,(#_curtrk)
    ld hl,#_trk_end
    ld e,a
    ld d,#0
    add hl,de
    ld a,(hl)
    or a,a
    jr z,tg_have_data
    ld l,#0
    ret
tg_have_data:
    ld hl,(#_rdptr)
    ld a,(hl)
    inc hl
    ld (#_rdptr),hl
    ld l,a
    ret
    __endasm;
}

// Lee una cantidad de longitud variable (delta o longitud de metaevento/sysex).
// El caso comun (un solo byte) no hace ningun desplazamiento de 32 bits.
DWORD trk_varlen (void) __naked
{
    // SDCC's z80 DWORD return convention: DE = low word, HL = high word
    // (confirmed empirically: a function returning the constant 5 compiles to
    // DE=5, HL=0, and DWORD addition carries E->D->L->H).
    __asm
    call _trk_get
    ld a,l
    ld (#_c),a
    and a,#0x7F
    ld e,a
    ld d,#0
    ld h,#0
    ld l,#0                  ; v = c & 0x7F
tv_loop:
    ld a,(#_c)
    and a,#0x80
    jr z,tv_done
    push de
    push hl
    call _trk_get
    ld a,l
    ld (#_c),a
    pop hl
    pop de
    ld b,#7                  ; v <<= 7
tv_shift:
    sla e
    rl d
    rl l
    rl h
    djnz tv_shift
    ld a,(#_c)                ; v |= c & 0x7F
    and a,#0x7F
    or e
    ld e,a
    jr tv_loop
tv_done:
    ret
    __endasm;
}

// Procesa un evento de la pista activa (el delta ya se consumió antes).
// A diferencia del reproductor de formato 0, aqui SIEMPRE se envia el byte de
// estado: el running status de la linea MIDI se rompe al intercalar pistas.
void trk_event (void) __naked
{
    // Scratch: evst holds the resolved status byte (st); c holds b (0xFF sentinel
    // meaning "first data byte not yet read"); param1 doubles as n outside the
    // metaevent branch (disjoint uses, never live at the same time).
    __asm
    call _trk_get
    ld a,l
    ld (#_c),a               ; c = b
    bit 7,a
    jr z,tev_running          ; b&0x80==0: running status, b is the first data byte
    ld (#_evst),a             ; st = b
    cp a,#0xF0
    jr nc,tev_no_update       ; st>=0xF0: sysex/meta never touch running status
    ld hl,#_trk_status
    ld a,(#_curtrk)
    ld e,a
    ld d,#0
    add hl,de
    ld a,(#_evst)
    ld (hl),a                 ; trk_status[curtrk] = st
tev_no_update:
    ld a,#0xFF
    ld (#_c),a                ; b = 0xFF: first data byte not read yet
    jr tev_haveSt
tev_running:
    ld a,(#_curtrk)
    ld hl,#_trk_status
    ld e,a
    ld d,#0
    add hl,de
    ld a,(hl)
    ld (#_evst),a             ; st = trk_status[curtrk]
tev_haveSt:
    ld a,(#_evst)
    cp a,#0xF0
    jr z,tev_sysex
    cp a,#0xF7
    jr z,tev_sysex
    cp a,#0xFF
    jp z,tev_meta
    jp tev_channel

tev_sysex:
    xor a,a
    ld (#_wire_status),a
    call _trk_varlen
    ld (#_lbytes),de          ; lbytes = trk_varlen() (truncated to WORD)
    ld a,(#_evst)
    cp a,#0xF0
    jr nz,tev_sx_loop
    ld a,#0xF0
    ld (#_buffer),a
    ld a,#1
    push af
    inc sp
    ld hl,#_buffer
    push hl
    call _SendMIDI
    pop af
    inc sp
tev_sx_loop:
    ld hl,(#_lbytes)
    ld a,h
    or l
    jp z,tev_return
    ld de,#48
    or a
    sbc hl,de
    jr c,tev_sx_small
    ld a,#48
    jr tev_sx_nset
tev_sx_small:
    ld a,(#_lbytes)
tev_sx_nset:
    ld (#_param1),a           ; n
    xor a,a
    ld (#_i),a
tev_sx_byteloop:
    ld a,(#_param1)
    ld b,a
    ld a,(#_i)
    cp a,b
    jr nc,tev_sx_bytesdone
    call _trk_get
    ld c,l
    ld a,(#_i)
    ld hl,#_buffer
    ld e,a
    ld d,#0
    add hl,de
    ld (hl),c
    ld hl,#_i
    inc (hl)
    jr tev_sx_byteloop
tev_sx_bytesdone:
    ld a,(#_param1)
    push af
    inc sp
    ld hl,#_buffer
    push hl
    call _SendMIDI
    pop af
    inc sp
    ld hl,(#_lbytes)
    ld a,(#_param1)
    ld e,a
    ld d,#0
    or a
    sbc hl,de
    ld (#_lbytes),hl          ; lbytes -= n
    jr tev_sx_loop

tev_meta:
    call _trk_get
    ld a,l
    ld (#_param1),a           ; param1 = trk_get()
    call _trk_varlen
    ld (#_lbytes),de
    ld a,(#_param1)
    cp a,#0x2F
    jr nz,tev_meta_tempo
    ld a,(#_curtrk)
    ld hl,#_trk_end
    ld e,a
    ld d,#0
    add hl,de
    ld (hl),#1                ; trk_end[curtrk] = 1
    ret
tev_meta_tempo:
    ld a,(#_param1)
    cp a,#0x51
    jr nz,tev_meta_skip
    ld hl,(#_lbytes)
    ld de,#3
    or a
    sbc hl,de
    jr nz,tev_meta_skip        ; lbytes != 3
    xor a,a
    ld (#_us_per_quarter+3),a
    call _trk_get
    ld a,l
    ld (#_us_per_quarter+2),a
    call _trk_get
    ld a,l
    ld (#_us_per_quarter+1),a
    call _trk_get
    ld a,l
    ld (#_us_per_quarter+0),a
    call _settempo
    ret
tev_meta_skip:
    ld hl,(#_lbytes)
tev_meta_skiploop:
    ld a,h
    or l
    jp z,tev_return
    push hl
    call _trk_get
    pop hl
    dec hl
    ld (#_lbytes),hl
    jr tev_meta_skiploop

tev_channel:
    xor a,a
    ld (#_param1),a           ; n = 0
    ld a,(#_evst)
    ld hl,#_wire_status
    cp a,(hl)
    jr z,tev_ch_skipws
    ld (hl),a                 ; wire_status = st
    ld hl,#_buffer
    ld (hl),a                 ; buffer[0] = st
    ld a,#1
    ld (#_param1),a           ; n = 1
tev_ch_skipws:
    ld a,(#_c)                ; b
    cp a,#0xFF
    jr nz,tev_ch_haveb
    call _trk_get
    ld a,l
tev_ch_haveb:
    ld c,a
    ld a,(#_param1)
    ld hl,#_buffer
    ld e,a
    ld d,#0
    add hl,de
    ld (hl),c
    inc a
    ld (#_param1),a           ; n++
    ld a,(#_evst)
    and a,#0xE0
    cp a,#0xC0
    jr z,tev_ch_send
    call _trk_get
    ld c,l
    ld a,(#_param1)
    ld hl,#_buffer
    ld e,a
    ld d,#0
    add hl,de
    ld (hl),c
    inc a
    ld (#_param1),a
tev_ch_send:
    ld a,(#_param1)
    push af
    inc sp
    ld hl,#_buffer
    push hl
    call _SendMIDI
    pop af
    inc sp
tev_return:
    ret
    __endasm;
}

// One prefetch step per quiet frame, so the SD cost lands in the gaps of the
// music instead of on top of dense passages. Remaining-byte counts fit in 16 bits.
//
// Service policy — sticky bursts over a worst-ring trigger:
//  - while the track of the last SD read still has room for a whole step, keep
//    serving IT (up to a FULL ring): those reads are sequential, no seek at all.
//    Serving the strict per-frame minimum instead used to ping-pong between two
//    draining tracks (both hovering just under the half watermark), paying a
//    FAT-walk seek nearly every 128-byte step;
//  - once it is full, the most depleted live ring below the half watermark opens
//    the next burst. Picking the worst ring (rather than a rotating candidate)
//    bounds any track's wait to one idle frame regardless of track count, so no
//    ring reaches zero mid-passage and forces trk_refill()'s synchronous
//    l2_fillstep() fallback right in the middle of live events;
//  - a burst OPENS with a seek-only frame: the FAT-walk seek and the first read
//    back to back can outlast the frame even from an early phase (every /INT
//    falling while the IM2 clock is dismounted is lost), and split apart each
//    half fits comfortably. The reads then follow seek-free.
void l2_prefetch (void) __naked
{
    __asm
    ld a,#0xFF
    ld (#_c),a                ; c: worst track found so far (0xFF = none live)
    ld hl,#0xFFFF
    ld (#_xn),hl               ; xn: its remaining ring bytes
    ld (#_rem),hl              ; rem: sd_trk's remaining bytes (0xFFFF = not live)
    ld hl,#_tst
    ld (#_pst),hl              ; pst = tst: walk with a pointer, no per-track multiply
    xor a,a
    ld (#_trkn),a
lpf_loop:
    ld a,(#_trkn)
    ld hl,#_tracks
    cp a,(hl)
    jp nc,lpf_loopdone
    ld a,(#_trkn)
    ld hl,#_trk_end
    ld e,a
    ld d,#0
    add hl,de
    ld a,(hl)
    or a,a
    jr nz,lpf_next
    ld a,(#_trkn)
    ld hl,#_l2_eof
    ld e,a
    ld d,#0
    add hl,de
    ld a,(hl)
    or a,a
    jr nz,lpf_next
    ld a,(#_trkn)
    ld b,a
    ld a,(#_curtrk)
    cp a,b
    jr nz,lpf_use_pst
    ld hl,#_cur                ; curtrk's tst slot is stale (its live state sits in
    push hl                    ; cur between switches): read the mirror instead so
    call _trk_remaining        ; its real urgency is seen.
    pop de
    jr lpf_have_remt
lpf_use_pst:
    ld hl,(#_pst)
    push hl
    call _trk_remaining
    pop de
lpf_have_remt:
    ld (#_remt),hl
    ld a,(#_sd_trk)            ; remember the last-read track's level as we pass it
    ld b,a
    ld a,(#_trkn)
    cp a,b
    jr nz,lpf_not_sd
    ld (#_rem),hl
lpf_not_sd:
    ld hl,(#_remt)
    ld de,(#_xn)
    or a
    sbc hl,de                  ; hl = remt - xn
    jr nc,lpf_next              ; remt >= xn: not worse
    ld hl,(#_remt)
    ld (#_xn),hl
    ld a,(#_trkn)
    ld (#_c),a
lpf_next:
    ld hl,(#_pst)
    ld de,#12
    add hl,de
    ld (#_pst),hl
    ld hl,#_trkn
    inc (hl)
    jr lpf_loop
lpf_loopdone:
    ld a,(#_c)
    inc a
    jp z,lpf_return              ; c == 0xFF: nobody live
    ld hl,(#_l2_area)            ; stickiness: while the last-read track's ring has
    ld de,#-127                  ; room for a whole step, keep serving IT (up to a
    add hl,de                    ; FULL ring) even if another ring is lower — its
    ex de,hl                     ; reads are sequential (no FAT-walk seek). Serving
    ld hl,(#_rem)                ; the strict minimum used to ping-pong between two
    or a                         ; draining tracks, paying a seek nearly every step
    sbc hl,de                    ; (both hover just under the half watermark);
    jr nc,lpf_worst              ; long bursts amortize one seek over ~16 steps.
    ld a,(#_sd_trk)
    ld (#_c),a
    ld hl,(#_rem)
    jr lpf_gate
lpf_worst:
    ld de,(#_l2_area)
    srl d
    rr e                          ; de = l2_area >> 1
    ld hl,(#_xn)
    or a
    sbc hl,de                    ; hl = xn - (l2_area>>1)
    jp nc,lpf_return              ; xn >= threshold: all above the low watermark
    ld hl,(#_xn)
lpf_gate:                        ; HL = the chosen ring's remaining bytes
    ld a,(#_hzbusy)              ; an event is due within ~2 frames: blocking on
    or a,a                       ; the SD now would audibly delay it, so defer to
    jr z,lpf_go                  ; a quieter frame — unless the ring is nearly
    ld de,(#_l2_area)            ; dry (below a quarter), where a scheduled stall
    srl d                        ; now beats an unscheduled one at zero
    rr e
    srl d
    rr e                          ; de = l2_area >> 2
    or a
    sbc hl,de
    jr nc,lpf_return
lpf_go:
    ld a,(#_c)
    push af
    inc sp
    call _set_curtrk
    inc sp                   ; only 1 net byte was pushed for this single BYTE arg
    ld a,(#_sd_trk)          ; opening a new burst? the FAT-walk seek gets a frame
    ld hl,#_curtrk           ; of its own: seek+read back to back is the longest
    cp a,(hl)                ; blocking window there is, and split apart each half
    jr z,lpf_fill            ; delays the music half as much
    call _sd_enter
    call _seeknext           ; to cur.l2end
    ld hl,(#_cur+4)          ; the file pointer moved with no read to account for
    ld (#_sdpos),hl          ; it: track it, or the next relative seek would hop
    ld hl,(#_cur+6)          ; from a stale base
    ld (#_sdpos+2),hl
    ld a,(#_curtrk)
    ld (#_sd_trk),a
    call _sd_exit
    jp _sd_account           ; recover any tick the call sat on (DI-holding kernels)
lpf_fill:
    call _l2_fillstep        ; sequential read, no seek: the shortest window
    ld hl,(#_xn)
    ld a,h
    or a,l
    ret z                    ; no SD call actually happened: nothing to account
    jp _sd_account
lpf_return:
    ret
    __endasm;
}

// Bucle principal de reproduccion.
// ntrk es el numero de pistas que declara la cabecera MThd.
// Devuelve 0 si se ha reproducido, 1 si no se encontro ninguna pista MTrk.
// Recorre los chunks del fichero construyendo la tabla de offsets de comienzo de
// cada pista. La longitud de cada chunk está en su cabecera.
void scan_tracks (BYTE ntrk) STACKARGS;   // in the init overlay at 0x3202 (ovl_holder)

BYTE playmidi1 (BYTE ntrk) STACKARGS
{
    // Scratch: param1 doubles as 'best' (the meta-event branch of trk_event
    // never runs concurrently with this function's own body).
    __asm
    ld hl,#0x8000
    ld (#_l2_area),hl
    ld a,#2
    ld (#_i),a
pm1_szloop:
    ld a,(#_i)
    or a,a
    jr z,pm1_szdone
    cp a,4(ix)
    jr nc,pm1_szdone
    ld hl,(#_l2_area)
    srl h
    rr l
    ld (#_l2_area),hl
    ld a,(#_i)
    add a,a
    ld (#_i),a
    jr pm1_szloop
pm1_szdone:
    ld a,(#_i)               ; the bank-side IM2 clock lives in the top ~900 bytes
    cp a,4(ix)               ; of bank 6: the rings would fill all 64KB exactly
    jr nz,pm1_ovdone         ; when the sizing loop stopped at i == ntrk (a
    ld hl,(#_l2_area)        ; power-of-two track count) — halve the ring size
    srl h                    ; so the allocator never reaches the reserved tail
    rr l
    ld (#_l2_area),hl
pm1_ovdone:
    call _sd_im2_init        ; build the bank-side vector table/ISR/counter
    ld hl,(#_l2_area)
    dec hl
    ld (#_lmask),hl
    ld a,#0xFF
    ld (#_sd_trk),a

    ld a,4(ix)
    push af
    inc sp
    call _scan_tracks
    inc sp

    ; sd_account working set: the frame length for the retroactive SD timing.
    ld hl,(#_us_per_int)     ; p10 = us_per_int/10: the frame in 10us spin units
    ld de,#10
    ld bc,#0
pm1_p10loop:
    or a
    sbc hl,de
    jr c,pm1_p10done
    inc bc
    jr pm1_p10loop
pm1_p10done:
    ld (#_p10),bc

    ld a,(#_tracks)
    or a,a
    jp nz,pm1_trackscheck_ok
    ld l,#1
    jp pm1_epilogue              ; el que llama imprime el error
pm1_trackscheck_ok:

    ld hl,#0
    ld (#_tcsize),hl
    ld hl,#240               ; TCACHE_TOTAL
    ld (#_remt),hl
pm1_tcloop:
    ld hl,(#_remt)
    ld a,(#_tracks)
    ld e,a
    ld d,#0
    or a
    sbc hl,de
    jr c,pm1_tcdone
    ld (#_remt),hl
    ld hl,(#_tcsize)
    inc hl
    ld (#_tcsize),hl
    jr pm1_tcloop
pm1_tcdone:
    ld a,(#_tcsize+1)
    or a,a
    jr z,pm1_tcsize_ok
    ld hl,#255
    ld (#_tcsize),hl
pm1_tcsize_ok:

    ld a,#0xFF
    ld (#_curtrk),a
    xor a,a
    ld (#_best),a
    ld (#_trkn),a
pm1_prefill_loop:
    ld a,(#_trkn)
    ld hl,#_tracks
    cp a,(hl)
    jp nc,pm1_prefill_done
    ld a,(#_trkn)
    push af
    inc sp
    call _set_curtrk
    inc sp
pm1_prefill_fillloop:
    call _l2_fillstep
    ld hl,(#_xn)
    ld a,h
    or l
    jr nz,pm1_prefill_fillloop
    call _trk_varlen              ; DE:HL = trk_varlen() (DE low, HL high)
    ld b,#6
pm1_prefill_shift:
    sla e
    rl d
    rl l
    rl h
    djnz pm1_prefill_shift
    ld a,(#_trkn)
    add a,a
    add a,a
    ld c,a
    ld b,#0
    push hl
    ld hl,#_trk_next
    add hl,bc
    ld (hl),e
    inc hl
    ld (hl),d
    inc hl
    pop de
    ld (hl),e
    inc hl
    ld (hl),d

    ld a,(#_trkn)
    ld hl,#_trk_end
    ld e,a
    ld d,#0
    add hl,de
    ld a,(hl)
    or a,a
    jr z,pm1_pf_alive
    ld a,(#_trkn)
    add a,a
    add a,a
    ld c,a
    ld b,#0
    ld hl,#_trk_next
    add hl,bc
    ld (hl),#0xFF
    inc hl
    ld (hl),#0xFF
    inc hl
    ld (hl),#0xFF
    inc hl
    ld (hl),#0xFF
    jr pm1_pf_next
pm1_pf_alive:
    ld hl,#_best
    inc (hl)
pm1_pf_next:
    ld hl,#_trkn
    inc (hl)
    jp pm1_prefill_loop
pm1_prefill_done:
    ld a,(#_best)
    or a,a
    jp nz,pm1_main_start
    ld l,#0
    jp pm1_epilogue
pm1_main_start:
    ld hl,#0
    ld (#_now),hl
    ld (#_now+2),hl
    xor a,a
    ld (#_tfrac),a
    ld (#_wire_status),a
    call _im2_on
    ld a,(#_int_cnt)
    ld (#_cnt_last),a

pm1_mainloop:
    ld bc,#0x7ffe
    in a,(c)
    and a,#1
    jp nz,pm1_no_space
    ld l,#0
    jp pm1_epilogue
pm1_no_space:
    xor a,a
    ld (#_hltf),a            ;assume catch-up pass (frame phase unknown)
    ld a,(#_cnt_last)
    ld hl,#_int_cnt
    cp a,(hl)
    jr nz,pm1_skip_halt
    halt
    ld a,#1                  ;woke ON the interrupt: phase is known from here on
    ld (#_hltf),a
    xor a,a
    ld (#_txlast),a          ;wire bytes sent this frame: none yet
    call _tx_flush           ;just woke ON the frame interrupt: the whole queue goes
                             ;out with the next /INT a full frame away. Any other
                             ;moment in the frame is a guess — a long sweep can end
                             ;anywhere, even right on the edge — so this halt (and
                             ;queue overflow, which self-credits) are the only two
                             ;places that ever touch the wire.
pm1_skip_halt:
    ld hl,#_cnt_last
    inc (hl)
    ld hl,(#_now)
    ld de,(#_ticks_per_int)
    add hl,de
    ld (#_now),hl
    ld hl,(#_now+2)
    ld de,(#_ticks_per_int+2)
    adc hl,de
    ld (#_now+2),hl
    ld a,(#_tfrac)
    ld hl,#_tpi_frac
    add a,(hl)
    ld (#_tfrac),a
    jr nc,pm1_no_carry_now
    ld hl,(#_now)
    inc hl
    ld (#_now),hl
    ld a,h
    or l
    jr nz,pm1_no_carry_now
    ld hl,(#_now+2)
    inc hl
    ld (#_now+2),hl
pm1_no_carry_now:

    ld hl,(#_ticks_per_int)  ;hznow = now + 2 frames of ticks: the sweep below
    ld de,(#_ticks_per_int+2);marks hzbusy when any track's next event falls
    add hl,hl                ;inside — an SD step taken then would audibly delay
    rl e                     ;those notes on slow media (see l2_prefetch)
    rl d
    ld bc,(#_now)
    add hl,bc
    ld (#_hznow),hl
    ld hl,(#_now+2)
    adc hl,de
    ld (#_hznow+2),hl
    xor a,a
    ld (#_hzbusy),a
    ld (#_fired),a
    ld hl,#_trk_next
    ld (#_pnext),hl
    xor a,a
    ld (#_trkn),a
pm1_sched_loop:
    ld a,(#_trkn)
    ld hl,#_tracks
    cp a,(hl)
    jp nc,pm1_sched_done
    ld hl,(#_pnext)
    ld e,(hl)
    inc hl
    ld d,(hl)
    inc hl
    ld c,(hl)
    inc hl
    ld b,(hl)                    ; BC:DE = *pnext (high:low)
    ld hl,(#_now+2)
    or a
    sbc hl,bc
    jp c,pm1_sched_next            ; now_high<pnext_high -> pnext>now -> skip
    ld a,h
    or l
    jr nz,pm1_fire                 ; now_high>pnext_high -> pnext<now -> fire
    ld hl,(#_now)
    or a
    sbc hl,de
    jp c,pm1_sched_next            ; now_low<pnext_low (highs equal) -> pnext>now -> skip
pm1_fire:
    ld a,#1
    ld (#_fired),a
    ld a,(#_trkn)
    push af
    inc sp
    call _set_curtrk
    inc sp
pm1_fire_loop:
    call _trk_event
    ld a,(#_trkn)
    ld hl,#_trk_end
    ld e,a
    ld d,#0
    add hl,de
    ld a,(hl)
    or a,a
    jr z,pm1_fire_notend
    ld hl,(#_pnext)
    ld (hl),#0xFF
    inc hl
    ld (hl),#0xFF
    inc hl
    ld (hl),#0xFF
    inc hl
    ld (hl),#0xFF
    ld hl,#_best
    dec (hl)
    ld a,(hl)
    or a,a
    jp nz,pm1_sched_next
    ld l,#0
    jp pm1_epilogue
pm1_fire_notend:
    call _trk_varlen
    ld b,#6
pm1_fire_shift:
    sla e
    rl d
    rl l
    rl h
    djnz pm1_fire_shift
    push hl
    ld hl,(#_pnext)
    ld a,(hl)
    add a,e
    ld (hl),a
    inc hl
    ld a,(hl)
    adc a,d
    ld (hl),a
    inc hl
    pop de
    ld a,(hl)
    adc a,e
    ld (hl),a
    inc hl
    ld a,(hl)
    adc a,d
    ld (hl),a

    ld hl,(#_pnext)
    ld e,(hl)
    inc hl
    ld d,(hl)
    inc hl
    ld c,(hl)
    inc hl
    ld b,(hl)
    ld hl,(#_now+2)
    or a
    sbc hl,bc
    jp c,pm1_sched_next
    ld a,h
    or l
    jp nz,pm1_fire_loop
    ld hl,(#_now)
    or a
    sbc hl,de
    jp c,pm1_sched_next
    jp pm1_fire_loop
pm1_sched_next:
    ld hl,(#_hznow+2)        ;not due this frame, but due inside the horizon?
    or a                     ;(BC:DE still hold this track's next-event tick)
    sbc hl,bc
    jr c,pm1_hz_far          ;hz_high < next_high: well beyond the horizon
    jr nz,pm1_hz_near        ;hz_high > next_high: inside
    ld hl,(#_hznow)
    or a
    sbc hl,de
    jr c,pm1_hz_far
pm1_hz_near:
    ld a,#1
    ld (#_hzbusy),a
pm1_hz_far:
    ld hl,(#_pnext)
    ld de,#4
    add hl,de
    ld (#_pnext),hl
    ld hl,#_trkn
    inc (hl)
    jp pm1_sched_loop
pm1_sched_done:
    ld a,(#_fired)
    or a,a
    jr nz,pm1_loop_end
    ld a,(#_hltf)            ;only take an SD step when the pass began with a
    or a,a                   ;halt: sd_account can then measure the call and
    jr z,pm1_loop_end        ;recover anything it sat on — a step taken on a
    call _l2_prefetch        ;catch-up pass would be unaccountable
pm1_loop_end:
    jp pm1_mainloop
pm1_epilogue:
    __endasm;
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
    else if ((WORD)(rem - 1101) < 319)
        rem = 992;                      // 128K/+2/+3 window: reading inflated by the faster crystal
    us_per_int = rem + 19000;
    txlen = 0;
    ((BYTE *)&sdpos)[3] = 0xFF;         // poison the position high: seeknext goes absolute
                                        // until the first playback read tracks it for real

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
    ULA = 7; // buffer[9] ? 6 : 4;

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

// Queues a block of memory for the MIDI output. Nothing touches the wire here:
// the queue is drained by tx_flush, normally once per frame right after the
// event sweep (or here, when the queue would overflow — the flush then credits
// any interrupt the oversized burst provably ate, see tx_flush).
void SendMIDI (BYTE *ev, BYTE lev) STACKARGS
{
  __asm
  push bc
  push de
  ld a,(#_txlen)
  cp a,#56                 ;>= 56 queued: the flush window is already >= 1 whole
  jr nc,smq_flush          ;frame of wire, so its credit accounting is exact —
                           ;flushing now keeps every overflow window that tight
  add a,6(ix)              ;txlen + lev
  cp a,#(68+1)             ;TXBUF_CAP (keep in sync!): past it the queue would
                           ;overrun into the est_*/hznow globals right after it
  jr c,smq_room
smq_flush:
  call _tx_flush           ;drain what is queued first
  ld a,6(ix)
smq_room:
  ld hl,#_txlen            ;dest = txbuf + old txlen; txlen = txlen + lev
  ld d,#0
  ld e,(hl)
  ld (hl),a
  ld hl,#_txbuf
  add hl,de
  ex de,hl                 ;de = append position
  ld l,4(ix)
  ld h,5(ix)               ;hl = ev
  ld c,6(ix)
  ld b,#0                  ;bc = lev (never 0: all callers pass at least 1)
  ldir
  pop de
  pop bc
  __endasm;
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

// Positions the file pointer at cur.l2end as cheaply as possible. An absolute
// esxdos seek walks the FAT cluster chain from the FIRST cluster of the file, so
// its cost grows with the offset — by minute 4 of a piece every burst start was a
// felt hiccup. But the player's seeks walk the file in ascending track order, so
// almost all of them are short hops FORWARD of the real position (sdpos): those
// are issued as SEEK_CUR with the delta, letting esxdos continue from the cluster
// it is already on. Only backward targets (the rotation wrapping from the last
// track to the first, roughly once per rings-half-drained cycle) pay the absolute
// walk. The esxdos original takes the mode in IXL; NextZXOS' compatible API takes
// it in L — both are loaded (0 = from start, 1 = forward from current).
void seeknext (void) __naked
{
    __asm
    push bc
    push de
    ld a,(#_cur + 4)    ;BCDE = cur.l2end - sdpos (the forward delta)
    ld hl,#_sdpos
    sub a,(hl)
    ld e,a
    ld a,(#_cur + 5)
    inc hl
    sbc a,(hl)
    ld d,a
    ld a,(#_cur + 6)
    inc hl
    sbc a,(hl)
    ld c,a
    ld a,(#_cur + 7)
    inc hl
    sbc a,(hl)
    ld b,a              ;carry set: the target is BEHIND the current position
    ld l,#1             ;mode 1: forward from current
    jr nc,snx_go
    ld de,(#_cur + 4)   ;backward: absolute seek to the target (mode 0)
    ld bc,(#_cur + 6)
    ld l,#0
snx_go:
    ld h,#0
    push ix
    push hl
    pop ix              ;IXL = mode (esxdos); L = mode (NextZXOS)
    ld a,(#_fhandle)
    rst #8
    .db #F_SEEK
    pop ix
    pop de
    pop bc
    ret
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
