r"""Voxel light inside a box (a VisArea's, from crymt_vis.txt): per level, the opaque cells whose centre lies in the box,
split by where their colour came from (the screen: seen bit 0; else the light pass), with their mean light (u8 of
1 - exp(-luminance)) and the share above 48. Unseen cells much brighter than the seen ones inside a room = light the
light pass gives them that the room does not get (the sun or the sky through missing cover).
  python tools\prof\voxsnap_box.py <dir> <name> x0 y0 z0 x1 y1 z1
"""
import os
import sys

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from voxsnap_diff import load, texel_cells  # noqa: E402


def main():
    folder, name = sys.argv[1], sys.argv[2]
    lo = np.array([float(v) for v in sys.argv[3:6]])
    hi = np.array([float(v) for v in sys.argv[6:9]])
    s = load(os.path.join(folder, 'voxsnap_%s.bin' % name))
    n = s['n']
    print('%s, box %s - %s' % (name, lo, hi))
    for l, lv in enumerate(s['levels']):
        size = s['cell0'] * (1 << l)
        x, y, z = texel_cells(n, lv['grid'])
        c = np.stack([(x + 0.5) * size, (y + 0.5) * size, (z + 0.5) * size], 1)
        inside = np.all((c >= lo) & (c <= hi), axis=1)
        op = (lv['a'] >= 128) & inside
        seen = (lv['seen'] & 1) > 0
        lum = lv['lum'].astype(np.float32)
        out = []
        for label, m in (('seen', op & seen), ('unseen', op & ~seen)):
            v = lum[m]
            out.append('%s %5d mean %5.1f, > 48: %5.1f%%' % (label, m.sum(), v.mean() if v.size else 0, 100.0 * (v > 48).mean() if v.size else 0))
        print('  level %d (%.2f m): %s | %s' % (l, size, out[0], out[1]))


if __name__ == '__main__':
    main()
