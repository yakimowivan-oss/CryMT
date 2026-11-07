"""Detects broken (flat fog-colored) vegetation: the most common color covers > 2% of the frame.
grayscore.py <png>...  prints one line per file and a summary."""
import sys
from collections import Counter
from PIL import Image
broken = 0
for path in sys.argv[1:]:
    im = Image.open(path).convert("RGB")
    w, h = im.size
    px = im.load()
    c = Counter()
    n = 0
    for y in range(90, h - 40, 2):
        for x in range(0, w, 2):
            c[px[x, y]] += 1
            n += 1
    (col, v), = c.most_common(1)
    share = 100.0 * v / n
    bad = share > 2.0 and 60 <= col[0] <= col[1] <= col[2] <= 240
    broken += bad
    print(f"{'BROKEN' if bad else 'ok    '} top={col} {share:5.1f}%  {path.replace(chr(92), '/').split('/')[-1]}")
print(f"broken {broken} of {len(sys.argv) - 1}")
