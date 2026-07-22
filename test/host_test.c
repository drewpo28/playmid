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
#define MAX_TRACKS   24
#define TCACHE_TOTAL 960

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

static FILE *F;
static unsigned long sim_ints = 0;
static unsigned long n_seeks = 0, n_reads = 0;

#define SEMIFILA8 0xFF                 /* SPACE never pressed */
#define WAIT_VRETRACE sim_ints++

static WORD read (BYTE handle, BYTE *buf, WORD nbytes)
{
    (void)handle;
    n_reads++;
    return (WORD)fread (buf, 1, nbytes, F);
}

static void seekset (BYTE handle, DWORD offset)
{
    (void)handle;
    n_seeks++;
    fseek (F, (long)offset, SEEK_SET);
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

    ppq = buffer[12]<<8 | buffer[13];
    if (buffer[12] & 0x80)
    {
        buffer[12] &= 0x7F;
        ticks_per_int = PRECISION * buffer[12] * buffer[13] * 20;
    }
    else
        ticks_per_int = PRECISION * 20 * ppq / 500;

    fhandle = 0;
    playmidi1 (buffer[10] ? MAX_TRACKS : buffer[11]);

    fprintf (stderr, "stats: %lu ints (%.2f s), %lu seeks, %lu reads, %u tracks, tcsize %u\n",
             sim_ints, sim_ints * 0.02, n_seeks, n_reads, tracks, tcsize);
    fclose (F);
    return 0;
}
