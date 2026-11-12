r"""Code addresses whose instructions carry a 32-bit displacement (a field offset), mapped to their functions from the
Ghidra tables in logs\dump\funcs: finddisp.py <dll> <hex displacement> [funcs file]"""
import bisect
import os
import struct
import sys

dll, disp = sys.argv[1], int(sys.argv[2], 16)
d = open(dll, 'rb').read()
pe = struct.unpack_from('<I', d, 0x3C)[0]
nsec = struct.unpack_from('<H', d, pe + 6)[0]
optsize = struct.unpack_from('<H', d, pe + 20)[0]
base = struct.unpack_from('<Q', d, pe + 24 + 24)[0]
funcs = sys.argv[3] if len(sys.argv) > 3 else os.path.join(r'D:\Games\Crysis-RE\logs\dump\funcs', os.path.basename(dll) + '.funcs.txt')
rows = sorted((int(p[0], 16), int(p[1], 16), p[2]) for p in (l.split() for l in open(funcs)) if len(p) >= 3)
starts = [r[0] for r in rows]
needle = struct.pack('<I', disp)
hits = {}
for i in range(nsec):
    o = pe + 24 + optsize + 40 * i
    name = d[o:o + 8].rstrip(b'\0')
    vsize, va, rawsize, raw = struct.unpack_from('<IIII', d, o + 8)
    chars = struct.unpack_from('<I', d, o + 36)[0]
    if not chars & 0x20000000:  # executable sections only
        continue
    pos = raw
    while True:
        pos = d.find(needle, pos, raw + rawsize)
        if pos < 0:
            break
        addr = base + va + pos - raw
        k = bisect.bisect_right(starts, addr) - 1
        f = rows[k] if k >= 0 and addr < rows[k][0] + rows[k][1] else None
        hits.setdefault(f[2] + ' @ %x' % f[0] if f else '?', []).append(addr)
        pos += 1
for f, a in sorted(hits.items(), key=lambda kv: kv[1][0]):
    print('%-50s %s' % (f, ' '.join('%x' % x for x in a)))
