r"""Voxel snapshots compared cell by cell in the world (CryMT's crymt_voxsnap <name> -> CryMT\logs\play\voxsnap_<name>.bin,
written by D3D12Backend.cpp WriteVoxSnap). For each pair A B and each geometry level: opaque cells (a >= 0.5) in A and B
over the cells both grids hold, how many appeared / went, split by where they lie for B's camera (in its view cone or
outside it), whether they hold a geometry stamp, and how much the light of cells opaque in both changed.
  python tools\prof\voxsnap_diff.py <dir> A B [A B ...]      (names without voxsnap_ / .bin)
  python tools\prof\voxsnap_diff.py <dir> --one A             (one snapshot: counts only)
"""
import math
import os
import struct
import sys

import numpy as np

VIEW_HALF_ANGLE = 50.0  # degrees around the camera's forward that count as "in view" (2560x1440 at the game's 60-degree
# vertical field of view: about 46 horizontally, 30 vertically; 50 takes the frame's corners)


def load(path):
    with open(path, 'rb') as f:
        data = f.read()
    if data[:4] != b'VXS1':
        raise ValueError(path + ': not a voxel snapshot')
    n, levels = struct.unpack_from('<II', data, 4)
    (frame,) = struct.unpack_from('<Q', data, 12)
    to_world = struct.unpack_from('<16f', data, 20)
    (cell0,) = struct.unpack_from('<f', data, 84)
    at = 88
    cells = n * n * n
    out = {'n': n, 'frame': frame, 'to_world': to_world, 'cell0': cell0, 'levels': []}
    for _ in range(levels):
        grid = np.array(struct.unpack_from('<3i', data, at), dtype=np.int64)
        at += 12
        a = np.frombuffer(data, np.uint8, cells, at); at += cells
        lum = np.frombuffer(data, np.uint8, cells, at); at += cells
        stamp = np.frombuffer(data, np.uint32, cells, at); at += cells * 4
        seen = np.frombuffer(data, np.uint8, cells, at); at += cells
        out['levels'].append({'grid': grid, 'a': a, 'lum': lum, 'stamp': stamp, 'seen': seen})
    return out


def texel_cells(n, grid):
    """World cell of every texel (x fastest): cell = g + ((t - g) mod n) per axis."""
    t = np.arange(n, dtype=np.int64)
    cx = grid[0] + np.mod(t - grid[0], n)
    cy = grid[1] + np.mod(t - grid[1], n)
    cz = grid[2] + np.mod(t - grid[2], n)
    z, y, x = np.meshgrid(cz, cy, cx, indexing='ij')
    return x.ravel(), y.ravel(), z.ravel()


def camera(s):
    m = s['to_world']
    pos = np.array(m[12:15])
    fwd = -np.array(m[8:11])
    fwd /= max(np.linalg.norm(fwd), 1e-6)
    return pos, fwd


def in_view(snap, level, x, y, z):
    pos, fwd = camera(snap)
    size = snap['cell0'] * (1 << level)
    c = np.stack([(x + 0.5) * size, (y + 0.5) * size, (z + 0.5) * size], 1) - pos
    d = np.linalg.norm(c, axis=1)
    cos = (c @ fwd) / np.maximum(d, 1e-6)
    return cos >= math.cos(math.radians(VIEW_HALF_ANGLE))


def describe(name, s):
    pos, fwd = camera(s)
    yaw = math.degrees(math.atan2(fwd[1], fwd[0]))
    return '%s: GI frame %d, camera %.1f %.1f %.1f, forward %.2f %.2f %.2f (yaw %.0f deg from +x)' % (
        name, s['frame'], pos[0], pos[1], pos[2], fwd[0], fwd[1], fwd[2], yaw)


def one(name, s, lines):
    lines.append(describe(name, s))
    for l, lv in enumerate(s['levels']):
        x, y, z = texel_cells(s['n'], lv['grid'])
        op = lv['a'] >= 128
        view = in_view(s, l, x, y, z)
        stamped = (lv['stamp'] >> 8) > 0
        lines.append('  level %d (%.2f m): opaque %d (in view %d, outside %d), with a geometry stamp %d, seen by the screen %d' % (
            l, s['cell0'] * (1 << l), op.sum(), (op & view).sum(), (op & ~view).sum(), (op & stamped).sum(),
            (op & (lv['seen'] & 1 > 0)).sum()))


def pair(na, a, nb, b, lines):
    lines.append('== %s -> %s (%d GI frames apart)' % (na, nb, b['frame'] - a['frame']))
    lines.append('  ' + describe(na, a))
    lines.append('  ' + describe(nb, b))
    n = a['n']
    for l, (la, lb) in enumerate(zip(a['levels'], b['levels'])):
        x, y, z = texel_cells(n, la['grid'])
        gb = lb['grid']
        both_hold = (x >= gb[0]) & (x < gb[0] + n) & (y >= gb[1]) & (y < gb[1] + n) & (z >= gb[2]) & (z < gb[2] + n)
        oa = (la['a'] >= 128) & both_hold
        ob = (lb['a'] >= 128) & both_hold
        appeared, went, kept = ob & ~oa, oa & ~ob, oa & ob
        view_b = in_view(b, l, x, y, z)
        view_a = in_view(a, l, x, y, z)
        stamp_a = (la['stamp'] >> 8) > 0
        dl = np.abs(lb['lum'].astype(np.int16) - la['lum'].astype(np.int16))[kept]
        lines.append('  level %d (%.2f m): opaque A %d, B %d, kept %d; went %d (%.1f%% of A), appeared %d' % (
            l, a['cell0'] * (1 << l), oa.sum(), ob.sum(), kept.sum(), went.sum(), 100.0 * went.sum() / max(oa.sum(), 1),
            appeared.sum()))
        for label, mask in (('in B\'s view', view_b), ('outside B\'s view', ~view_b)):
            a_here = (oa & mask).sum()
            lines.append('    %-18s A opaque %7d, went %7d (%5.1f%%), appeared %6d' % (
                label, a_here, (went & mask).sum(), 100.0 * (went & mask).sum() / max(a_here, 1), (appeared & mask).sum()))
        lines.append('    went: in A\'s view %d, outside it %d; with a geometry stamp in A %d, without %d' % (
            (went & view_a).sum(), (went & ~view_a).sum(), (went & stamp_a).sum(), (went & ~stamp_a).sum()))
        # The stamp's opacity byte (RunVoxelGeometry): 255 solid, 254 LOD dissolve (solid), 127 foliage (CRYMT_GI_FOLIAGE 0.5).
        kind_b = lb['stamp'] & 255
        lines.append('    appeared by B\'s stamp: solid %d, dissolve %d, foliage %d, other %d, none %d' % (
            (appeared & (kind_b == 255)).sum(), (appeared & (kind_b == 254)).sum(), (appeared & (kind_b == 127)).sum(),
            (appeared & (kind_b != 255) & (kind_b != 254) & (kind_b != 127) & (lb['stamp'] > 0)).sum(), (appeared & (lb['stamp'] == 0)).sum()))
        if dl.size:
            lines.append('    light of the kept cells: mean change %.2f (of 255), changed by more than 16: %.2f%%, by more than 48: %.2f%%' % (
                dl.mean(), 100.0 * (dl > 16).mean(), 100.0 * (dl > 48).mean()))


def main():
    if len(sys.argv) < 3:
        print(__doc__)
        return 1
    folder = sys.argv[1]
    names = sys.argv[2:]
    path = lambda name: os.path.join(folder, 'voxsnap_%s.bin' % name)
    lines = []
    if names[0] == '--one':
        for name in names[1:]:
            one(name, load(path(name)), lines)
    else:
        cache = {}
        get = lambda name: cache.setdefault(name, load(path(name)))
        for i in range(0, len(names) - 1, 2):
            pair(names[i], get(names[i]), names[i + 1], get(names[i + 1]), lines)
    print('\n'.join(lines))
    return 0


if __name__ == '__main__':
    sys.exit(main())
