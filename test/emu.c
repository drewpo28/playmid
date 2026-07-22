/* Run the PLAYMID dot-command binary under Z80 emulation with esxdos syscall traps.
 * Usage: ./emu PLAYMID file.mid [max_frames]
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "z80.h"

static uint8_t mem[65536];
static FILE *fhs[16];
static unsigned long midi_outs = 0, frames = 0, syscalls = 0;
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

static uint8_t rb(void *ud, uint16_t a) { (void)ud; return mem[a]; }
static unsigned long canary_writes = 0;
static void wb(void *ud, uint16_t a, uint8_t v) {
    (void)ud;
    if (a >= 0x3400 && a <= 0x3FFF && canary_writes++ < 5)
        fprintf(stderr, "[CANARY] write %02X to %04X\n", v, a);
    mem[a] = v;
}
static uint8_t pin(z80 *z, uint8_t port) { (void)z; (void)port; return 0xFF; }
static void pout(z80 *z, uint8_t port, uint8_t v) {
    (void)z; (void)v;
    if (port == 0xFD) midi_outs++;   /* AY / MIDI bit-bang activity */
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
        while (k < 63 && mem[p]) name[k++] = mem[p++];
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
    default:
        fprintf(stderr, "[esx] UNIMPLEMENTED call %02X\n", call);
        z->a = 1; z->cf = 1; break;
    }
}

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
    uint16_t cl = 0x9000;
    strcpy((char *)mem + cl, argv[2]);
    mem[cl + strlen(argv[2])] = 0x0D;

    /* IM1 handler stub: EI ; RET */
    mem[0x0038] = 0xFB; mem[0x0039] = 0xC9;

    z80 z;
    z80_init(&z);
    z.read_byte = rb; z.write_byte = wb; z.port_in = pin; z.port_out = pout;
    z.pc = 0x2000;
    z.sp = 0xFF00 - 2;
    mem[0xFEFE] = 0x00; mem[0xFEFF] = 0x00;   /* return-to-0 sentinel */
    z.h = cl >> 8; z.l = cl & 0xFF;
    z.iy = 0x5C3A;
    z.iff1 = z.iff2 = !start_di;
    z.interrupt_mode = 1;

    unsigned long last_int = 0;
    unsigned long steps = 0;
    while (1) {
        if (z.pc == 0x0000) {
            fprintf(stderr, "EXIT: L=%02X carry=%d after %lu frames, %lu syscalls, %lu midi port writes, %lu canary writes\n",
                    z.l, z.cf, frames, syscalls, midi_outs, canary_writes);
            break;
        }
        if (z.pc == 0x0008) {
            uint16_t ret = mem[z.sp] | (mem[z.sp + 1] << 8);
            z.sp += 2;
            uint8_t call = mem[ret];
            esx(&z, call);
            z.pc = ret + 1;
            continue;
        }
        if (send_trap && z.pc == send_trap) {
            printf("%lu %02X\n", frames, z.a);
        }
        if (z.pc == 0x0010) {
            uint16_t ret = mem[z.sp] | (mem[z.sp + 1] << 8);
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
        z80_step(&z);
        steps++;
        if (z.cyc - last_int > 69888) {        /* 50 Hz frame */
            last_int = z.cyc;
            frames++;
            if (frames > max_frames) { fprintf(stderr, "TIMEOUT after %lu frames, %lu syscalls, %lu midi outs\n", frames, syscalls, midi_outs); break; }
            if (frames % 500 == 0 && trace) fprintf(stderr, "[t] frame %lu, midi outs %lu, pc=%04X\n", frames, midi_outs, z.pc);
            z80_gen_int(&z, 0xFF);
        }
    }
    return 0;
}
