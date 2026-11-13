r"""Temporal flicker of a still view in a crop: per-pixel luminance std over consecutive captured frames (crymt_capture N)
in a box given as fractions of the frame (default the upper 28% left of the weapon: walls and ceiling, no HUD), plus the
mean absolute change between neighbouring frames and the share of pixels whose std exceeds 1/255 and 3/255.
  python tools\prof\flicker_crop.py <x0> <y0> <x1> <y1> <frame.ppm> ...
"""
import sys

import numpy as np


def load(path):
    with open(path, 'rb') as f:
        assert f.readline().strip() == b'P6'
        w, h = map(int, f.readline().split())
        f.readline()
        a = np.frombuffer(f.read(w * h * 3), dtype=np.uint8).reshape(h, w, 3).astype(np.float32)
    return a @ np.array([0.2126, 0.7152, 0.0722], dtype=np.float32)


x0, y0, x1, y1 = (float(v) for v in sys.argv[1:5])
frames = [load(p) for p in sys.argv[5:]]
h, w = frames[0].shape
c = np.stack([f[int(y0 * h):int(y1 * h), int(x0 * w):int(x1 * w)] for f in frames])
std = c.std(axis=0)
step = np.abs(np.diff(c, axis=0)).mean()
print('frames %d, crop mean %.1f: std mean %.3f, > 1/255 %.1f%%, > 3/255 %.2f%%, frame-to-frame change %.3f' % (
    len(frames), c.mean(), std.mean(), (std > 1).mean() * 100, (std > 3).mean() * 100, step))
