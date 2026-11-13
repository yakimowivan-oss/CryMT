r"""Are the voxel levels consistent where they overlap? One snapshot: for distance bands around the camera, per geometry
level that covers the band, the opaque cells' volume share, their mean opacity and their mean light (u8 of 1 - exp(-lum)),
split by where the colour came from (the screen: seen bit 0; else the light pass / old). A level that is brighter or
emptier than the finer one in the same band is what shows as a lighter ring where the level changes.
  python tools\prof\voxsnap_levels.py <dir> <name> [<name> ...]
"""
import os
import sys

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from voxsnap_diff import load, texel_cells  # noqa: E402

BANDS = [(0, 4), (4, 8), (8, 16), (16, 32), (32, 64)]


def main():
    folder = sys.argv[1]
    for name in sys.argv[2:]:
        s = load(os.path.join(folder, 'voxsnap_%s.bin' % name))
        n = s['n']
        eye = np.array(s['to_world'][12:15])
        print('%s: camera %.1f %.1f %.1f' % (name, eye[0], eye[1], eye[2]))
        print('  band m    level  cells   opaque%%  mean a  light(seen)  light(unseen)  seen%%')
        for lo, hi in BANDS:
            for l, lv in enumerate(s['levels']):
                size = s['cell0'] * (1 << l)
                half = size * n / 2
                if hi > half * 0.9:  # the band must lie inside this level's grid
                    continue
                x, y, z = texel_cells(n, lv['grid'])
                c = np.stack([(x + 0.5) * size, (y + 0.5) * size, (z + 0.5) * size], 1) - eye
                d = np.linalg.norm(c, axis=1)
                band = (d >= lo) & (d < hi)
                a = lv['a'][band].astype(np.float32) / 255
                op = a >= 0.5
                seen = (lv['seen'][band] & 1) > 0
                lum = lv['lum'][band].astype(np.float32)
                ls = lum[op & seen]
                lu = lum[op & ~seen]
                print('  %3d-%-3d   %d     %7d  %6.2f   %6.3f   %6.1f (%5d)  %6.1f (%5d)  %5.1f' % (
                    lo, hi, l, band.sum(), 100.0 * op.mean() if band.any() else 0, a[op].mean() if op.any() else 0,
                    ls.mean() if ls.size else 0, ls.size, lu.mean() if lu.size else 0, lu.size,
                    100.0 * (op & seen).sum() / max(op.sum(), 1)))


if __name__ == '__main__':
    main()
