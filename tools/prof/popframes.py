"""Pop-in / flicker in a still view (CRYMT_CAPTURE PPMs, any number of windows): per frame the pixels whose colour
differs from the median of all frames by more than a threshold (an object missing or extra in that frame, not noise),
per neighbouring pair the jumps, and a heat map of where.
popframes.py <out.png> <frame.ppm> ... [threshold=48]"""
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
threshold = int(opts.get("threshold", 48))
frames = np.stack([load(p) for p in paths])
n, h, w = frames.shape[:3]
median = np.median(frames, axis=0)
mask = np.ones((h, w), bool)
mask[: h // 14, :] = False  # the engine's info text at the top
off = (np.abs(frames - median).max(axis=3) > threshold) & mask
pairs = [(np.abs(frames[i] - frames[i + 1]).max(axis=2) > threshold) & mask for i in range(n - 1)]
print("pixels off the median per frame:", " ".join(str(int(o.sum())) for o in off))
print("jumping pixels per pair:", " ".join(str(int(p.sum())) for p in pairs))
heat = off.sum(axis=0)
img = frames[0].astype(np.float32) * 0.5
img[..., 0] = np.maximum(img[..., 0], np.clip(heat * 60, 0, 255))
Image.fromarray(img.clip(0, 255).astype(np.uint8)).save(out)
# The frame most off the median: where, as a crop beside the median, enlarged.
worst = int(np.argmax(off.sum(axis=(1, 2))))
ys, xs = np.nonzero(off[worst])
if len(xs) > 50:
    cx, cy = int(np.median(xs)), int(np.median(ys))
    x0, y0 = max(0, cx - 160), max(0, cy - 90)
    a = frames[worst][y0:y0 + 180, x0:x0 + 320].clip(0, 255).astype(np.uint8)
    b = median[y0:y0 + 180, x0:x0 + 320].clip(0, 255).astype(np.uint8)
    crop = np.concatenate([a, np.full((a.shape[0], 6, 3), 255, np.uint8), b], axis=1)
    Image.fromarray(crop).resize((crop.shape[1] * 2, crop.shape[0] * 2), Image.NEAREST).save(out.replace(".png", "_worst.png"))
    print("worst frame %d (%s): %d pixels off, around x %d y %d" % (worst, paths[worst].split("\\")[-1], len(xs), cx, cy))
