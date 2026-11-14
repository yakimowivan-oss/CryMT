r"""Mean brightness per frame of a frame series in a few boxes (fractions of the frame) and where frames differ most:
which areas switch frame to frame (a light going on and off) and in what rhythm.
  python tools\prof\frame_series.py <out_diff.png> <frame.ppm> ...
Prints per frame the mean of each box, then the boxes' max-min; saves |frame k - frame k+1| (max over k) as an image.
"""
import sys

import numpy as np
from PIL import Image

BOXES = {
    'ceiling': (0.30, 0.02, 0.95, 0.22),
    'left wall': (0.08, 0.25, 0.45, 0.55),
    'right wall': (0.48, 0.25, 0.72, 0.55),
    'floor': (0.15, 0.62, 0.65, 0.95),
    'whole': (0.0, 0.0, 1.0, 1.0),
}


def load(p):
    return np.asarray(Image.open(p).convert('RGB'), dtype=np.float32).mean(axis=2)


def main():
    out, files = sys.argv[1], sys.argv[2:]
    frames = [load(f) for f in files]
    h, w = frames[0].shape
    rows = []
    for k, f in enumerate(frames):
        vals = []
        for name, (x0, y0, x1, y1) in BOXES.items():
            vals.append(f[int(y0 * h):int(y1 * h), int(x0 * w):int(x1 * w)].mean())
        rows.append(vals)
        print('frame %2d  ' % (k + 1) + '  '.join('%s %6.2f' % (n, v) for n, v in zip(BOXES, vals)))
    a = np.array(rows)
    print('max-min   ' + '  '.join('%s %6.2f' % (n, v) for n, v in zip(BOXES, a.max(0) - a.min(0))))
    diff = np.zeros_like(frames[0])
    for k in range(len(frames) - 1):
        diff = np.maximum(diff, np.abs(frames[k + 1] - frames[k]))
    print('pixels changing > 16 between some two frames: %.2f%%, > 48: %.2f%%' % ((diff > 16).mean() * 100, (diff > 48).mean() * 100))
    Image.fromarray(np.clip(diff * 4, 0, 255).astype(np.uint8)).resize((w // 2, h // 2)).save(out)


main()
