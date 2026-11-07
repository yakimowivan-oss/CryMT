"""Differences between consecutive captured frames (CRYMT_CAPTURE PPMs) for alternating-frame tests (CRYMT_ZOWN=toggle):
neighbours (one frame ours, one the engine's) against pairs two apart (same path). A path error shows as neighbour pairs
differing clearly more than same-path pairs.
ppmdiff.py <first.ppm> <second.ppm> ... [threshold=24]"""
import sys

import numpy as np


def load(path):
    with open(path, "rb") as f:
        assert f.readline().strip() == b"P6"
        w, h = map(int, f.readline().split())
        f.readline()
        return np.frombuffer(f.read(w * h * 3), dtype=np.uint8).reshape(h, w, 3).astype(np.int16)


paths = [a for a in sys.argv[1:] if "=" not in a]
opts = dict(a.split("=") for a in sys.argv[1:] if "=" in a)
threshold = int(opts.get("threshold", 24))
frames = [load(p) for p in paths]


def stats(a, b):
    d = np.abs(a - b).max(axis=2)
    return (d > threshold).mean() * 100.0, d.mean()


for step, label in ((1, "neighbours (ours vs engine)"), (2, "two apart (same path)")):
    rows = [stats(frames[i], frames[i + step]) for i in range(len(frames) - step)]
    share = [r[0] for r in rows]
    mean = [r[1] for r in rows]
    print(f"{label}: pixels > {threshold}: " + " ".join(f"{s:.3f}%" for s in share) + f" | mean abs: " + " ".join(f"{m:.3f}" for m in mean))
