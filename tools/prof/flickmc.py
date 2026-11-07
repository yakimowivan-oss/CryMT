"""One-frame flicker with the camera turning: the neighbours of each frame are shifted onto it first (global shift by
phase correlation, sub-pixel, measured on a reference area), then A-B-A as in flickaba.py within a region. For far
content (a mountain) a camera rotation is close to a plain shift.
flickmc.py <out_prefix> <frame.ppm> ... [threshold=20] [region=x0,y0,x1,y1] [ref=x0,y0,x1,y1]"""
import sys

import numpy as np
from PIL import Image


def load(path):
    with open(path, "rb") as f:
        assert f.readline().strip() == b"P6"
        w, h = map(int, f.readline().split())
        f.readline()
        return np.frombuffer(f.read(w * h * 3), dtype=np.uint8).reshape(h, w, 3).astype(np.float32).mean(axis=2)


def shifted(img, dy, dx):
    """img moved by (dy, dx), bilinear, edges repeated."""
    h, w = img.shape
    ys = np.clip(np.arange(h) - dy, 0, h - 1)
    xs = np.clip(np.arange(w) - dx, 0, w - 1)
    y0 = np.floor(ys).astype(int)
    x0 = np.floor(xs).astype(int)
    y1 = np.minimum(y0 + 1, h - 1)
    x1 = np.minimum(x0 + 1, w - 1)
    fy = (ys - y0)[:, None]
    fx = (xs - x0)[None, :]
    top = img[y0][:, x0] * (1 - fx) + img[y0][:, x1] * fx
    bottom = img[y1][:, x0] * (1 - fx) + img[y1][:, x1] * fx
    return top * (1 - fy) + bottom * fy


def shift_of(a, b):
    """The shift that moves b onto a."""
    A = np.fft.fft2(a - a.mean())
    B = np.fft.fft2(b - b.mean())
    R = A * np.conj(B)
    R /= np.abs(R) + 1e-6
    r = np.fft.ifft2(R).real
    y, x = np.unravel_index(np.argmax(r), r.shape)
    h, w = r.shape

    def sub(m, p, n):
        d = m - 2 * p + n
        return 0.0 if d == 0 else 0.5 * (m - n) / d

    dx = sub(r[y, (x - 1) % w], r[y, x], r[y, (x + 1) % w])
    dy = sub(r[(y - 1) % h, x], r[y, x], r[(y + 1) % h, x])
    return (y if y < h // 2 else y - h) + dy, (x if x < w // 2 else x - w) + dx


prefix = sys.argv[1]
paths = [a for a in sys.argv[2:] if "=" not in a]
opts = dict(a.split("=") for a in sys.argv[2:] if "=" in a)
threshold = float(opts.get("threshold", 20))
x0, y0, x1, y1 = map(int, opts.get("region", "0,80,960,380").split(","))
rx0, ry0, rx1, ry1 = map(int, opts.get("ref", "%d,%d,%d,%d" % (x0, y0, x1, y1)).split(","))
frames = [load(p) for p in paths]
counts = []
best = (0, None)
for i in range(1, len(frames) - 1):
    b = frames[i]
    aligned = []
    for k in (i - 1, i + 1):
        dy, dx = shift_of(b[ry0:ry1, rx0:rx1], frames[k][ry0:ry1, rx0:rx1])
        aligned.append(shifted(frames[k], dy, dx))
    a, c = aligned
    m = 12  # the border the shift fills in
    reg = (slice(y0 + m, y1 - m), slice(x0 + m, x1 - m))
    up = (b - a > threshold) & (b - c > threshold)
    down = (a - b > threshold) & (c - b > threshold)
    flick = ((up | down) & (np.abs(a - c) < threshold / 2))[reg]
    counts.append(int(flick.sum()))
    if flick.sum() > best[0]:
        best = (int(flick.sum()), i)
print("A-B-A after alignment per middle frame:", " ".join(map(str, counts)))
if best[1] is not None:
    i = best[1]
    strip = np.concatenate([np.asarray(Image.open(paths[k]))[y0:y1, x0:x1] for k in (i - 1, i, i + 1)], axis=0)
    Image.fromarray(strip).save(prefix + "_best.png")
    print("worst: %s, %d pixels" % (paths[i].replace("\\", "/").split("/")[-1], best[0]))
