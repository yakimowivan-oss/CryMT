r"""Consecutive-frame differences of frame series (frames_spot.ps1): per pair k -> k+1 the mean absolute luminance
difference over the view's upper 60% without the panel box, to tell even flicker from single frames that jump.
  python tools\prof\pair_diffs.py <dir> <name>=<prefix> ...
"""
import glob
import os
import re
import sys

import numpy as np
from PIL import Image


def lum(a):
    return a[..., 0] * 0.2126 + a[..., 1] * 0.7152 + a[..., 2] * 0.0722


def main():
    d, items = sys.argv[1], sys.argv[2:]
    for it in items:
        name, prefix = it.split('=', 1)
        exact = re.compile(re.escape(os.path.basename(prefix)) + r'_c(\d+)\.ppm$')
        files = sorted((f for f in glob.glob(os.path.join(d, prefix + '_c*.ppm')) if exact.match(os.path.basename(f))),
                       key=lambda p: int(exact.match(os.path.basename(p)).group(1)))
        frames = [lum(np.asarray(Image.open(f).convert('RGB'), dtype=np.float32)) for f in files]
        h, w = frames[0].shape
        mask = np.zeros((h, w), bool)
        mask[int(0.08 * h):int(0.6 * h), :] = True
        mask[int(0.35 * h):, :int(0.28 * w)] = False
        diffs = [np.abs(frames[k + 1] - frames[k])[mask].mean() for k in range(len(frames) - 1)]
        print('%-12s ' % name + ' '.join('%5.2f' % x for x in diffs))


main()
