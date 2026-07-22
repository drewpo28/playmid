# playmid

A tiny MIDI player (file formats 0 and 1) for the ZX Spectrum 128K. It uses the MIDI OUT connector available in those models. Also works in the ZX-UNO with the MIDI plugin (or compatible).

## Format 1 support

Format 1 (multi-track) files are played by merging all tracks on the fly. A table of track start offsets is built from the MTrk chunk headers, and each track keeps its own file offset, running status, absolute next-event tick and a private read cache. The main loop always services the track whose next event is earliest (minimum next_tick), reloading its cache with esxdos `F_SEEK` + `F_READ` when needed. Tempo meta events (FF 51) apply globally, which is exactly what the tick-based merge requires. Up to 24 tracks are supported; the 1KB buffer at 0x3000 holds a 64-byte output staging area plus 960 bytes of read caches split evenly between the tracks, so files with fewer tracks get bigger caches and fewer seeks. Format 0 files are played with the same engine (single track, one 960-byte sequential cache). Nothing above 0x33FF is touched at runtime — command launchers such as the LNF Browser keep their own state in the top of the DivMMC page, and interrupts are explicitly enabled at startup since some launchers pass control with them disabled.

Since events from different tracks are interleaved on the wire, the status byte is always sent explicitly (per-track running status is honoured while parsing, but never relied upon on output).

## Building

Requires SDCC (3.x or 4.x) with the Z80 backend:

```sh
sdcc -mz80 --reserve-regs-iy --opt-code-size --max-allocs-per-node 10000 \
  --nostdlib --nostdinc --no-std-crt0 --code-loc 0x2000 --data-loc 0x2d00 \
  playmid.c z80.lib -L /path/to/sdcc/lib/z80
makebin -s 65535 -p playmid.ihx playmid.bin
dd if=playmid.bin of=PLAYMID bs=1 skip=8192
```

Do **not** pass `--sdcccall 0` on SDCC 4.2+: the bundled `z80.lib` is built with the default register calling convention, and the 32-bit multiply/divide helpers silently return garbage if the compiler passes their arguments on the stack. The functions containing inline assembly are individually marked `__sdcccall(0)` in the source instead. Check in the `.map` file that `_CODE` ends below `--data-loc` and `_HOME` ends below 0x3000.

Copy `PLAYMID` to `/BIN` on the SD card and run it as an esxdos dot command:

```text
.playmid tune.mid
```

Press SPACE to stop playback.
