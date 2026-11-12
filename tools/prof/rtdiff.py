r"""Difference of two sampled runs on one thread: inclusive and exclusive ms per frame of each named frame (engine
functions by the Ghidra tables, CryMT by its symbols), A minus B, largest first. Sample shares become ms per frame by
the run's mean frame time over the sampled window. rtdiff.py <prefixA> <prefixB> <thread> [top=30] [exclusive=0]
(prefixes as spot_run -Sample writes them: logs\spot\<tag>_<variant>)"""
import bisect
import glob
import os
import subprocess
import sys
from collections import Counter

a_pre, b_pre, thread = sys.argv[1], sys.argv[2], sys.argv[3]
opts = dict(x.split('=', 1) for x in sys.argv[4:] if '=' in x)
top = int(opts.get('top', 30))
excl = opts.get('exclusive', '0') == '1'

F = {}
for path in glob.glob(r'D:\Games\Crysis-RE\logs\dump\funcs\*.funcs.txt'):
    mod = os.path.basename(path)[:-len('.funcs.txt')]
    base = int(open(path).readline().split()[0], 16) & ~0xFFFF
    rows = sorted((int(p[0], 16) - base, int(p[1], 16), ' '.join(p[2:])) for p in (l.split() for l in open(path)) if len(p) >= 3)
    F[mod.lower().replace('.dll', '')] = ([r[0] for r in rows], rows)
nm = subprocess.run([r'D:\Games\Crysis-RE\tools\llvm\bin\llvm-nm.exe', '--demangle', '--defined-only', r'D:\Games\Crysis-RE\build\CryMT.dll'],
                    capture_output=True, text=True).stdout
syms = sorted((int(p[0], 16) - 0x180000000, p[2]) for p in (l.split(' ', 2) for l in nm.splitlines()) if len(p) == 3 and p[1] in 'tT')
saddr = [s[0] for s in syms]


def name(frame):
    if '!+0x' not in frame:
        return frame
    mod, off = frame.split('!+0x')
    rva = int(off, 16)
    m = mod.lower().replace('.dll', '').replace('.exe', '')
    if m == 'crymt':
        i = bisect.bisect_right(saddr, rva) - 1
        return 'CryMT!' + syms[i][1][:80] if i >= 0 else frame
    if m in F:
        starts, rows = F[m]
        i = bisect.bisect_right(starts, rva) - 1
        if i >= 0 and rows[i][0] <= rva < rows[i][0] + max(rows[i][1], 1):
            return '%s!%s' % (mod.replace('.dll', ''), rows[i][2][:80])
    return '%s!+0x%x' % (mod, rva & ~0xFFF)


def load(prefix):
    lines = open(prefix + '.samples.txt', encoding='utf-8', errors='replace').read().splitlines()
    inc, exc, n = Counter(), Counter(), 0
    for l in lines:
        p = l.split('\t')
        if len(p) < 3 or p[0] != thread:
            continue
        frames = [name(f) for f in p[2].split(';') if f and f != '?']  # root first, the leaf last
        n += 1
        if frames:
            exc[frames[-1]] += 1
        for f in set(frames):
            inc[f] += 1
    return inc, exc, n


def frame_ms(prefix):
    toks = open(prefix + '.frames.txt').read().split()
    freq, st = int(toks[1]), [int(x) for x in toks[2:]]
    st = st[-800:]
    return (st[-1] - st[0]) * 1000.0 / freq / (len(st) - 1)


ia, ea, na = load(a_pre)
ib, eb, nb = load(b_pre)
fa, fb = frame_ms(a_pre), frame_ms(b_pre)
A = ea if excl else ia
B = eb if excl else ib
print('A %s: %d samples, frame %.2f ms; B %s: %d samples, frame %.2f ms; %s, ms per frame' % (a_pre, na, fa, b_pre, nb, fb, 'exclusive' if excl else 'inclusive'))
rows = []
for k in set(A) | set(B):
    ma = A[k] / max(na, 1) * fa
    mb = B[k] / max(nb, 1) * fb
    rows.append((ma - mb, ma, mb, k))
rows.sort(reverse=True)
for d, ma, mb, k in rows[:top]:
    print('  %+6.2f  %6.2f %6.2f  %s' % (d, ma, mb, k))
