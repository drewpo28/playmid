#!/usr/bin/env python3
"""Generate format-1 SMF test files and the reference merged output.

Reference model matches the engine's contract:
 - merge all tracks by absolute tick; ties broken by track order (lower index first)
 - channel events are emitted with explicit status (no wire running status)
 - F0 sysex -> F0 byte then payload; F7 -> payload only
 - meta events produce no output; FF 51 changes tempo globally; FF 2F ends track
Output lines: "<tick> <hex bytes...>" (tick = absolute MIDI tick of the event).
The C harness prints interrupt counts, which we convert to expected ticks by
replicating the fixed-point clock: per-int increment = floor(64*20000*ppq/us_per_quarter)
... but for byte-stream comparison we only compare the BYTES sequence, and check
timing separately with a tolerance.
"""
import sys, struct, random

def vlq(n):
    out = [n & 0x7F]
    n >>= 7
    while n:
        out.append(0x80 | (n & 0x7F))
        n >>= 7
    return bytes(reversed(out))

def track(events):
    """events: list of (delta, bytes) ; auto-appends end of track"""
    data = b''.join(vlq(d) + b for d, b in events)
    data += vlq(0) + bytes([0xFF, 0x2F, 0x00])
    return b'MTrk' + struct.pack('>I', len(data)) + data

def smf1(ppq, tracks):
    return b'MThd' + struct.pack('>IHHH', 6, 1, len(tracks), ppq) + b''.join(tracks)

def note_on(ch, n, v):  return bytes([0x90 | ch, n, v])
def note_off(ch, n):    return bytes([0x80 | ch, n, 64])
def tempo(us):          return bytes([0xFF, 0x51, 0x03]) + struct.pack('>I', us)[1:]
def meta_text(s):       return bytes([0xFF, 0x01]) + vlq(len(s)) + s.encode()
def prog(ch, p):        return bytes([0xC0 | ch, p])
def sysex_f0(payload):  return bytes([0xF0]) + vlq(len(payload)) + payload

# ---------------- test file 1: the tricky one ----------------
ppq = 480
random.seed(42)

# track 0: conductor, tempo changes mid-song
t0 = [(0, tempo(500000)), (0, meta_text("conductor")),
      (960, tempo(250000)),      # tick 960: double speed
      (960, tempo(1000000))]     # tick 1920: half speed

# track 1: running status inside the track + chord at tick 0, long enough to
# cross cache boundaries: many notes
t1 = [(0, note_on(0, 60, 100))]
t1.append((0, bytes([64, 100])))          # running status: note 64 on
t1.append((0, bytes([67, 100])))          # running status: note 67 on
tick = 0
for k in range(120):                       # long stream -> crosses 192B cache many times
    t1.append((120, note_off(0, 60 + (k % 12))))
    t1.append((0,   note_on(0, 60 + ((k+1) % 12), 90)))

# track 2: chord smeared across tracks (delta 0 aligned with t1 & t3), prog change, sysex
t2 = [(0, prog(1, 42)), (0, note_on(1, 40, 80)),
      (480, sysex_f0(bytes([0x7E, 0x7F, 0x09, 0x01]))),
      (480, note_off(1, 40))]

# track 3: sparse events, ends much later
t3 = [(0, note_on(2, 36, 127)), (1920, note_off(2, 36)), (1920, note_on(2, 38, 60)),
      (480, note_off(2, 38))]

f1 = smf1(ppq, [track(t0), track(t1), track(t2), track(t3)])
open('test1.mid', 'wb').write(f1)

# ---------------- test file 2: 15 tracks, delta-0 chord across all ----------------
trks = []
for tn in range(15):
    ev = [(0, note_on(tn % 16, 40 + tn, 100)),
          (480, note_off(tn % 16, 40 + tn)),
          (240 * tn, note_on(tn % 16, 52 + tn, 70)),
          (240, note_off(tn % 16, 52 + tn))]
    trks.append(track(ev))
f2 = smf1(192, trks)
open('test2.mid', 'wb').write(f2)

# ---------------- reference merger ----------------
def parse_track(data):
    """yield (abstick, kind, payload) ; kind: 'ch','sysex0','sysex7','meta'"""
    pos, tick, status = 0, 0, None
    out = []
    while pos < len(data):
        delta = 0
        while True:
            b = data[pos]; pos += 1
            delta = (delta << 7) | (b & 0x7F)
            if not (b & 0x80): break
        tick += delta
        b = data[pos]
        if b & 0x80:
            status = b; pos += 1
        st = status
        if st == 0xF0 or st == 0xF7:
            ln = 0
            while True:
                x = data[pos]; pos += 1
                ln = (ln << 7) | (x & 0x7F)
                if not (x & 0x80): break
            payload = data[pos:pos+ln]; pos += ln
            out.append((tick, 'sysex0' if st == 0xF0 else 'sysex7', payload))
        elif st == 0xFF:
            typ = data[pos]; pos += 1
            ln = 0
            while True:
                x = data[pos]; pos += 1
                ln = (ln << 7) | (x & 0x7F)
                if not (x & 0x80): break
            payload = data[pos:pos+ln]; pos += ln
            out.append((tick, 'meta', bytes([typ]) + payload))
            if typ == 0x2F:
                break
        else:
            hi = st & 0xF0
            nd = 1 if hi in (0xC0, 0xD0) else 2
            payload = bytes([st]) + data[pos:pos+nd]; pos += nd
            out.append((tick, 'ch', payload))
    return out

def reference(fname):
    raw = open(fname, 'rb').read()
    ntrk = struct.unpack('>H', raw[10:12])[0]
    pos = 14
    tracks = []
    for _ in range(ntrk):
        assert raw[pos:pos+4] == b'MTrk'
        ln = struct.unpack('>I', raw[pos+4:pos+8])[0]
        tracks.append(parse_track(raw[pos+8:pos+8+ln]))
        pos += 8 + ln
    # merge: (tick, track_index, seq) ordering == engine's min-scan with tie by track index
    merged = []
    idx = [0]*len(tracks)
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
        if kind == 'ch':
            merged.append((tick, payload))
        elif kind == 'sysex0':
            merged.append((tick, bytes([0xF0])))
            # engine sends payload in chunks of <=128; byte sequence identical
            if payload: merged.append((tick, payload))
        elif kind == 'sysex7':
            if payload: merged.append((tick, payload))
        # meta: no output
    return merged

for fname in ('test1.mid', 'test2.mid'):
    with open(fname.replace('.mid', '.ref'), 'w') as f:
        for tick, data in reference(fname):
            f.write('%d %s\n' % (tick, ' '.join('%02X' % b for b in data)))
print("generated test1.mid test2.mid + refs")
