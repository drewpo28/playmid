# playmid test harness

Host-side verification of the playback engine (the code between the
`FORMAT1 ENGINE BEGIN/END` markers in `playmid.c`).

```sh
# 1. extract the engine verbatim from the shipped source
sed -n '/FORMAT1 ENGINE BEGIN/,/FORMAT1 ENGINE END/p' ../playmid.c > engine.inc

# 2. host harness: plays a .mid, prints "<50Hz-interrupt-count> <event bytes...>" per SendMIDI
gcc -w -O1 -o host_test host_test.c
./host_test file.mid

# 3. generate fixture files + reference merge (test1.mid, test2.mid)
python3 gen_ref.py

# 4. exact fixed-point timing check of host_test output
python3 check_timing.py file.mid file.out

# 5. randomized differential test (30 seeds) engine-vs-reference
python3 fuzz.py

# 6. run the actual PLAYMID binary under Z80 emulation with esxdos traps
#    (needs z80.c/z80.h from https://github.com/superzazu/z80)
gcc -O2 -o emu emu.c z80.c
./emu ../PLAYMID file.mid [max_frames] [SendMIDIByte-addr-hex from .map] [start-with-DI]
```

`emu` prints one line per MIDI byte (`<frame> <hex>`) when given the
`_SendMIDIByte` address from the linker map, so the real Z80 binary's output
can be diffed against `host_test`'s.
