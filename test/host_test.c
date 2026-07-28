/* Host-side test harness for the playmid format-1 engine.
 * The engine code is extracted verbatim from playmid.c (between the
 * FORMAT1 ENGINE BEGIN/END markers) into engine.inc, so the code under
 * test is exactly the code that ships.
 *
 * Output: one line per SendMIDI call:  <int_count> <hex bytes...>
 * where int_count is the number of simulated 50Hz interrupts elapsed.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

typedef unsigned char BYTE;
typedef unsigned short WORD;
typedef uint32_t DWORD;

#define PRECISION 64
#define MAX_TRACKS   17
#define TCACHE_TOTAL 240

/* globals shared with the engine (declared in playmid.c outside the markers) */
BYTE i, c;
BYTE param1;
WORD lbytes;
DWORD ppq, ticks_per_int;
DWORD us_per_quarter;
BYTE buffer[1024];
BYTE tcaches[TCACHE_TOTAL];
BYTE fhandle;
BYTE errno_;
/* state the Z80 build keeps at absolute addresses in the buffer page */
typedef struct {
    DWORD off;
    DWORD l2end;
    WORD  bank;
    BYTE  cpos;
    BYTE  clen;
} TRKST;
TRKST tst[MAX_TRACKS];
TRKST cur;
TRKST *curstate, *pst;
BYTE tracks, curtrk, trkn, fired, wire_status;
DWORD trk_next[MAX_TRACKS];
WORD trk_steps[MAX_TRACKS];
BYTE l2_eof[MAX_TRACKS], trk_status[MAX_TRACKS], trk_end[MAX_TRACKS];
WORD tcsize, l2_area, rem, remt, lmask, fillb, xn;
BYTE pick, sd_trk;
DWORD now;
DWORD hznow; BYTE hzbusy;    /* SD-prefetch event horizon (see the engine sweep) */
BYTE hltf, txlast;           /* frame-phase tracking for sd_account (Z80 build) */
WORD p10;
DWORD *pnext;
BYTE *rdptr, *rdend, *cptr;
static BYTE banks64[65536];
static void bankmove (WORD woff, BYTE *p, WORD n, BYTE wr)
{
    if (wr) memcpy(banks64 + woff, p, n); else memcpy(p, banks64 + woff, n);
}
#define L2STAGE      (buffer+0x3C)
#define L2STAGE_SIZE 128
DWORD sdpos;
static unsigned long n_seeks_fwd = 0;
BYTE tpi_frac, tfrac;
WORD us_per_int;
static void settempo (void)   /* mirror of the Z80 build: 16.16 ticks per frame */
{
    DWORD d;
    if (!us_per_quarter || !ppq) return;
    d = us_per_quarter * 2 / ppq;   /* us per tick at double scale, like the Z80 build */
    if (!d) d = 1;
    d = ((DWORD)us_per_int << 17) / d;
    ticks_per_int = d >> 10;
    tpi_frac = (BYTE)((WORD)d >> 2);
}

static FILE *F;
static unsigned long sim_ints = 0;
static unsigned long n_seeks = 0, n_reads = 0;

#define SEMIFILA8 0xFF                 /* SPACE never pressed */
volatile BYTE int_cnt; BYTE cnt_last; BYTE im2_active;
static void im2_on (void) { im2_active = 1; cnt_last = int_cnt; }
static void im2_off (void) { im2_active = 0; }
/* bank-resident IM2 clock across esxdos calls: hardware-only concern — harness
   reads cost no simulated time, so no interrupts can fall inside them */
static void sd_im2_init (void) {}
static void sd_enter (void) {}
static void sd_exit (void) {}
static void sd_account (void) {}   /* retroactive SD-call timing: hardware-only */
static void tx_flush (void) {}     /* wire batching + DI-burst tick recovery: hardware-only
                                      concern; the harness SendMIDI prints immediately, which
                                      matches the Z80 build's tick-level timing exactly (the
                                      queue drains within the same frame it was filled) */
#define WAIT_VRETRACE (sim_ints++, int_cnt++)

static WORD read (BYTE handle, BYTE *buf, WORD nbytes)
{
    (void)handle;
    n_reads++;
    return (WORD)fread (buf, 1, nbytes, F);
}

static void seeknext (void)   /* mock: the Z80 build hops forward from sdpos when it can */
{
    n_seeks++;
    if (cur.l2end >= sdpos) n_seeks_fwd++;
    fseek (F, (long)cur.l2end, SEEK_SET);
}

static void SendMIDI (BYTE *ev, BYTE lev)
{
    BYTE k;
    printf ("%lu", sim_ints);
    for (k = 0; k < lev; k++)
        printf (" %02X", ev[k]);
    printf ("\n");
}

static int cmp4b (BYTE *a, BYTE *b)
{
    if (a[0] != b[0] || a[1] != b[1] || a[2] != b[2] || a[3] != b[3])
        return 0;
    return 1;
}

#include "engine.inc"

int main (int argc, char **argv)
{
    if (argc < 2) { fprintf (stderr, "usage: %s file.mid\n", argv[0]); return 1; }
    F = fopen (argv[1], "rb");
    if (!F) { perror ("open"); return 1; }

    if (fread (buffer, 1, 512, F) < 14) { fprintf (stderr, "short file\n"); return 1; }
    if (!cmp4b (buffer, (BYTE*)"MThd")) { fprintf (stderr, "no MThd\n"); return 1; }
    if (buffer[9] > 1) { fprintf (stderr, "unsupported format\n"); return 1; }

    us_per_int = getenv("USPI") ? atoi(getenv("USPI")) : 20000;  /* override to match an emu run */
    ppq = buffer[12]<<8 | buffer[13];
    if (buffer[12] & 0x80)
    {
        buffer[12] &= 0x7F;
        ppq = buffer[12] * buffer[13];
        us_per_quarter = 1000000;
    }
    else
        us_per_quarter = 500000;
    settempo ();

    fhandle = 0;
    playmidi1 (buffer[10] ? MAX_TRACKS : buffer[11]);

    fprintf (stderr, "stats: %lu ints (%.2f s), %lu seeks (%lu fwd-relative), %lu reads, %u tracks, tcsize %u\n",
             sim_ints, sim_ints * 0.02, n_seeks, n_seeks_fwd, n_reads, tracks, tcsize);
    fclose (F);
    return 0;
}
