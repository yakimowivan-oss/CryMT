r"""The most common call chains of CrySampler2 samples that pass through one function, named (CryMT by its symbols,
engine modules by the Ghidra tables in logs\dump\funcs), as ms per frame.
stackof.py <prefix> <thread> <substring of a named frame> [top=8] [depth=14] [dll=D:\Games\Crysis-RE\build\CryMT.dll]"""
import bisect
import glob
import os
import subprocess
import sys
from collections import Counter

prefix, thread, needle = sys.argv[1], sys.argv[2], sys.argv[3]
opts = dict(a.split('=', 1) for a in sys.argv[4:] if '=' in a)
top, depth = int(opts.get('top', 8)), int(opts.get('depth', 14))
dll = opts.get('dll', r'D:\Games\Crysis-RE\build\CryMT.dll')

nm = subprocess.run([r'D:\Games\Crysis-RE\tools\llvm\bin\llvm-nm.exe', '--demangle', '--defined-only', dll], capture_output=True, text=True).stdout
syms = sorted((int(p[0], 16) - 0x180000000, p[2]) for p in (l.split(' ', 2) for l in nm.splitlines()) if len(p) == 3 and p[1] in 'tT')
addrs = [a for a, _ in syms]
F = {}
for path in glob.glob(r'D:\Games\Crysis-RE\logs\dump\funcs\*.funcs.txt'):
    mod = os.path.basename(path)[:-len('.funcs.txt')]
    base = int(open(path).readline().split()[0], 16) & ~0xFFFF
    rows = sorted((int(p[0], 16) - base, int(p[1], 16), p[2]) for p in (l.split() for l in open(path)) if len(p) >= 3)
    F[mod] = ([r[0] for r in rows], rows)


def name(f):
    if f.startswith('CryMT.dll!+0x'):
        off = int(f[13:], 16)
        i = bisect.bisect_right(addrs, off) - 1
        if i >= 0:
            n = syms[i][1].replace('(anonymous namespace)::', '')
            return 'CryMT!' + (n.split('(')[0] if '(' in n else n)
    for mod in F:
        if f.startswith(mod + '!+0x'):
            off = int(f.split('+0x')[1], 16)
            a, rows = F[mod]
            i = bisect.bisect_right(a, off) - 1
            if i >= 0 and off <= rows[i][0] + max(rows[i][1], 1):
                return '%s!%s(+%x)' % (mod[:-4], rows[i][2], off)
    return f


frames = open(prefix + '.frames.txt').read().split()
freq, stamps = int(frames[1]), [int(x) for x in frames[2:]]
mine = [p for p in (l.rstrip('\n').split('\t') for l in open(prefix + '.samples.txt')) if len(p) >= 3 and p[0] == thread]
t0, t1 = int(mine[0][1]), int(mine[-1][1])
nframes = max(1, bisect.bisect_right(stamps, t1) - bisect.bisect_left(stamps, t0) - 1)
per = (t1 - t0) / freq * 1000.0 / len(mine) / nframes
chains = Counter()
total = 0
for p in mine:
    st = [name(f) for f in p[2].split(';') if f and f != '?']
    hit = [i for i, f in enumerate(st) if needle in f]
    if not hit:
        continue
    total += 1
    i = hit[-1]
    chains[' <- '.join(reversed(st[max(0, i - depth):i + 2]))] += 1
print('%s on thread %s: %.2f ms a frame (%d frames)' % (needle, thread, total * per, nframes))
for c, n in chains.most_common(top):
    print('%6.2f ms  %s' % (n * per, c))
