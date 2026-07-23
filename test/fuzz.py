#!/usr/bin/env python3
"""Randomized differential test: engine (C harness) vs python reference."""
import random, struct, subprocess, sys
from gen_ref import vlq, track, smf1, reference
import check_timing

def rand_track(rng, ch):
    ev = []
    tick_budget = rng.randint(200, 4000)
    t = 0
    last_status = None
    while t < tick_budget:
        d = rng.choice([0, 0, 1, rng.randint(1, 240)])
        t += d
        kind = rng.random()
        if kind < 0.55:
            st = 0x90 | ch
            b = bytes([st, rng.randint(30, 90), rng.randint(1, 127)])
            # sometimes use running status in the file
            if last_status == st and rng.random() < 0.5:
                b = b[1:]
            else:
                last_status = st
            ev.append((d, b))
        elif kind < 0.7:
            st = 0x80 | ch
            b = bytes([st, rng.randint(30, 90), 64])
            if last_status == st and rng.random() < 0.5:
                b = b[1:]
            else:
                last_status = st
            ev.append((d, b))
        elif kind < 0.78:
            ev.append((d, bytes([0xC0 | ch, rng.randint(0, 127)])))
            last_status = 0xC0 | ch
        elif kind < 0.86:
            ev.append((d, bytes([0xE0 | ch, rng.randint(0, 127), rng.randint(0, 127)])))
            last_status = 0xE0 | ch
        elif kind < 0.93:
            txt = bytes(rng.randint(32, 126) for _ in range(rng.randint(0, 200)))
            ev.append((d, bytes([0xFF, rng.choice([1,2,3,4,5,6,7,0x7F])]) + vlq(len(txt)) + txt))
        elif kind < 0.97:
            us = rng.randint(200000, 1200000)
            ev.append((d, bytes([0xFF, 0x51, 0x03]) + struct.pack('>I', us)[1:]))
        else:
            payload = bytes(rng.randint(0, 127) for _ in range(rng.randint(0, 300)))
            if rng.random() < 0.5:
                ev.append((d, bytes([0xF0]) + vlq(len(payload)) + payload))
            else:
                ev.append((d, bytes([0xF7]) + vlq(len(payload)) + payload))
            last_status = None
    return ev

fails = 0
for seed in range(30):
    rng = random.Random(seed)
    ntrk = rng.randint(1, 17)
    ppq = rng.choice([96, 192, 384, 480, 960])
    trks = [track(rand_track(rng, tn % 16)) for tn in range(ntrk)]
    fn = 'fuzz.mid'
    open(fn, 'wb').write(smf1(ppq, trks))
    out = subprocess.run(['./host_test', fn], capture_output=True, text=True).stdout
    actual = [(int(l.split()[0]), bytes(int(x,16) for x in l.split()[1:])) for l in out.splitlines()]
    sim = check_timing.simulate(fn)
    # flatten to a per-byte stream: SendMIDI chunking of long sysex is not a difference
    actual = [(t, b) for t, bb in actual for b in bb]
    sim = [(t, b) for t, bb in sim for b in bb]
    if sim != actual:
        print(f"seed {seed}: MISMATCH ({len(sim)} vs {len(actual)} events)")
        for k, (s, a) in enumerate(zip(sim, actual)):
            if s != a:
                print("  first diff at", k, s, a); break
        fails += 1
    else:
        print(f"seed {seed}: OK ({ntrk} trk, ppq {ppq}, {len(sim)} events)")
print("FAILURES:", fails)
sys.exit(1 if fails else 0)
