#!/usr/bin/env python3
"""Replicate the engine's fixed-point clock and compare interrupt counts
per emitted MIDI line against the C harness output."""
import sys, struct
from gen_ref import parse_track

PRECISION = 64

def simulate(fname):
    raw = open(fname, 'rb').read()
    ppq = struct.unpack('>H', raw[12:14])[0]
    ntrk = struct.unpack('>H', raw[10:12])[0]
    pos = 14
    tracks = []
    for _ in range(ntrk):
        ln = struct.unpack('>I', raw[pos+4:pos+8])[0]
        tracks.append(parse_track(raw[pos+8:pos+8+ln]))
        pos += 8 + ln

    ticks_per_int = PRECISION * 20 * ppq // 500
    now = 0          # scaled
    ints = 0
    idx = [0]*len(tracks)
    out = []
    while True:
        best, besttick = -1, None
        for ti, tr in enumerate(tracks):
            if idx[ti] >= len(tr): continue
            tk = tr[idx[ti]][0]
            if besttick is None or tk < besttick:
                best, besttick = ti, tk
        if best < 0: break
        tick, kind, payload = tracks[best][idx[best]]
        idx[best] += 1
        target = tick * PRECISION
        while now < target:
            ints += 1
            now += ticks_per_int
        if kind == 'ch':
            out.append((ints, payload))
        elif kind == 'sysex0':
            out.append((ints, bytes([0xF0])))
            if payload: out.append((ints, payload))
        elif kind == 'sysex7':
            if payload: out.append((ints, payload))
        elif kind == 'meta':
            if payload[0] == 0x51 and len(payload) == 4:
                us = int.from_bytes(payload[1:4], 'big')
                if us:
                    ticks_per_int = PRECISION * 20000 * ppq // us
    return out

if __name__ == '__main__':
    fname, outname = sys.argv[1], sys.argv[2]
    sim = simulate(fname)
    actual = [l.split() for l in open(outname)]
    assert len(sim) == len(actual), f"line count differs: {len(sim)} vs {len(actual)}"
    bad = 0
    for k, ((si, sb), al) in enumerate(zip(sim, actual)):
        ai = int(al[0]); ab = bytes(int(x, 16) for x in al[1:])
        if sb != ab or si != ai:
            print(f"line {k}: sim ({si} {sb.hex()}) != actual ({ai} {ab.hex()})")
            bad += 1
            if bad > 10: break
    print("TIMING OK" if bad == 0 else f"{bad}+ mismatches", f"({len(sim)} events)")
