# playmid

A tiny MIDI player (file formats 0 and 1) for the ZX Spectrum 128K, running as an esxdos dot command. It uses the MIDI OUT connector available in those models. Also works in the ZX-UNO with the MIDI plugin (or compatible).

Files of any size can be played: the file is streamed from the SD card, never loaded whole. Up to 17 tracks are merged on the fly (a typical format 1 file has a tempo track plus up to 16 channel tracks); format 0 is simply the single-track case of the same engine. Tempo meta events (FF 51) from any track apply globally.

## How it works

**Streaming through a two-level cache.** Each track owns a ring buffer in the 128K RAM banks 1/3/4/6, sized to the largest power of two such that all tracks fit in the 64KB (a power of two makes every ring-wrap a masking operation — there is no spare room for multiply/divide code). All rings are filled completely before the clock starts, so files that fit in the banks never touch the SD again. During playback the rings are topped up *incrementally*, in bounded 128-byte steps (a few milliseconds each) scheduled into frames where no event fires, so SD activity hides in the quiet gaps of the music. Because an esxdos `F_SEEK` walks the FAT cluster chain and costs real milliseconds, the steps are batched into long sequential *bursts*: the prefetcher sticks with the track of the last SD read (sequential reads need no seek at all) until its ring is full, and only starts another burst for a ring drained below half. A whole window is never reloaded in one go: that stalls playback for several frames and is audible as a glitch — if a ring runs dry mid-passage, the player borrows a single bounded step and moves on. From the rings, small per-track L1 caches inside the player's buffer are refilled with fast RAM copies. Using the banks sacrifices the 128 BASIC RAM disk contents, and working 128K paging is required (the machine must not be locked in 48K mode).

**Scheduler** (the shape is borrowed from [zx-midiplayer](https://github.com/drewpo28/zx-midiplayer)): once per frame every track is visited with a single "is it due yet?" comparison, and a due track drains all its pending events in one go until its next event lies in the future. There is no minimum-tick search at all, so in dense chords the per-event cost is just parsing and the wire. Draining a track's events consecutively also keeps output running status alive longer (the status byte is only sent when it changes), which cuts wire traffic noticeably. Per-track hot state is mirrored into globals on track switch instead of indexing arrays on every byte.

**Clock** (also zx-midiplayer's mechanism): a tiny IM2 interrupt handler — the vector table and the ISR live inside the player's buffer — counts every frame interrupt, and the scheduler catches up on all frames that elapsed while events were being parsed or sent, so the tempo never drags behind during dense passages. The FRAMES sysvar cannot be used for this: it is frozen while a dot command runs. Around every esxdos call the player switches back to the bone-stock interrupt environment (IM1, I=0x3F); this proved to be the only variant that neither resets (plain IM2 — the kernel pages its bank over the vector table) nor hangs (interrupts disabled across the call — the kernel needs them on some paths). The table and ISR are constant data loaded with the binary itself and survive the calls (the kernel unmaps the page, it doesn't corrupt it), so the switch is nearly free; frames elapsing inside an esxdos call are still invisible to the clock, which is the other reason SD accesses are kept to short bounded steps and few seeks.

Four further clock details keep the tempo honest:

- *Fractional tick rate*: the ticks-per-frame value is computed like zx-midiplayer's — microseconds-per-MIDI-tick first, then a 16.16 fixed-point ticks-per-frame — and the sub-integer remainder is accumulated every frame. A single truncated integer rate (the original design) was up to 2.3% slow for low-resolution files (ppq 24 at 60bpm: 30.72 → 30), which was audible as the tempo dragging in *some* MIDIs while high-ppq files played fine.

- *Frame-length calibration* (zx-midiplayer does the same): at startup the player counts a known-cycle loop across two interrupts and derives the machine's true frame duration, instead of assuming 20ms. A Pentagon frame is 71680 T-states (20.48ms) — assuming 50Hz made everything play 2.4% slow there; the 128K family's 3.5469MHz crystal is recognised by its reading and snapped to the exact 19.992ms.
- *Interrupt-safe bank copies*: the /INT pulse lasts only ~32 T-states and is not latched, so any interrupt arriving while interrupts are disabled is lost — and every lost frame delays the whole song by 20ms. Bank copies (a millisecond of `ldir` per L1 refill) used to hide behind DI; instead the player now switches SP to a scratch stack inside the always-mapped DivMMC page, so interrupts stay enabled while a foreign bank is paged in.
- *A send-burst tick guard*: MIDI bytes are bit-banged with interrupts disabled (~371µs each — an ISR would corrupt the bit timing), so in dense passages a long send chain can swallow the frame interrupt entirely. The player counts wire bytes since the last accounted frame; if more than a frame's worth went out and the counter still hasn't moved, the tick provably fell inside a send and is credited back. The check is self-verifying — if the interrupt landed in one of the enabled gaps between bytes, the counter differs and nothing is credited.

**Bank switching discipline.** While a foreign bank is paged at 0xC000 the player never touches the caller's stack or the frame pointer (BASIC's stack usually lives up there): all parameters are read first and SP is switched to a scratch stack inside the DivMMC page for the duration of the copy, which is what lets interrupts stay enabled. The previous banking state is restored from BANKM after every copy. Only banks 1/3/4/6 are ever paged — never 5 (the screen lives there), 7 (shadow screen), 2 or 0.

**Memory discipline.** Everything lives inside the player's 4KB code/data budget plus the 1KB buffer at 0x3000 (output staging, IM2 handler and its 257-byte vector table, L2 read staging, per-track L1 caches). The player deliberately touches nothing else: not the DivMMC page above 0x33FF and not the screen — command launchers such as the LNF Browser keep live state in both, and writing there hangs or resets the machine on return. Nothing is ever printed on the success path (under such launchers the BASIC screen channel is invalid and printing via RST 16 resets the machine); instead the border shows the state: green while a format 0 file plays, yellow for format 1, blue on a bad file, restored on exit. Interrupts are explicitly enabled at startup since some launchers pass control with them disabled.

**On exit** the player sends All Sound Off + All Notes Off on all 16 channels, so no notes are left hanging.

## Usage

Copy `PLAYMID` to `/BIN` on the SD card and run it as an esxdos dot command (directly or from a file browser with an extension mapping):

```text
.playmid tune.mid
```

Press SPACE to stop playback. The border is green while a format 0 file plays and yellow for format 1. A short buffering pause before the music starts is normal — the cache rings of all tracks are filled up front, so with typical files the SD card is barely touched (or not at all) while the music plays.

## Building

Requires SDCC 4.x with the Z80 backend (`makebin` ships with it):

```sh
sdcc -mz80 --reserve-regs-iy --opt-code-size --max-allocs-per-node 100000 \
  --nostdlib --nostdinc --no-std-crt0 --code-loc 0x2000 --data-loc 0x2eb0 \
  playmid.c z80.lib -L /path/to/sdcc/lib/z80
makebin -s 65535 -p playmid.ihx playmid.bin
dd if=playmid.bin of=PLAYMID bs=1 skip=8192
```

Do **not** pass `--sdcccall 0` on SDCC 4.2+: the bundled `z80.lib` is built with the default register calling convention, and the 32-bit multiply/divide helpers silently return garbage if the compiler passes their arguments on the stack. The functions containing inline assembly are individually marked `__sdcccall(0)` in the source instead. After any change, check in the `.map` file that `_CODE` ends below `--data-loc` and `_HOME` ends below 0x3000, adjusting `--data-loc` if needed (the whole dot command must fit in 0x2000-0x2FFF plus the buffer at 0x3000 — it is packed to within a handful of bytes, which is also why several scalar globals live at absolute addresses inside the buffer page).

## Testing

The playback engine is testable off-target — see [test/README.md](test/README.md). The engine code between the `FORMAT1 ENGINE BEGIN/END` markers is extracted verbatim into a gcc harness and fuzzed byte-exact against an independent Python reference model, and the final `PLAYMID` binary is run in a Z80 emulator with esxdos syscall traps, a faithful 128K memory model (bank 5 aliases the screen) and canaries on all forbidden memory regions.
