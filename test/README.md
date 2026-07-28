# playmid test harness

Host-side verification of the playback engine (the code between the
`FORMAT1 ENGINE BEGIN/END` markers in `playmid.c`).

`engine.inc` is the C model of the engine. It used to be extracted verbatim
from `playmid.c`; since the engine was rewritten in assembly it is maintained
BY HAND as an exact mirror — every semantic change to the asm between the
`FORMAT1 ENGINE BEGIN/END` markers must be mirrored there (the asm functions
carry their C shape in comments), or the differential tests below lose their
meaning.

```sh
# 1. host harness: plays a .mid, prints "<50Hz-interrupt-count> <event bytes...>" per SendMIDI
gcc -w -O1 -o host_test host_test.c
./host_test file.mid

# 2. generate fixture files + reference merge (test1.mid, test2.mid)
python3 gen_ref.py

# 3. exact fixed-point timing check of host_test output
python3 check_timing.py file.mid file.out

# 4. randomized differential test (30 seeds) engine-vs-reference
python3 fuzz.py

# 4b. L2 ring-buffer stress: files far larger than the per-track bank window,
#     so the ring pointers wrap around several times (byte- and timing-exact)
python3 stress_ring.py

# 5. run the actual PLAYMID binary under Z80 emulation with esxdos traps
#    (needs z80.c/z80.h from https://github.com/superzazu/z80)
gcc -O2 -o emu emu.c z80.c
./emu ../PLAYMID file.mid [max_frames] [SendMIDIByte-addr-hex from .map] [start-with-DI]

# 5b. same, but with the REAL ULA /INT behaviour: the pulse lasts ~32 T-states
#     and is NOT latched, so an interrupt arriving while the player has
#     interrupts disabled (MIDI bit-banging) is lost forever. emu.c latches
#     interrupts until EI and therefore cannot see clock-drift bugs; this
#     variant reproduced the "drags behind ZMP" drift and verifies the fix.
#     Prints "[lost-int] frame N pc=XXXX" per eaten interrupt and a LOST_INTS
#     total; compare total frames against a plain emu run of the same file —
#     the difference is the net clock drift.
gcc -O2 -DINTT=71680 -o emu_pulse emu_pulse.c z80.c
./emu_pulse ../PLAYMID file.mid [max_frames] [SendMIDIByte-addr-hex] [start-with-DI]

# 5c. emu_pulse also charges wall time for esxdos calls (T-states per call) and
#     can model a kernel that holds DI through the transfer, where the /INT
#     pulse dies invisibly to ANY handler (the MiSTer image-path case):
EMU_READ_T=21000 EMU_SEEK_T=42000 ./emu_pulse ...            # 6ms reads, 12ms seeks
EMU_SD_DI=1 EMU_READ_T=21000 EMU_SEEK_T=42000 ./emu_pulse ...# same, kernel holds DI
#     EMU_WATCH_CNT=1 logs every clock credit; EMU_FILL_PC=<addr> logs each
#     l2_fillstep entry with its frame phase. Measure in-body drift as the
#     first-to-last-event frame span against a zero-cost run of the same build.
```

`emu` prints one line per MIDI byte (`<frame> <hex>`) when given the
`_SendMIDIByte` address from the linker map, so the real Z80 binary's output
can be diffed against `host_test`'s. Build it with `-DINTT=71680` (Pentagon)
or `70908` (128K) to emulate other frame lengths and exercise the player's
frame calibration; it reports the calibrated value as `USPI=...` on exit, and
`USPI=<value> ./host_test file.mid` makes the harness use the same frame
length so the two byte streams compare byte-for-byte even on dense files.
