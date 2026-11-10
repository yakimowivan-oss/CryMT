r"""Inclusive share of engine functions (Ghidra function tables in logs\dump\funcs) on one CrySampler2 thread.
rtfuncs.py <prefix> <thread index> [top=30]"""
import bisect
import glob
import os
import sys
from collections import Counter

F = {}
for path in glob.glob(r'D:\Games\Crysis-RE\logs\dump\funcs\*.funcs.txt'):
    mod = os.path.basename(path)[:-len('.funcs.txt')]
    base = int(open(path).readline().split()[0], 16) & ~0xFFFF
    rows = []
    for l in open(path):
        p = l.split()
        if len(p) >= 3:
            rows.append((int(p[0], 16) - base, int(p[1], 16), p[2]))
    rows.sort()
    F[mod] = ([r[0] for r in rows], rows)
known = {0xA4480: 'FX_ProcessBatchesList', 0xA4DD0: 'general pass', 0xA4990: 'general group', 0x268D0: 'BatchStatic loop',
         0x25690: 'instanced path', 0xA11F0: 'merge test', 0xA4080: 'FX_ObjectChange', 0x3E670: 'param setter',
         0x1BA60: 'pass setup', 0x5B870: 'Shader_SetShaders', 0x29090: 'per-pass body'}


def fn(f):
    for mod in F:
        if f.startswith(mod + '!+0x'):
            off = int(f.split('+0x')[1], 16)
            a, rows = F[mod]
            i = bisect.bisect_right(a, off) - 1
            if i >= 0 and off <= rows[i][0] + max(rows[i][1], 1):
                return '%s %s %s' % (mod[:-4], rows[i][2], known.get(rows[i][0], '') if mod.startswith('CryRender') else '')
            return f
    return f if '!' in f and not f.startswith(('ntdll', 'KERNEL')) else None


prefix, thread = sys.argv[1], sys.argv[2]
top = int(sys.argv[3]) if len(sys.argv) > 3 else 30
incl = Counter()
n = 0
for l in open(prefix + '.samples.txt'):
    p = l.rstrip('\n').split('\t')
    if len(p) < 3 or p[0] != thread:
        continue
    n += 1
    st = [f for f in p[2].split(';') if f and f != '?'] if p[2] else []
    for x in set(x for x in (fn(f) for f in st) if x):
        incl[x] += 1
print('samples', n)
for k, v in incl.most_common(top):
    print('  %5.1f%%  %s' % (100 * v / n, k))
