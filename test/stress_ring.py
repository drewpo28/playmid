#!/usr/bin/env python3
"""Stress test for the L2 ring buffers: files much larger than the per-track
L2 window, so the ring read/write pointers wrap around multiple times.
Byte- and timing-exact comparison against the python reference, like fuzz.py.
"""
import random, struct, subprocess, sys
from gen_ref import vlq, smf1, track
import check_timing

def smf0(ppq, trk):
    return b'MThd' + struct.pack('>IHHH', 6, 0, 1, ppq) + trk

def big_track(rng, ch, target_bytes):
    """Note stream of roughly target_bytes, mixing dense chords (delta 0),
    normal deltas and occasional long gaps (idle frames -> prefetch slots)."""
    ev, size = [], 0
    while size < target_bytes:
        r = rng.random()
        if r < 0.15:
            d = 0                      # chord
        elif r < 0.9:
            d = rng.randint(1, 60)
        else:
            d = rng.randint(480, 1920) # long gap: lets the prefetcher run
        n = rng.randint(30, 90)
        ev.append((d, bytes([0x90 | ch, n, rng.randint(1, 127)])))
        ev.append((rng.randint(1, 120), bytes([0x80 | ch, n, 64])))
        size += 8
    return ev

def run_case(name, fname):
    out = subprocess.run(['./host_test', fname], capture_output=True, text=True)
    actual = [(int(l.split()[0]), bytes(int(x, 16) for x in l.split()[1:]))
              for l in out.stdout.splitlines()]
    sim = check_timing.simulate(fname)
    actual = [(t, b) for t, bb in actual for b in bb]
    sim = [(t, b) for t, bb in sim for b in bb]
    ok = sim == actual
    if not ok:
        for k, (s, a) in enumerate(zip(sim, actual)):
            if s != a:
                print(f"  first diff at byte {k}: sim {s} actual {a}")
                break
    print(f"{name}: {'OK' if ok else 'MISMATCH'} "
          f"({len(actual)} bytes) [{out.stderr.strip()}]")
    return ok

fails = 0
rng = random.Random(1234)

# case 1: format 0, ~150KB -> single 32KB ring wraps several times
open('stress0.mid', 'wb').write(smf0(480, track(big_track(rng, 0, 150000))))
fails += not run_case('format0 150KB (ring 32K, wraps)', 'stress0.mid')

# case 2: format 1, 2 tracks x ~80KB -> 32KB rings wrap several times
trks = [track(big_track(rng, tn, 80000)) for tn in range(2)]
open('stress1.mid', 'wb').write(smf1(480, trks))
fails += not run_case('format1 2x80KB (rings 32K, wrap)', 'stress1.mid')

# case 3: format 1, 17 tracks x ~10KB -> 2KB rings wrap ~5 times
trks = [track(big_track(rng, tn % 16, 10000)) for tn in range(17)]
open('stress2.mid', 'wb').write(smf1(480, trks))
fails += not run_case('format1 17x10KB (rings 2KB, wrap x5)', 'stress2.mid')

print("FAILURES:", fails)
sys.exit(1 if fails else 0)
