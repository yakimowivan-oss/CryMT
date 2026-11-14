r"""Terrain gone white (its textures missing) per frame of frame series: the share of whitish pixels (bright, little
colour) in the lower middle of the view (the ground, left of the weapon) and in the upper band (distant slopes).
  python tools\prof\white_ground.py <dir> <name>=<prefix> ...
"""
import glob
import os
import re
import sys

import numpy as np
from PIL import Image


def whitish(a):
    mx, mn = a.max(axis=2), a.min(axis=2)
    return (mx > 170) & ((mx - mn) < 0.2 * mx)


def main():
    d, items = sys.argv[1], sys.argv[2:]
    for it in items:
        name, prefix = it.split('=', 1)
        exact = re.compile(re.escape(os.path.basename(prefix)) + r'_c(\d+)\.ppm$')
        files = sorted((f for f in glob.glob(os.path.join(d, prefix + '_c*.ppm')) if exact.match(os.path.basename(f))),
                       key=lambda p: int(exact.match(os.path.basename(p)).group(1)))
        cells = []
        for f in files:
            a = np.asarray(Image.open(f).convert('RGB'), dtype=np.float32)
            h, w = a.shape[:2]
            ground = a[int(0.62 * h):int(0.9 * h), int(0.25 * w):int(0.5 * w)]
            far = a[int(0.2 * h):int(0.45 * h), int(0.35 * w):int(0.95 * w)]
            cells.append('%4.0f/%3.0f' % (whitish(ground).mean() * 100, whitish(far).mean() * 100))
        print('%-10s ground/far white %%: ' % name + ' '.join(cells))


main()
