r"""Which functions of a Crysis module changed in another build (Warhead, Wars): every .pdata function gets a fingerprint,
the sequence of its instruction mnemonics (addresses and displacements ignored); Crysis functions whose fingerprint the
other build lacks are listed with their Ghidra names (logs\dump\funcs), biggest first.
fndiff.py <dll name> <other game dir> [top=60]
Run with tools\venv\Scripts\python.exe (capstone)."""
import hashlib
import os
import struct
import sys
from capstone import CS_ARCH_X86, CS_MODE_64, Cs

dll, other = sys.argv[1], sys.argv[2]
top = int(sys.argv[3]) if len(sys.argv) > 3 else 60
md = Cs(CS_ARCH_X86, CS_MODE_64)
md.skipdata = True


def fingerprints(path):
    d = open(path, 'rb').read()
    pe = struct.unpack_from('<I', d, 0x3C)[0]
    nsec = struct.unpack_from('<H', d, pe + 6)[0]
    optsize = struct.unpack_from('<H', d, pe + 20)[0]
    opt = pe + 24
    base = struct.unpack_from('<Q', d, opt + 24)[0]
    pr, ps = struct.unpack_from('<II', d, opt + 112 + 3 * 8)
    secs = [struct.unpack_from('<IIII', d, opt + optsize + 40 * i + 8) for i in range(nsec)]

    def off(r):
        return next(r - va + rp for vs, va, rs, rp in secs if va <= r < va + max(vs, rs))

    out = {}
    seen = set()
    o = off(pr)
    for i in range(0, ps, 12):
        b, e, _ = struct.unpack_from('<III', d, o + i)
        if b in seen:
            continue
        seen.add(b)
        code = d[off(b):off(b) + (e - b)]
        h = hashlib.md5(' '.join(x[2] for x in md.disasm_lite(code, base + b)).encode()).hexdigest()
        out[base + b] = (h, e - b)
    return out


crysis = fingerprints(os.path.join(r'D:\Games\Crysis\Bin64', dll))
theirs = fingerprints(os.path.join(other, 'Bin64', dll))
their_hashes = {}
for a, (h, n) in theirs.items():
    their_hashes.setdefault(h, a)
names = {}
fpath = os.path.join(r'D:\Games\Crysis-RE\logs\dump\funcs', dll + '.funcs.txt')
if os.path.exists(fpath):
    import bisect
    rows = []
    for l in open(fpath):
        p = l.split()
        if len(p) >= 3:
            rows.append((int(p[0], 16), p[2]))
    rows.sort()
    starts = [r[0] for r in rows]

    def name_of(a):
        i = bisect.bisect_right(starts, a) - 1
        return rows[i][1] if i >= 0 else '?'
else:
    def name_of(a):
        return '?'
changed = [(n, a) for a, (h, n) in crysis.items() if h not in their_hashes]
same_bytes = sum(n for a, (h, n) in crysis.items() if h in their_hashes)
total = sum(n for a, (h, n) in crysis.items())
print('%s vs %s: %d of %d Crysis function chunks unchanged (%.1f%% of code), %d changed or gone' % (
    dll, other, len(crysis) - len(changed), len(crysis), 100.0 * same_bytes / total, len(changed)))
for n, a in sorted(changed, reverse=True)[:top]:
    print('  %x %6d bytes  %s' % (a, n, name_of(a)))
