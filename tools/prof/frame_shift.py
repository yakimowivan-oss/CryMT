r"""Global image shift between consecutive frames (phase correlation with sub-pixel peak fit) of frame series
(frames_spot.ps1 / toggle_frames.ps1), over the view's upper 60% without the panel box: tells camera movement (sway,
drift) from shading flicker.
  python tools\prof\frame_shift.py <dir> <name>=<prefix> ...
"""
import glob
import os
import re
import sys

import numpy as np
from PIL import Image


def lum(a):
    return a[..., 0] * 0.2126 + a[..., 1] * 0.7152 + a[..., 2] * 0.0722


def shift(a, b):
    fa, fb = np.fft.fft2(a), np.fft.fft2(b)
    r = fa * np.conj(fb)
    r /= np.abs(r) + 1e-9
    c = np.real(np.fft.ifft2(r))
    py, px = np.unravel_index(np.argmax(c), c.shape)
    h, w = c.shape

    def sub(m, p, n):
        l, cc, rr = m[(p - 1) % n], m[p], m[(p + 1) % n]
        den = l - 2 * cc + rr
        return p + (0.5 * (l - rr) / den if den != 0 else 0)
    y = sub(c[:, px], py, h)
    x = sub(c[py, :], px, w)
    if y > h / 2:
        y -= h
    if x > w / 2:
        x -= w
    return x, y


def main():
    d, items = sys.argv[1], sys.argv[2:]
    for it in items:
        name, prefix = it.split('=', 1)
        exact = re.compile(re.escape(os.path.basename(prefix)) + r'_c(\d+)\.ppm$')
        files = sorted((f for f in glob.glob(os.path.join(d, prefix + '_c*.ppm')) if exact.match(os.path.basename(f))),
                       key=lambda p: int(exact.match(os.path.basename(p)).group(1)))
        frames = []
        for f in files:
            a = lum(np.asarray(Image.open(f).convert('RGB'), dtype=np.float32))
            h, w = a.shape
            frames.append(a[int(0.08 * h):int(0.33 * h), int(0.3 * w):int(0.95 * w)])
        out = []
        for k in range(len(frames) - 1):
            x, y = shift(frames[k + 1], frames[k])
            out.append('%+.2f,%+.2f' % (x, y))
        print('%-8s ' % name + ' '.join(out))


main()
