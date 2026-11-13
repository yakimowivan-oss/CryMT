r"""How the engine cube's injected cells (stamp frame 1, opacity byte 253) relate to what the camera finds when it turns
to them: A = a snapshot before the turn, B = after (same place). Level 0..2: the cube cells of A, how many B keeps, how
many cells appeared in B, and of those how many lie next to (one cell from) a cube cell of A: a surface the cube put
one cell off. Also the light change of A's cube cells that B keeps. Compared per texel (texel = cell mod n, the same
world cell where both grids hold it; neighbours by toroidal rolls).
  python tools\prof\voxsnap_cubecheck.py <dir> A B
"""
import os
import sys

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from voxsnap_diff import load, texel_cells  # noqa: E402

CUBE = 256 + 253


def dilate(m):
    out = m.copy()
    for axis in range(3):
        for shift in (-1, 1):
            out |= np.roll(m, shift, axis=axis)
    return out


def main():
    folder, na, nb = sys.argv[1], sys.argv[2], sys.argv[3]
    a = load(os.path.join(folder, 'voxsnap_%s.bin' % na))
    b = load(os.path.join(folder, 'voxsnap_%s.bin' % nb))
    n = a['n']
    for l in range(len(a['levels'])):
        la, lb = a['levels'][l], b['levels'][l]
        x, y, z = texel_cells(n, la['grid'])
        gb = lb['grid']
        both = ((x >= gb[0]) & (x < gb[0] + n) & (y >= gb[1]) & (y < gb[1] + n) & (z >= gb[2]) & (z < gb[2] + n)).reshape(n, n, n)
        oa = (la['a'] >= 128).reshape(n, n, n) & both
        ob = (lb['a'] >= 128).reshape(n, n, n) & both
        cube_a = oa & (la['stamp'] == CUBE).reshape(n, n, n)
        appeared = ob & ~oa
        near = appeared & dilate(cube_a)
        kept = cube_a & ob
        dl = np.abs(lb['lum'].astype(np.int16) - la['lum'].astype(np.int16)).reshape(n, n, n)[kept]
        print('level %d: cube cells in A %d (of %d opaque), kept in B %d, went %d; appeared in B %d, next to a cube cell of A %d (%.0f%%)%s' % (
            l, cube_a.sum(), oa.sum(), kept.sum(), (cube_a & ~ob).sum(), appeared.sum(), near.sum(),
            100.0 * near.sum() / max(appeared.sum(), 1),
            '; kept cube cells light change mean %.1f, > 48: %.1f%%' % (dl.mean(), 100.0 * (dl > 48).mean()) if dl.size else ''))


if __name__ == '__main__':
    main()
