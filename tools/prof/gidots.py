"""GI-made dots: in frames captured at the same timedemo frames with GI off and on (CRYMT_CAPTURE_FRAME), the pixels GI
brightened clearly more than all their 8 neighbours (difference image on - off), per frame.
gidots.py <tag_off> <tag_on> [windows=3] [count=8] [margin=0.05]  (files logs\\play\\<tag>_w<w>_c<n>.ppm)"""
import os
import sys

import numpy as np

root = os.path.join(os.path.dirname(__file__), "..", "..", "logs", "play")


def load(path):
    with open(path, "rb") as f:
        assert f.readline().strip() == b"P6"
        w, h = map(int, f.readline().split())
        f.readline()
        a = np.frombuffer(f.read(w * h * 3), dtype=np.uint8).reshape(h, w, 3).astype(np.float32) / 255.0
    return a @ np.array([0.2126, 0.7152, 0.0722], dtype=np.float32)


off, on = sys.argv[1], sys.argv[2]
opts = dict(a.split("=") for a in sys.argv[3:] if "=" in a)
windows, count, margin = int(opts.get("windows", 3)), int(opts.get("count", 8)), float(opts.get("margin", 0.05))
totals = []
for w in range(1, windows + 1):
    per = []
    for c in range(1, count + 1):
        a = load(os.path.join(root, "%s_w%d_c%d.ppm" % (off, w, c)))
        b = load(os.path.join(root, "%s_w%d_c%d.ppm" % (on, w, c)))
        d = b - a
        cen = d[1:-1, 1:-1]
        nb = np.max(np.stack([d[1 + dy:d.shape[0] - 1 + dy, 1 + dx:d.shape[1] - 1 + dx] for dy in (-1, 0, 1) for dx in (-1, 0, 1) if dy or dx]), axis=0)
        per.append(int(((cen > nb + margin) & (cen > margin)).sum()))
    totals += per
    print("  window %d: GI dots per frame %.1f (max %d), mean |on - off| %.4f" % (w, np.mean(per), max(per), float(np.abs(d).mean())))
print("%s vs %s: GI dots per frame %.1f" % (on, off, np.mean(totals)))
