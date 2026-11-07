# Objects that vanish (or change) for a frame or two in a run of consecutive captures: per 32x32 block, the frame
# differs from both the frame before and the one two frames on by more than 20 on average while those two agree within
# 8. Prints the frames and blocks hit, saves the worst frame with its neighbours (blocks outlined in magenta).
# Usage: blinkblocks.py <variant> <point> <out.png>
import glob
import sys
import numpy as np
from PIL import Image

v, w, out = sys.argv[1], int(sys.argv[2]), sys.argv[3]
D = r"D:\Games\Crysis-RE\logs\play\gd_%s_w%d_c%d.ppm"
n = len(glob.glob(D.replace("%s", v).replace("%d", str(w), 1).replace("%d", "*")))
frames = [np.asarray(Image.open(D % (v, w, c))).astype(np.float32) for c in range(1, n + 1)]
B = 32
def blocks(x):
    g = x.mean(axis=2)
    h, wd = g.shape[0] // B * B, g.shape[1] // B * B
    return g[:h, :wd].reshape(h // B, B, wd // B, B).mean(axis=(1, 3))
bl = [blocks(f) for f in frames]
worst = (0, None, None)
for i in range(1, n - 2):
    for span in (1, 2):
        if i + span >= n:
            continue
        a, c = bl[i - 1], bl[i + span]
        mid = [bl[i + k] for k in range(span)]
        hit = (np.abs(a - c) < 8) & np.all([np.abs(m - a) > 20 for m in mid], axis=0)
        hit[:4] = False  # the info text rows
        if hit.sum():
            ys, xs = np.nonzero(hit)
            print("frame index %d (+%d): %d blocks, around x %d y %d, %s" % (i + 1, span, hit.sum(), int(xs.mean() * B), int(ys.mean() * B),
                  "darker" if (mid[0] - a)[hit].mean() < 0 else "brighter"))
            if hit.sum() > worst[0]:
                worst = (hit.sum(), i, hit)
if worst[1] is not None:
    i, hit = worst[1], worst[2]
    vis = frames[i].copy()
    for y, x in zip(*np.nonzero(hit)):
        vis[y * B:(y + 1) * B, x * B] = vis[y * B:(y + 1) * B, (x + 1) * B - 1] = [255, 0, 255]
        vis[y * B, x * B:(x + 1) * B] = vis[(y + 1) * B - 1, x * B:(x + 1) * B] = [255, 0, 255]
    Image.fromarray(np.concatenate([frames[i - 1], vis, frames[i + 1]], axis=1).clip(0, 255).astype(np.uint8)).resize((2400, 450)).save(out)
    print("saved worst: frame index", i + 1)
else:
    print("no blinking blocks")
