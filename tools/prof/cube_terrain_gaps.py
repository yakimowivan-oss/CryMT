r"""Terrain missing from the engine cube's faces: the share of whitish pixels (bright, little colour: sky or fog showing
where the ground should be) in each face's ground half, per step of a cube_modes.ps1 run.
  python tools\prof\cube_terrain_gaps.py <dir> <tag> <step> ...
Side faces (0-3): the half below the horizon. Faces are stored flipped vertically (the engine's layout): rows 0..h/2 are
the lower half. -Z (5): all of it. +Z (4) left out.
"""
import os
import sys

import numpy as np
from PIL import Image


def whitish(a):
    mx, mn = a.max(axis=2), a.min(axis=2)
    return (mx > 170) & ((mx - mn) < 0.25 * mx)


def main():
    d, tag, steps = sys.argv[1], sys.argv[2], sys.argv[3:]
    print('step        +X     -X     +Y     -Y     -Z')
    for s in steps:
        cells = []
        for k in (0, 1, 2, 3, 5):
            a = np.asarray(Image.open(os.path.join(d, 'cube_%s_%s_envcube%d.ppm' % (tag, s, k))).convert('RGB'), dtype=np.float32)
            h = a.shape[0]
            part = a if k == 5 else a[:h // 2]
            cells.append('%5.1f%%' % (whitish(part).mean() * 100))
        print('%-10s ' % s + '  '.join(cells))


main()
