r"""Three projections of a voxel snapshot's opaque cells (top: x right, y up; front: x right, z up; side: y right, z up),
one level, as a PNG: grey = cells of B, red = cells of B that A does not have (A optional: another run at the same place,
same grid). Brightness = how many cells along the projected axis. The camera's cell is marked green.
  python tools\prof\voxsnap_views.py <dir> <level> <out.png> B [A]
"""
import os
import sys

import numpy as np
from PIL import Image

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from voxsnap_diff import load, texel_cells  # noqa: E402


def grid3(snap, level):
    n = snap['n']
    lv = snap['levels'][level]
    x, y, z = texel_cells(n, lv['grid'])
    g = np.zeros((n, n, n), dtype=bool)  # [x, y, z] relative to the grid's first cell
    op = lv['a'] >= 128
    g[(x - lv['grid'][0])[op], (y - lv['grid'][1])[op], (z - lv['grid'][2])[op]] = True
    return g, lv['grid']


def main():
    folder, level, out, nb = sys.argv[1], int(sys.argv[2]), sys.argv[3], sys.argv[4]
    na = sys.argv[5] if len(sys.argv) > 5 else None
    b = load(os.path.join(folder, 'voxsnap_%s.bin' % nb))
    gb, first = grid3(b, level)
    extra = np.zeros_like(gb)
    if na:
        a = load(os.path.join(folder, 'voxsnap_%s.bin' % na))
        ga, first_a = grid3(a, level)
        if list(first_a) != list(first):
            print('grids differ: %s vs %s' % (list(first_a), list(first)))
        extra = gb & ~ga
    n = gb.shape[0]
    size = b['cell0'] * (1 << level)
    cam = [int(b['to_world'][12 + i] // size - first[i]) for i in range(3)]
    panels = []
    for axis, (u, v) in ((2, (0, 1)), (1, (0, 2)), (0, (1, 2))):
        allc = gb.sum(axis=axis).astype(np.float32)
        red = extra.sum(axis=axis).astype(np.float32)
        img = np.zeros((n, n, 3), dtype=np.float32)
        base = np.sqrt(allc / max(allc.max(), 1))
        img[..., 0] = img[..., 1] = img[..., 2] = base * 200
        mask = red > 0
        img[mask, 0] = 90 + 165 * np.sqrt(red[mask] / max(red.max(), 1))
        img[mask, 1] *= 0.3
        img[mask, 2] *= 0.3
        cu, cv = cam[u], cam[v]
        if 0 <= cu < n and 0 <= cv < n:
            img[max(cu - 1, 0):cu + 2, max(cv - 1, 0):cv + 2] = (0, 255, 0)
        img = np.transpose(img, (1, 0, 2))[::-1]  # rows: v up
        panels.append(np.kron(img, np.ones((3, 3, 1))))
    sep = np.full((n * 3, 6, 3), 255, dtype=np.float32)
    Image.fromarray(np.concatenate([panels[0], sep, panels[1], sep, panels[2]], axis=1).astype(np.uint8)).save(out)
    print('%s level %d: B %d cells, only in B %d; camera cell in grid %s; %s' % (nb, level, gb.sum(), extra.sum(), cam, out))


if __name__ == '__main__':
    main()
