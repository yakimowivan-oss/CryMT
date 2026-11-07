"""Frame-to-frame flicker in consecutive captures (CRYMT_CAPTURE ... COUNT=n): a pixel flickers at frame n when it jumps
away from both neighbours in the same direction while the neighbours agree (A-B-A: something there for one frame
only, or missing for one frame only). Slow camera motion moves edges monotonically and does not count.
flickaba.py <out.png> <frame.ppm> ... [threshold=40] [region=x0,y0,x1,y1]"""
import sys

import numpy as np
from PIL import Image


def load(path):
    with open(path, "rb") as f:
        assert f.readline().strip() == b"P6"
        w, h = map(int, f.readline().split())
        f.readline()
        return np.frombuffer(f.read(w * h * 3), dtype=np.uint8).reshape(h, w, 3).astype(np.int16)


out = sys.argv[1]
paths = [a for a in sys.argv[2:] if "=" not in a]
opts = dict(a.split("=") for a in sys.argv[2:] if "=" in a)
threshold = int(opts.get("threshold", 40))
frames = np.stack([load(p) for p in paths]).mean(axis=3)  # luminance-ish, one channel
n, h, w = frames.shape
mask = np.zeros((h, w), bool)
x0, y0, x1, y1 = map(int, opts.get("region", "0,%d,%d,%d" % (h // 14, w, h)).split(","))
mask[y0:y1, x0:x1] = True
heat = np.zeros((h, w), np.int32)
counts = []
for i in range(1, n - 1):
    a, b, c = frames[i - 1], frames[i], frames[i + 1]
    up = (b - a > threshold) & (b - c > threshold)
    down = (a - b > threshold) & (c - b > threshold)
    flick = (up | down) & (np.abs(a - c) < threshold / 2) & mask
    heat += flick
    counts.append(int(flick.sum()))
print("A-B-A flicker pixels per middle frame:", " ".join(map(str, counts)))
img = np.stack([frames[0] * 0.5] * 3, axis=2)
img[..., 0] = np.maximum(img[..., 0], np.clip(heat * 120, 0, 255))
Image.fromarray(img.clip(0, 255).astype(np.uint8)).save(out)
ys, xs = np.nonzero(heat)
if len(xs) > 30:
    cx, cy = int(np.median(xs)), int(np.median(ys))
    print("flicker centre x %d y %d, bounding x %d..%d y %d..%d" % (cx, cy, np.percentile(xs, 5), np.percentile(xs, 95), np.percentile(ys, 5), np.percentile(ys, 95)))
