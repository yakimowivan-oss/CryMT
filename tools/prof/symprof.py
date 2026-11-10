"""CrySampler2 samples of one thread with CryMT.dll's functions named (its symbol table, llvm-nm): inclusive and exclusive
shares, as ms per frame over the sampled frames.
symprof.py <prefix> <thread index> [dll=D:\\Games\\Crysis-RE\\build\\CryMT.dll] [top=30]"""
import bisect
import subprocess
import sys
from collections import Counter

prefix, thread = sys.argv[1], int(sys.argv[2])
opts = dict(a.split('=', 1) for a in sys.argv[3:] if '=' in a)
dll = opts.get('dll', r'D:\Games\Crysis-RE\build\CryMT.dll')
top = int(opts.get('top', 30))
nm = subprocess.run([r'D:\Games\Crysis-RE\tools\llvm\bin\llvm-nm.exe', '--demangle', '--defined-only', dll], capture_output=True, text=True).stdout
syms = []
for line in nm.splitlines():
    p = line.split(' ', 2)
    if len(p) == 3 and p[1] in 'tT':
        syms.append((int(p[0], 16) - 0x180000000, p[2]))
syms.sort()
addrs = [a for a, _ in syms]


def name(frame):
    if frame.startswith('CryMT.dll!+0x'):
        off = int(frame[13:], 16)
        i = bisect.bisect_right(addrs, off) - 1
        if i >= 0:
            n = syms[i][1]
            return 'CryMT!' + (n if len(n) < 110 else n[:107] + '...')
    return frame


frames = open(prefix + '.frames.txt').read().split()
freq = int(frames[1])
stamps = [int(x) for x in frames[2:]]
samples = [l.rstrip('\n').split('\t') for l in open(prefix + '.samples.txt')]
mine = [p for p in samples if len(p) >= 3 and int(p[0]) == thread]
if not mine:
    sys.exit('no samples for thread %d' % thread)
t0, t1 = int(mine[0][1]), int(mine[-1][1])
lo, hi = bisect.bisect_left(stamps, t0), bisect.bisect_right(stamps, t1)
nframes = max(1, hi - lo - 1)
ms_per_sample = (t1 - t0) / freq * 1000.0 / len(mine) / nframes  # wall ms per frame a sample stands for
incl, excl = Counter(), Counter()
for p in mine:
    stack = [name(f) for f in p[2].split(';') if f and f != '?'] if p[2] else []
    if not stack:
        excl['(unknown)'] += 1
        continue
    excl[stack[-1]] += 1
    for f in set(stack):
        incl[f] += 1
print('thread %d: %d samples over %d frames, %.3f ms a sample a frame' % (thread, len(mine), nframes, ms_per_sample))
print('exclusive:')
for f, c in excl.most_common(top):
    print('  %6.2f ms  %s' % (c * ms_per_sample, f))
print('inclusive:')
for f, c in incl.most_common(top):
    print('  %6.2f ms  %s' % (c * ms_per_sample, f))
