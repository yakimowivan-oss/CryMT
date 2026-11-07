"""Temporal flicker and isolated bright dots in a series of captured frames of a static view (CRYMT_CAPTURE PPMs).
flicker: per-pixel luminance standard deviation over the frames (mean, and share of pixels above 0.02);
dots: pixels brighter than all 8 neighbours by a factor and a margin, per frame (mean), and their share in the
upper or lower half when a mask is wanted. Compare runs with GI off and on.
flicker.py <frame.ppm> ... [factor=1.25] [margin=0.06]"""
import sys

import numpy as np


def load(path):
    with open(path, "rb") as f:
        assert f.readline().strip() == b"P6"
        w, h = map(int, f.readline().split())
        f.readline()
        a = np.frombuffer(f.read(w * h * 3), dtype=np.uint8).reshape(h, w, 3).astype(np.float32) / 255.0
    return a @ np.array([0.2126, 0.7152, 0.0722], dtype=np.float32)


paths = [a for a in sys.argv[1:] if "=" not in a]
opts = dict(a.split("=") for a in sys.argv[1:] if "=" in a)
factor = float(opts.get("factor", 1.25))
margin = float(opts.get("margin", 0.06))
lum = np.stack([load(p) for p in paths])
std = lum.std(axis=0)


def dots(l):
    c = l[1:-1, 1:-1]
    n = np.max(np.stack([l[1 + dy:l.shape[0] - 1 + dy, 1 + dx:l.shape[1] - 1 + dx] for dy in (-1, 0, 1) for dx in (-1, 0, 1) if dy or dx]), axis=0)
    return int(((c > n * factor) & (c > n + margin)).sum())


counts = [dots(l) for l in lum]
print("frames %d: flicker mean %.4f, pixels > 0.02: %.3f%%; dots per frame %.0f (min %d, max %d)" %
      (len(paths), std.mean(), (std > 0.02).mean() * 100, np.mean(counts), min(counts), max(counts)))
