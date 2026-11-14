r"""Per face of the engine cube, how far each culling mode's face is from the reference (no culling): closed meshes look
the same with the right culling, the wrong one shows through. Share of pixels off by more than 24/255 and the mean
difference, per face 0-5 (+X -X +Y -Y +Z -Z).
  python tools\prof\cube_faces_vs.py <dir> <tag> <reference step> <step> ...   (files cube_<tag>_<step>_envcube<k>.ppm)
"""
import os
import sys

import numpy as np
from PIL import Image


def load(p):
    return np.asarray(Image.open(p).convert('RGB'), dtype=np.float32)


def main():
    d, tag, ref, steps = sys.argv[1], sys.argv[2], sys.argv[3], sys.argv[4:]
    print('face        ' + '  '.join('%-16s' % s for s in steps))
    for k in range(6):
        r = load(os.path.join(d, 'cube_%s_%s_envcube%d.ppm' % (tag, ref, k)))
        cells = []
        for s in steps:
            x = load(os.path.join(d, 'cube_%s_%s_envcube%d.ppm' % (tag, s, k)))
            diff = np.abs(x - r).max(axis=2)
            cells.append('%5.1f%% mean %5.1f' % ((diff > 24).mean() * 100, diff.mean()))
        print('face %d      ' % k + '  '.join('%-16s' % c for c in cells))


main()
