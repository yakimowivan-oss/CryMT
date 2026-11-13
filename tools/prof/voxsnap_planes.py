r"""Where a voxel snapshot's opaque cells lie: per level, the cells per slice along x, y and z (world cell index), the
fullest slices first. Walls (a whole slice filled: a vertex shader that pins depth) show as slices near n * n.
  python tools\prof\voxsnap_planes.py <dir> <name> [top]
"""
import os
import sys

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from voxsnap_diff import load, texel_cells  # noqa: E402


def main():
    folder, name = sys.argv[1], sys.argv[2]
    top = int(sys.argv[3]) if len(sys.argv) > 3 else 6
    s = load(os.path.join(folder, 'voxsnap_%s.bin' % name))
    n = s['n']
    m = s['to_world']
    print('%s: camera %.1f %.1f %.1f' % (name, m[12], m[13], m[14]))
    for l, lv in enumerate(s['levels']):
        x, y, z = texel_cells(n, lv['grid'])
        op = lv['a'] >= 128
        size = s['cell0'] * (1 << l)
        print('level %d (%.2f m): opaque %d of %d; camera cell %d %d %d; grid from %s' % (
            l, size, op.sum(), op.size, int(m[12] // size), int(m[13] // size), int(m[14] // size), list(lv['grid'])))
        for axis, c in (('x', x), ('y', y), ('z', z)):
            counts = np.bincount(c[op] - c.min(), minlength=n)
            order = np.argsort(counts)[::-1][:top]
            print('  along %s: fullest slices %s; median slice %d' % (
                axis, ', '.join('%d:%d' % (c.min() + k, counts[k]) for k in order), int(np.median(counts))))


if __name__ == '__main__':
    main()
