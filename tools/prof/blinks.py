# Blinks in consecutive captures (gd_<variant>_w<point>_c<1..N>.ppm): pixels that differ from both neighbours by more
# than 40 in the same direction while the neighbours agree within 15 - an object missing (or extra) for one frame.
# Usage: blinks.py <variant> [<variant2> ...]
import glob
import sys
import numpy as np
from PIL import Image

D = r"D:\Games\Crysis-RE\logs\play\gd_%s_w%d_c%d.ppm"
for v in sys.argv[1:]:
    points = sorted({int(f.split("_w")[1].split("_")[0]) for f in glob.glob(D.replace("%s", v).replace("%d", "*"))})
    total = []
    for w in points:
        n = len(glob.glob(D.replace("%s", v).replace("%d", str(w), 1).replace("%d", "*")))
        f = [np.asarray(Image.open(D % (v, w, c))).astype(np.float32).mean(axis=2) for c in range(1, n + 1)]
        shares = []
        for i in range(1, n - 1):
            a, b, c = f[i - 1], f[i], f[i + 1]
            blink = (np.abs(a - c) < 15) & (np.abs(b - a) > 40) & (np.abs(b - c) > 40)
            shares.append(blink.mean() * 100)
        total += shares
        print("%s point %d: blinking pixels per frame %% max %.3f mean %.3f (%d frames)" % (v, w, max(shares), np.mean(shares), n))
    print("%s: mean %.3f%%, frames with over 0.1%%: %d of %d" % (v, np.mean(total), sum(s > 0.1 for s in total), len(total)))
