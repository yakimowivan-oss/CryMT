r"""Frames of one view per variant at two times of day: mean luminance of the frame, its top half and its upper-left
quarter (the walls under the ceiling), and how much each changes between the two times (an indoor view should change
little: only the light that comes in through openings moves with the sun).
  python tools\prof\tod_compare.py <dir> <prefixA> <prefixB> variant [variant ...]   (files <prefix>_<variant>.ppm)
"""
import os
import sys

import numpy as np
from PIL import Image


def stats(path):
    a = np.asarray(Image.open(path).convert('RGB'), dtype=np.float32)
    lum = 0.2126 * a[..., 0] + 0.7152 * a[..., 1] + 0.0722 * a[..., 2]
    h, w = lum.shape
    return lum.mean(), lum[: h // 2].mean(), lum[: h // 2, : w // 2].mean()


def main():
    folder, pa, pb = sys.argv[1], sys.argv[2], sys.argv[3]
    print('%-18s %-22s %-22s %s' % ('variant', pa + ' frame/top/UL', pb + ' frame/top/UL', 'change frame/top/UL'))
    for v in sys.argv[4:]:
        a = stats(os.path.join(folder, '%s_%s.ppm' % (pa, v)))
        b = stats(os.path.join(folder, '%s_%s.ppm' % (pb, v)))
        print('%-18s %5.1f %5.1f %5.1f      %5.1f %5.1f %5.1f      %+5.1f %+5.1f %+5.1f' % ((v,) + a + b + tuple(y - x for x, y in zip(a, b))))


if __name__ == '__main__':
    main()
