r"""Temporal flicker on flat areas only (walls, ceilings: where the GI shows), leaving out edges and textured detail that
move with sub-pixel camera shake and the anti-aliasing: pixels whose mean frame has a small local gradient and a small
local spread (3x3), inside a box given as fractions of the frame.
  python tools\prof\flicker_flat.py <x0> <y0> <x1> <y1> <frame.ppm> ...
"""
import sys

import numpy as np
from PIL import Image


def load(p):
    return np.asarray(Image.open(p).convert('RGB'), dtype=np.float32) @ np.array([0.2126, 0.7152, 0.0722], dtype=np.float32)


x0, y0, x1, y1 = (float(v) for v in sys.argv[1:5])
f = np.stack([load(p) for p in sys.argv[5:]])
h, w = f.shape[1:]
f = f[:, int(y0 * h):int(y1 * h), int(x0 * w):int(x1 * w)]
m = f.mean(axis=0)
gx = np.abs(np.diff(m, axis=1))[:-1, :]
gy = np.abs(np.diff(m, axis=0))[:, :-1]
grad = np.maximum(gx, gy)
pad = np.pad(m, 1, mode='edge')
win = np.stack([pad[1 + dy:pad.shape[0] - 1 + dy, 1 + dx:pad.shape[1] - 1 + dx] for dy in (-1, 0, 1) for dx in (-1, 0, 1)])
spread = (win.max(axis=0) - win.min(axis=0))[:-1, :-1]
flat = (grad < 1.0) & (spread < 2.0)
std = f.std(axis=0)[:-1, :-1]
s = std[flat]
print('frames %d, flat pixels %.1f%% (mean %.1f): std mean %.3f, > 1/255 %.2f%%, > 2/255 %.2f%%, worst 1%% %.2f' % (
    f.shape[0], flat.mean() * 100, m[:-1, :-1][flat].mean(), s.mean(), (s > 1).mean() * 100, (s > 2).mean() * 100, np.percentile(s, 99)))
