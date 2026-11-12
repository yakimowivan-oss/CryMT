r"""Mean of every crymt_detail.txt column over a spot run's window (from the census reset to quit), next to the frame time,
and the main thread's top sync reasons in it. spot_phases.py <dir> <tag> <variant>"""
import os
import re
import sys
from collections import Counter

d, tag, v = sys.argv[1:4]
p = lambda f: os.path.join(d, '%s_%s_%s' % (tag, v, f))
text = open(p('crymt_focus.txt'), encoding='utf-8', errors='replace').read()
text = text[text.rfind("console command 'r_DynTexMaxSize"):]
a = int(re.search(r"frame (\d+): census counts reset", text).group(1))
b = int(re.search(r"frame (\d+): console command 'quit'", text).group(1)) - 5
toks = open(p('crymt_frames.txt')).read().split()
freq, stamps = int(toks[1]), [int(x) for x in toks[2:]]
lines = open(p('crymt_detail.txt')).read().splitlines()
names = lines[0].split()[1:]
rows = {int(l.split()[0]): l.split()[1:] for l in lines[1:]}
sel = [i for i in range(a, min(b, len(stamps) - 1)) if i in rows]
frame = sum((stamps[i + 1] - stamps[i]) * 1000.0 / freq for i in sel) / len(sel)
print('%s %s: %d frames, frame %.2f ms' % (tag, v, len(sel), frame))
for k, n in enumerate(names):
    if n in ('seq', 'topReason'):
        continue
    print('  %-12s %8.3f' % (n, sum(float(rows[i][k]) for i in sel) / len(sel)))
k = names.index('topReason')
w = names.index('topWait')
reasons = Counter()
waits = Counter()
for i in sel:
    reasons[rows[i][k]] += 1
    waits[rows[i][k]] += float(rows[i][w])
print('  top sync reason per frame (reason: frames, mean top wait ms):')
for r, c in reasons.most_common(8):
    print('    %6s: %5d  %6.2f' % (r, c, waits[r] / c))
