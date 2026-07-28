/* Run the PLAYMID dot-command binary under Z80 emulation with esxdos syscall traps.
 * Usage: ./emu PLAYMID file.mid [max_frames]
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "z80.h"

#ifndef INTT               /* T-states per frame: 69888=48K, 70908=128K, 71680=Pentagon */
#define INTT 69888
#endif

static uint8_t mem[65536];
static uint8_t banks[8][0x4000];        /* 128K RAM banks, window at 0xC000 */
static uint8_t curbank = 0;
static unsigned long bank_switches = 0;
static FILE *fhs[16];
static unsigned long midi_outs = 0, frames = 0, syscalls = 0;
static unsigned long long prof[256];   /* cycles per 256-byte PC bucket */
static int trace = 1;

#define HOOK_BASE 128
#define MISC_BASE (HOOK_BASE+8)
#define FSYS_BASE (MISC_BASE+16)
#define M_GETSETDRV (MISC_BASE+1)
#define F_OPEN  (FSYS_BASE+2)
#define F_CLOSE (FSYS_BASE+3)
#define F_READ  (FSYS_BASE+5)
#define F_WRITE (FSYS_BASE+6)
#define F_SEEK  (FSYS_BASE+7)

/* real 128K memory model: 0x4000-0x7FFF = bank 5 (the screen lives here!),
   0x8000-0xBFFF = bank 2, 0xC000-0xFFFF = switchable bank */
static uint8_t *maddr(uint16_t a) {
    if (a >= 0xC000) return &banks[curbank][a - 0xC000];
    if (a >= 0x8000) return &banks[2][a - 0x8000];
    if (a >= 0x4000) return &banks[5][a - 0x4000];
    return &mem[a];
}
#define RD(a) (*maddr((uint16_t)(a)))
static uint8_t rb(void *ud, uint16_t a) { (void)ud; return RD(a); }
static unsigned long canary_writes = 0;
static unsigned long cnt_credits = 0;
static void wb(void *ud, uint16_t a, uint8_t v) {
    (void)ud;
    if (a == 0x30FD && getenv("EMU_WATCH_CNT")) {
        uint8_t old = *maddr(a);
        if ((uint8_t)(old - v) < 8 && old != v) {   /* a decrease = a credit */
            cnt_credits += (uint8_t)(old - v);
            fprintf(stderr, "[credit] frame %lu: cnt_last %u -> %u\n", frames, old, v);
        }
    }
    if (((a >= 0x3400 && a <= 0x3FFF) || (a >= 0x4000 && a <= 0x5AFF)) && canary_writes++ < 8)
        fprintf(stderr, "[CANARY] write %02X to %04X (cpu)\n", v, a);
    if (a >= 0xC000 && (curbank == 5 || curbank == 7 || curbank == 2) && canary_writes++ < 8)
        fprintf(stderr, "[CANARY] write %02X to banked 0x%04X with FORBIDDEN bank %u mapped\n", v, a, curbank);
    *maddr(a) = v;
}
static uint8_t pin(z80 *z, uint8_t port) { (void)z; (void)port; return 0xFF; }
static void pout(z80 *z, uint8_t port, uint8_t v) {
    uint16_t full = ((uint16_t)z->b << 8) | port;
    if ((full & 0x8002) == 0) {      /* 128K memory port 0x7FFD */
        curbank = v & 7;
        bank_switches++;
        return;
    }
    if (port == 0xFD) midi_outs++;   /* AY / MIDI bit-bang activity */
    if (port == 0xFE) {              /* ULA: bits 0-2 = border colour */
        static int lastb = -1;
        if ((v & 7) != lastb) {
            lastb = v & 7;
            fprintf(stderr, "[border] %d\n", lastb);
        }
    }
}

static uint16_t hl(z80 *z) { return (z->h << 8) | z->l; }
static uint16_t bc(z80 *z) { return (z->b << 8) | z->c; }
static uint16_t de(z80 *z) { return (z->d << 8) | z->e; }

static void esx (z80 *z, uint8_t call)
{
    syscalls++;
    switch (call) {
    case M_GETSETDRV:
        z->a = 0x41; z->cf = 0; break;
    case F_OPEN: {
        char name[64]; int k = 0;
        uint16_t p = hl(z);
        while (k < 63 && RD(p)) { name[k++] = RD(p); p++; }
        name[k] = 0;
        int h = -1;
        for (int j = 4; j < 16; j++) if (!fhs[j]) { h = j; break; }
        FILE *f = fopen(name, "rb");
        if (trace) fprintf(stderr, "[esx] F_OPEN '%s' mode=%02X -> %s\n", name, z->b, f ? "ok" : "FAIL");
        if (!f) { z->a = 5; z->cf = 1; break; }
        fhs[h] = f; z->a = h; z->cf = 0; break;
    }
    case F_CLOSE:
        if (trace) fprintf(stderr, "[esx] F_CLOSE h=%d\n", z->a);
        if (z->a < 16 && fhs[z->a]) { fclose(fhs[z->a]); fhs[z->a] = NULL; }
        z->cf = 0; break;
    case F_READ: {
        uint16_t addr = hl(z), n = bc(z);
        FILE *f = (z->a < 16) ? fhs[z->a] : NULL;
        if (!f) { z->a = 7; z->cf = 1; break; }
        size_t r = fread(mem + addr, 1, n, f);
        if (trace) fprintf(stderr, "[esx] F_READ h=%d addr=%04X n=%u -> %zu\n", z->a, addr, n, r);
        z->b = (r >> 8) & 0xFF; z->c = r & 0xFF;
        z->h = ((addr + r) >> 8) & 0xFF; z->l = (addr + r) & 0xFF;
        z->cf = 0; break;
    }
    case F_SEEK: {
        /* esxdos 0.8.x: mode in IXL; NextZXOS: mode in L. Emulate esxdos strictly. */
        uint8_t mode = z->ix & 0xFF;
        uint32_t off = ((uint32_t)z->b << 24) | ((uint32_t)z->c << 16) | ((uint32_t)z->d << 8) | z->e;
        FILE *f = (z->a < 16) ? fhs[z->a] : NULL;
        if (!f) { z->a = 7; z->cf = 1; break; }
        long base = (mode == 0) ? 0 : ftell(f);
        long tgt = (mode == 2) ? base - (long)off : base + (long)off;
        fseek(f, tgt, SEEK_SET);
        long np = ftell(f);
        if (trace) fprintf(stderr, "[esx] F_SEEK h=%d mode=%d(ixl) l=%d off=%u -> pos %ld\n", z->a, mode, z->l, off, np);
        z->b = (np >> 24) & 0xFF; z->c = (np >> 16) & 0xFF; z->d = (np >> 8) & 0xFF; z->e = np & 0xFF;
        z->cf = 0; break;
    }
    default: {
        extern uint16_t pcring[64]; extern int pcri; extern int pcdumped;
        fprintf(stderr, "[esx] UNIMPLEMENTED call %02X (ret addr %04X, sp %04X)\n", call,
                (unsigned)(RD(z->sp-2) | (RD(z->sp-1)<<8)), z->sp);
        if (!pcdumped) {
            pcdumped = 1;
            fprintf(stderr, "[pc history]");
            for (int k = 0; k < 64; k++) fprintf(stderr, " %04X", pcring[(pcri + k) & 63]);
            fprintf(stderr, "\n");
        }
        break; }
        z->a = 1; z->cf = 1; break;
    }
}

uint16_t pcring[64]; int pcri = 0; int pcdumped = 0;

int main (int argc, char **argv)
{
    if (argc < 3) { fprintf(stderr, "usage: %s PLAYMID file.mid [max_frames]\n", argv[0]); return 1; }
    unsigned long max_frames = (argc > 3) ? strtoul(argv[3], 0, 10) : 20000;
    unsigned int send_trap = (argc > 4) ? strtoul(argv[4], 0, 16) : 0;
    int start_di = (argc > 5) ? atoi(argv[5]) : 0;

    FILE *f = fopen(argv[1], "rb");
    if (!f) { perror("PLAYMID"); return 1; }
    size_t sz = fread(mem + 0x2000, 1, 0x4000, f);
    fclose(f);
    fprintf(stderr, "loaded %zu bytes at 0x2000\n", sz);

    /* command line at 0x9000, terminated by 0x0D */
    uint16_t cl = 0x9000;   /* 0x8000-0xBFFF = bank 2 */
    strcpy((char *)banks[2] + (cl - 0x8000), argv[2]);
    banks[2][cl - 0x8000 + strlen(argv[2])] = 0x0D;

    /* IM1 handler stub: EI ; RET */
    mem[0x0038] = 0xFB; mem[0x0039] = 0xC9;

    z80 z;
    z80_init(&z);
    z.read_byte = rb; z.write_byte = wb; z.port_in = pin; z.port_out = pout;
    z.pc = 0x2000;
    z.sp = 0xFF00 - 2;
    banks[0][0x3EFE] = 0x00; banks[0][0x3EFF] = 0x00;   /* return-to-0 sentinel (bank 0 at 0xC000) */
    banks[5][23388 - 0x4000] = 0x10;                    /* BANKM (0x5B5C, bank 5): paging unlocked, bank 0 */
    z.h = cl >> 8; z.l = cl & 0xFF;
    z.iy = 0x5C3A;
    z.iff1 = z.iff2 = !start_di;
    z.interrupt_mode = 1;

    int dbg = (getenv("EMUDBG") != 0);
    unsigned long last_int = 0;
    unsigned long steps = 0;
    /* realistic ULA /INT: the pulse lasts ~32 T-states and is NOT latched —
       if interrupts are disabled for its whole duration, the frame is lost */
    unsigned long int_gen_cyc = 0, lost_ints = 0;
    while (1) {
        if (z.pc == 0x0000) {
            {
                unsigned long long tot = 0;
                for (int k = 0; k < 256; k++) tot += prof[k];
                fprintf(stderr, "profile (cycles by PC page, top 12):\n");
                for (int r = 0; r < 12; r++) {
                    int bi = 0;
                    for (int k = 1; k < 256; k++) if (prof[k] > prof[bi]) bi = k;
                    if (!prof[bi]) break;
                    fprintf(stderr, "  %02XXX: %llu (%.1f%%)\n", bi, prof[bi], 100.0*prof[bi]/tot);
                    prof[bi] = 0;
                }
            }
            fprintf(stderr, "USPI=%u LOST_INTS=%lu\n", mem[0x30F6] | (mem[0x30F7] << 8), lost_ints);  /* us_per_int the player calibrated */
            fprintf(stderr, "EXIT: L=%02X carry=%d after %lu frames, %lu syscalls, %lu midi port writes, %lu canary writes, %lu bank switches, final bank %u (%s)\n",
                    z.l, z.cf, frames, syscalls, midi_outs, canary_writes, bank_switches, curbank,
                    curbank == (mem[23388] & 7) ? "restored OK" : "NOT RESTORED");
            break;
        }
        if (z.pc == 0x0008) {
            uint16_t ret = RD(z.sp) | (RD(z.sp + 1) << 8);
            z.sp += 2;
            uint8_t call = RD(ret);
            esx(&z, call);
            /* charge realistic wall time for SD work (EMU_READ_T / EMU_SEEK_T,
               T-states per call). The player dismounts its IM2 clock around
               esxdos calls, so /INTs falling inside are counted by nobody —
               exactly the loss mode that streams >64KB files drag from. */
            {
                static long read_t = -1, seek_t = -1, sd_di = 0;
                if (read_t < 0) {
                    const char *r = getenv("EMU_READ_T"), *s = getenv("EMU_SEEK_T");
                    read_t = r ? atol(r) : 0;
                    seek_t = s ? atol(s) : 0;
                    sd_di = getenv("EMU_SD_DI") ? atol(getenv("EMU_SD_DI")) : 0;
                }
                if (call == F_READ) z.cyc += read_t;
                if (call == F_SEEK) z.cyc += seek_t;
                /* walk the frame grid THROUGH the charged time (an atomic cyc
                   jump would bunch the interrupts after the call and skew any
                   in-player time measurement):
                   - EMU_SD_DI=1: the kernel holds DI through the transfer —
                     the /INT pulses die at the source, invisible to ANY
                     handler (the worst real case, seen on the MiSTer image path)
                   - otherwise, with the player's bank-6 clock mounted
                     (curbank==6), each interrupt ticks the bank-side counter
                     exactly like the real ISR would */
                while (z.cyc - last_int > INTT) {
                    last_int += INTT;
                    frames++;
                    if (++mem[23672] == 0 && ++mem[23673] == 0) ++mem[23674];
                    if (sd_di) {
                        lost_ints++;
                        fprintf(stderr, "[di-int] frame %lu\n", frames);
                    } else if (curbank == 6) {
                        banks[6][0x3D7E]++;   /* the bank-resident ISR's counter */
                    } else {
                        lost_ints++;          /* no clock mounted: lost outright */
                        fprintf(stderr, "[im1-lost] frame %lu\n", frames);
                    }
                }
            }
            z.pc = ret + 1;
            continue;
        }
        if (send_trap && z.pc == send_trap) {
            printf("%lu %02X\n", frames, z.a);
        }
        if (z.pc == 0x0010) {
            uint16_t ret = RD(z.sp) | (RD(z.sp + 1) << 8);
            z.sp += 2;
            fputc(z.a == 0x0D ? '\n' : z.a, stderr);
            z.pc = ret;
            continue;
        }
        if (z.iff1 && z.iy != 0x5C3A) {
            static unsigned long iy_hits = 0;
            static uint16_t last_pc = 0;
            if (z.pc != last_pc && iy_hits < 20) {
                fprintf(stderr, "[IY] pc=%04X iy=%04X (interrupts enabled!)\n", z.pc, z.iy);
                last_pc = z.pc;
                iy_hits++;
            }
        }
        {
            static long fill_trap = -1;
            if (fill_trap < 0) { const char *t = getenv("EMU_FILL_PC"); fill_trap = t ? strtol(t, 0, 16) : 0; }
            if (fill_trap && z.pc == (uint16_t)fill_trap)
                fprintf(stderr, "[fill] frame %lu phase=%lums ret=%04X\n", frames,
                        (z.cyc - last_int) / 3500, (unsigned)(RD(z.sp) | (RD(z.sp+1) << 8)));
        }
        if (z.int_pending && z.iff1 && z.interrupt_mode == 1) {
            /* the INT will be serviced by the IM1 stub (esxdos state): the
               player's IM2 counter never sees it — an invisible lost tick */
            fprintf(stderr, "[im1-int] frame %lu pc=%04X\n", frames, z.pc);
        }
        {
            uint16_t ppc = z.pc;
            pcring[pcri] = ppc; pcri = (pcri + 1) & 63;
            if (dbg && steps < 400) fprintf(stderr, "[pc] %04X a=%02X hl=%02X%02X sp=%04X\n", z.pc, z.a, z.h, z.l, z.sp);
            unsigned long c0 = z.cyc;
            int was_halted = z.halted;
            z80_step(&z);
            if (!was_halted) prof[ppc >> 8] += z.cyc - c0;
        }
        steps++;
        if (z.int_pending && z.cyc - int_gen_cyc > 32) {
            z.int_pending = 0;                 /* pulse expired unseen: lost forever */
            lost_ints++;
            fprintf(stderr, "[lost-int] frame %lu pc=%04X\n", frames, z.pc);
        }
        if (z.cyc - last_int > INTT) {         /* one video frame */
            last_int += INTT;                  /* rigid grid: charged SD time may
                                                  span several frames — each fires */
            frames++;
            /* ROM ISR increments FRAMES (23672, 3 bytes) */
            if (++mem[23672] == 0 && ++mem[23673] == 0) ++mem[23674];
            if (frames > max_frames) { fprintf(stderr, "TIMEOUT after %lu frames, %lu syscalls, %lu midi outs\n", frames, syscalls, midi_outs); break; }
            if (frames % 500 == 0 && trace) fprintf(stderr, "[t] frame %lu, midi outs %lu, pc=%04X\n", frames, midi_outs, z.pc);
            z80_gen_int(&z, 0xFF);
            int_gen_cyc = z.cyc;
        }
    }
    return 0;
}
