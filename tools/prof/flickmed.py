"""Short-lived changes in consecutive captures: per frame the pixels off the temporal median of the frames around it
(radius r, default 4) by more than a threshold, so states lasting up to r frames count (A-B-A, A-B-B-A, ...), while
slow camera motion stays near the median. Prints the count per frame and saves the worst frame beside its median.
flickmed.py <out.png> <frame.ppm> ... [threshold=25] [radius=4] [region=x0,y0,x1,y1]"""
import sys

import numpy as np
from PIL import Image


def load(path):
    with open(path, "rb") as f:
        assert f.readline().strip() == b"P6"
        w, h = map(int, f.readline().split())
        f.readline()
        return np.frombuffer(f.read(w * h * 3), dtype=np.uint8).reshape(h, w, 3)


out = sys.argv[1]
paths = [a for a in sys.argv[2:] if "=" not in a]
opts = dict(a.split("=") for a in sys.argv[2:] if "=" in a)
threshold = int(opts.get("threshold", 25))
radius = int(opts.get("radius", 4))
first = load(paths[0])
h, w = first.shape[:2]
x0, y0, x1, y1 = map(int, opts.get("region", "0,%d,%d,%d" % (h // 14, w, h)).split(","))
frames = np.stack([load(p)[y0:y1, x0:x1] for p in paths]).astype(np.int16)
n = len(frames)
counts = []
worst = (0, 0, None, None)
for i in range(n):
    lo, hi = max(0, i - radius), min(n, i + radius + 1)
    if hi - lo < 2 * radius + 1:
        counts.append(-1)
        continue
    median = np.median(frames[lo:hi], axis=0)
    off = np.abs(frames[i] - median).max(axis=2) > threshold
    counts.append(int(off.sum()))
    if off.sum() > worst[0]:
        worst = (int(off.sum()), i, median, off)
print("pixels off the temporal median per frame (-1 = edge):", " ".join(map(str, counts)))
if worst[2] is not None:
    total, i, median, off = worst
    a = frames[i].clip(0, 255).astype(np.uint8)
    b = median.clip(0, 255).astype(np.uint8)
    crop = np.concatenate([a, np.full((6, a.shape[1], 3), 255, np.uint8), b], axis=0)
    Image.fromarray(crop).save(out)
    ys, xs = np.nonzero(off)
    print("worst frame %d (%s): %d pixels, around x %d y %d" % (i, paths[i].split("\\")[-1].split("/")[-1], total, x0 + np.median(xs), y0 + np.median(ys)))
