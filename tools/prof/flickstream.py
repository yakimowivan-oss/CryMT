"""One-frame flicker over a whole run (CRYMT_CAPTURE_STREAM file): per frame the pixels that jump away from both
neighbours in the same direction while the neighbours agree (A-B-A). Prints the frames above a floor, and saves the
worst ones as strips (previous, this, next) next to the stream file.
flickstream.py <x_stream.raw> [threshold=20] [floor=150] [top=6]"""
import sys

import numpy as np
from PIL import Image

path = sys.argv[1]
opts = dict(a.split("=") for a in sys.argv[2:] if "=" in a)
threshold = int(opts.get("threshold", 20))
floor = int(opts.get("floor", 150))
top = int(opts.get("top", 6))
data = np.fromfile(path, dtype=np.uint8)
w, h = data[:8].view(np.uint32)
size = 4 + int(w) * int(h) * 3
n = (len(data) - 8) // size
records = data[8:8 + n * size].reshape(n, size)
frames_id = records[:, :4].copy().view(np.uint32).ravel()
images = records[:, 4:].reshape(n, h, w, 3)
skip = h // 14  # the engine's info text at the top
print("%s: %d frames %dx%d, world frames %d..%d" % (path.split("\\")[-1], n, w, h, frames_id[0], frames_id[-1]))
counts = np.zeros(n, np.int64)
for i in range(1, n - 1):
    a, b, c = (images[k, skip:].mean(axis=2) for k in (i - 1, i, i + 1))
    up = (b - a > threshold) & (b - c > threshold)
    down = (a - b > threshold) & (c - b > threshold)
    counts[i] = int(((up | down) & (np.abs(a - c) < threshold / 2)).sum())
events = [(int(counts[i]), i) for i in range(n) if counts[i] >= floor]
print("frames with >= %d A-B-A pixels (of %d): %d" % (floor, w * (h - skip), len(events)))
print("  " + " ".join("%d:%d" % (frames_id[i], c) for c, i in events))
for c, i in sorted(events, reverse=True)[:top]:
    strip = np.concatenate([images[i - 1], np.full((h, 3, 3), 255, np.uint8), images[i], np.full((h, 3, 3), 255, np.uint8), images[i + 1]], axis=1)
    Image.fromarray(strip).save(path.replace("_stream.raw", "_ev%d.png" % frames_id[i]))
