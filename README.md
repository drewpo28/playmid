# playmid

A tiny MIDI player (file formats 0 and 1) for the ZX Spectrum 128K. It uses the MIDI OUT connector available in those models. Also works in the ZX-UNO with the MIDI plugin (or compatible).

## Format 1 support

Format 1 (multi-track) files are played by merging all tracks on the fly. A table of track start offsets is built from the MTrk chunk headers, and each track keeps its own file offset, running status, absolute next-event tick and a private read cache. The main loop always services the track whose next event is earliest (minimum next_tick), refilling its cache when needed. Tempo meta events (FF 51) apply globally, which is exactly what the tick-based merge requires. Up to 17 tracks are supported (a typical format 1 file has a tempo track plus up to 16 channel tracks). Format 0 files are played with the same engine (single track, one big sequential cache).

Files of any size are streamed through a two-level cache: each track owns a window in the 128K RAM banks 1/3/4/6 (64KB shared, port 0x7FFD) that is refilled from the SD card in large sequential reads with a single seek per window, and the small per-track L1 caches in the player's buffer are refilled from those windows with fast RAM copies. This cuts the seek count by two orders of magnitude compared to seeking on every small cache refill (an esxdos seek walks the FAT cluster chain and costs milliseconds). On frames where no event fires, the player prefetches the most-depleted window in advance, so SD refills land in the quiet gaps of the music instead of piling up on top of dense passages (all windows are filled together at start and would otherwise run dry together too). Note that using the banks sacrifices the 128 BASIC RAM disk contents. While a foreign bank is paged at 0xC000 the player never touches the stack or the frame pointer (BASIC's stack usually lives up there) and interrupts are disabled; the previous banking state is restored from BANKM after every copy. During any esxdos call interrupts are fully disabled, because the kernel pages its own bank over the player's IM2 vector table.

Everything lives inside the 1KB buffer at 0x3000: 48 bytes of output staging, the IM2 handler and its 257-byte vector table, and the per-track read caches. The player deliberately touches nothing else: not the DivMMC page above 0x33FF and not the screen — command launchers such as the LNF Browser keep live state in both, and writing there hangs or resets the machine on return. Interrupts are explicitly enabled at startup since some launchers pass control with them disabled. Playback time is kept the way zx-midiplayer does it: a tiny IM2 interrupt handler (vector table and ISR live inside the player's buffer) counts every 50Hz interrupt, and the scheduler catches up on all frames that elapsed while events were being parsed or sent — so the tempo never drags behind during dense passages, no matter how long a burst takes. The FRAMES sysvar can't be used for this because it is frozen while a dot command runs. Around esxdos calls the player temporarily returns to IM1. On exit the player sends All Sound Off + All Notes Off on all 16 channels so no notes are left hanging. The border is green while playing and turns blue on a bad file (the player prints nothing: under launchers like the LNF Browser the BASIC screen channel is invalid and printing resets the machine).

The scheduler follows zx-midiplayer's shape: once per frame every track is visited with a single "is it due yet?" comparison, and a due track drains all its pending events in one go until its next event lies in the future. There is no minimum-tick search at all, so in dense chords the per-event cost is just parsing and the wire. Draining a track's events consecutively also keeps output running status alive longer (the status byte is only sent when it changes), which cuts wire traffic noticeably. Per-track hot state is mirrored into globals on track switch instead of indexing arrays on every byte.

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
