#!/usr/bin/env python3
# Rewrites an SF2 with every sample above TARGET Hz resampled to TARGET Hz (polyphase low-pass FIR).
# All other chunks and samples are copied unchanged; loop points are scaled to the new rate.
import struct, sys
from math import gcd
import numpy as np
from scipy.signal import resample_poly

TARGET = 48000
PAD = 46  # zero samples after each sample, as required by the SF2 spec

src, dst = sys.argv[1], sys.argv[2]
f = open(src, 'rb')

def chunks(start, end):
    pos = start
    while pos + 8 <= end:
        f.seek(pos)
        cid, size = struct.unpack('<4sI', f.read(8))
        yield cid, pos + 8, size
        pos += 8 + size + (size & 1)

_, total, _ = struct.unpack('<4sI4s', f.read(12))
top = []  # (list type, [(cid, off, size)])
for cid, off, size in chunks(12, 8 + total):
    assert cid == b'LIST'
    f.seek(off)
    kind = f.read(4)
    top.append((kind, list(chunks(off + 4, off + size))))

sdta = dict((c, (o, s)) for c, o, s in dict(top)[b'sdta'])
assert b'sm24' not in sdta, '24-bit SoundFonts are not supported'
smpl_off, _ = sdta[b'smpl']
pdta = dict((c, (o, s)) for c, o, s in dict(top)[b'pdta'])
f.seek(pdta[b'shdr'][0])
raw = f.read(pdta[b'shdr'][1])
shdr = [list(struct.unpack_from('<20sIIIIIbbHH', raw, i * 46)) for i in range(len(raw) // 46)]

def read_samples(start, end):
    f.seek(smpl_off + start * 2)
    return np.frombuffer(f.read((end - start) * 2), dtype='<i2')

# Build the new sample data in file order
out = open(dst + '.smpl.tmp', 'w+b')
order = sorted(range(len(shdr) - 1), key=lambda i: shdr[i][1])
prev_end = 0
pos = 0
converted = 0
for i in order:
    name, start, end, ls, le, rate = shdr[i][:6]
    assert start >= prev_end, f'overlapping sample data at sample {i}'
    prev_end = end
    data = read_samples(start, end)
    if rate > TARGET and end > start:
        g = gcd(rate, TARGET)
        up, down = TARGET // g, rate // g
        y = resample_poly(data.astype(np.float64), up, down, window=('kaiser', 10.0))
        data = np.clip(np.round(y), -32768, 32767).astype('<i2')
        scale = TARGET / rate
        ls = min(round((ls - start) * scale), len(data))
        le = min(round((le - start) * scale), len(data))
        shdr[i][5] = TARGET
        converted += 1
    else:
        ls -= start
        le -= start
    out.write(data.tobytes())
    out.write(b'\0' * PAD * 2)
    shdr[i][1:5] = [pos, pos + len(data), pos + ls, pos + le]
    pos += len(data) + PAD

# Terminal record
shdr[-1][1:5] = [0, 0, 0, 0]
smpl_size = pos * 2
new_shdr = b''.join(struct.pack('<20sIIIIIbbHH', *r) for r in shdr)

with open(dst, 'wb') as o:
    o.write(b'RIFF\0\0\0\0sfbk')
    for kind, subs in top:
        lstart = o.tell()
        o.write(b'LIST\0\0\0\0' + kind)
        for cid, off, size in subs:
            if kind == b'sdta' and cid == b'smpl':
                o.write(struct.pack('<4sI', cid, smpl_size))
                out.seek(0)
                while True:
                    buf = out.read(1 << 24)
                    if not buf:
                        break
                    o.write(buf)
                continue
            if kind == b'pdta' and cid == b'shdr':
                data = new_shdr
            else:
                f.seek(off)
                data = f.read(size)
            o.write(struct.pack('<4sI', cid, len(data)) + data + (b'\0' if len(data) & 1 else b''))
        lend = o.tell()
        o.seek(lstart + 4)
        o.write(struct.pack('<I', lend - lstart - 8))
        o.seek(lend)
    end = o.tell()
    o.seek(4)
    o.write(struct.pack('<I', end - 8))

out.close()
import os
os.remove(dst + '.smpl.tmp')
print(f'{converted} samples resampled, new size {end / 2**20:.0f} MB')
