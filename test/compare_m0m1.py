#!/usr/bin/env python3
"""m0.mid (format 0) and m1.mid (format 1) contain the same music.
Compare: per-tick multisets of channel events + tempo maps must match."""
import struct, sys
from collections import Counter
from gen_ref import parse_track

def load(fname):
    raw = open(fname, 'rb').read()
    ntrk = struct.unpack('>H', raw[10:12])[0]
    pos = 14
    tracks = []
    for _ in range(ntrk):
        assert raw[pos:pos+4] == b'MTrk', f"{fname}: chunk at {pos} is not MTrk"
        ln = struct.unpack('>I', raw[pos+4:pos+8])[0]
        tracks.append(parse_track(raw[pos+8:pos+8+ln]))
        pos += 8 + ln
    return tracks

def flatten(tracks):
    ch = Counter()      # (tick, payload) for channel+sysex events
    tempo = []          # (tick, us)
    end = 0
    for tr in tracks:
        for tick, kind, payload in tr:
            if kind == 'ch' or kind.startswith('sysex'):
                ch[(tick, kind, bytes(payload))] += 1
            elif kind == 'meta' and payload[0] == 0x51:
                tempo.append((tick, int.from_bytes(payload[1:4], 'big')))
            end = max(end, tick)
    tempo.sort()
    return ch, tempo, end

c0, t0, e0 = flatten(load('/home/drew/playmid/m0.mid'))
c1, t1, e1 = flatten(load('/home/drew/playmid/m1.mid'))

print(f"m0: {sum(c0.values())} events, {len(t0)} tempo changes, last tick {e0}")
print(f"m1: {sum(c1.values())} events, {len(t1)} tempo changes, last tick {e1}")
d = (c0 - c1) + (c1 - c0)
if d:
    print("EVENT DIFFS:")
    for k, v in list(d.items())[:15]:
        print("  ", k, "x", v, "(m0 only)" if c0[k] > c1[k] else "(m1 only)")
else:
    print("CHANNEL/SYSEX EVENTS IDENTICAL")
print("TEMPO MAPS", "IDENTICAL" if t0 == t1 else f"DIFFER: {t0[:5]} vs {t1[:5]}")
