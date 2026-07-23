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
    tracks = tracks[:17]                     # MAX_TRACKS

    def tpi_calc(us):
        # mirror of the engine's settempo: 16.16 ticks per frame, split into an
        # integer part at PRECISION=64 plus an 8-bit fraction (1/256ths of a unit)
        d = us // ppq
        if d == 0:
            d = 1
        t = (20000 << 16) // d
        return [t >> 10, (t >> 2) & 0xFF]

    ticks_per_int = tpi_calc(500000)         # [integer, fraction/1024]
    tfrac = [0]
    now = 0          # scaled
    ints = 0
    idx = [0]*len(tracks)
    out = []
    wire = [None]    # wire running status

    def emit(kind, payload):
        if kind == 'ch':
            if payload[0] == wire[0]:
                out.append((ints, payload[1:]))
            else:
                wire[0] = payload[0]
                out.append((ints, payload))
        elif kind == 'sysex0':
            wire[0] = None
            out.append((ints, bytes([0xF0])))
            if payload: out.append((ints, payload))
        elif kind == 'sysex7':
            wire[0] = None
            if payload: out.append((ints, payload))
        elif kind == 'meta':
            if payload[0] == 0x51 and len(payload) == 4:
                us = int.from_bytes(payload[1:4], 'big')
                if us:
                    ticks_per_int[:] = tpi_calc(us)

    # frame-sweep scheduler, mirror of the engine (zx-midiplayer style):
    # once per frame visit every track in order; a due track drains all its
    # pending events until its next tick is in the future
    live = sum(1 for tr in tracks if tr)
    for ti, tr in enumerate(tracks):
        if not tr: live -= 0
    while live > 0:
        ints += 1
        now += ticks_per_int[0]
        tfrac[0] += ticks_per_int[1]
        if tfrac[0] >= 256:                  # 8-bit wrap carries a PRECISION unit, like the engine
            tfrac[0] -= 256
            now += 1
        for ti, tr in enumerate(tracks):
            if idx[ti] >= len(tr):
                continue
            if tr[idx[ti]][0] * PRECISION > now:
                continue
            while idx[ti] < len(tr) and tr[idx[ti]][0] * PRECISION <= now:
                tick, kind, payload = tr[idx[ti]]
                idx[ti] += 1
                emit(kind, payload)
                if kind == 'meta' and payload[0] == 0x2F:
                    idx[ti] = len(tr)
            if idx[ti] >= len(tr):
                live -= 1
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
