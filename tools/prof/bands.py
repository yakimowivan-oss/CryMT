r"""Frame brightness by distance band against a reference variant (no GI): per band the mean luminance of each variant's
frame and its ratio to the reference, so a jump between neighbouring bands (the probes' edge, the cones' mid range,
the GI's fade) shows as a step in the ratio. Distances from a variant's <tag>_dist.r32 (the GI's own size, metres).
  python tools\prof\bands.py <dir> <spot> <reference variant> <variant> [<variant> ...] [--dist <variant>]
Files: <dir>/out_<spot>_<variant>_frame.ppm and out_<spot>_<variant>_dist.r32 (crymt_capture via user_spots.ps1).
"""
import os
import struct
import sys

import numpy as np
from PIL import Image

BANDS = [0, 4, 8, 16, 24, 32, 48, 75, 120, 1e9]


def lum(path):
    a = np.asarray(Image.open(path).convert('RGB'), dtype=np.float32)
    return a @ np.array([0.2126, 0.7152, 0.0722], dtype=np.float32)


def read_dist(path):
    with open(path, 'rb') as f:
        w, h = struct.unpack('<II', f.read(8))
        return np.frombuffer(f.read(), dtype=np.float32)[:w * h].reshape(h, w)


def main():
    args = sys.argv[1:]
    dist_from = None
    if '--dist' in args:
        dist_from = args[args.index('--dist') + 1]
        args = args[:args.index('--dist')] + args[args.index('--dist') + 2:]
    folder, spot, ref = args[0], args[1], args[2]
    variants = args[3:]
    name = lambda v, k, e: os.path.join(folder, 'out_%s_%s_%s.%s' % (spot, v, k, e))
    dist_v = dist_from or next(v for v in variants if os.path.exists(name(v, 'dist', 'r32')))
    d = read_dist(name(dist_v, 'dist', 'r32'))
    frames = {v: lum(name(v, 'frame', 'ppm')) for v in [ref] + variants}
    h, w = frames[ref].shape
    yi = (np.arange(h) * d.shape[0] // h)[:, None]
    xi = (np.arange(w) * d.shape[1] // w)[None, :]
    full = d[yi, xi]
    # leave out the HUD and the weapon roughly: the lower right quarter and the left panel strip
    mask = np.ones((h, w), bool)
    mask[int(h * 0.55):, int(w * 0.45):] = False
    mask[int(h * 0.3):int(h * 0.65), :int(w * 0.22)] = False
    print('spot %s, distances from %s; ratio = variant / %s' % (spot, dist_v, ref))
    head = '%-10s %7s' % ('band m', 'pixels') + ''.join('  %-16s' % v[:16] for v in [ref] + variants)
    print(head)
    for lo, hi in zip(BANDS[:-1], BANDS[1:]):
        sel = mask & (full >= lo) & (full < hi) & (full > 0)
        n = int(sel.sum())
        if n < 500:
            continue
        r = frames[ref][sel].mean()
        cells = ['  %6.1f          ' % r]
        for v in variants:
            m = frames[v][sel].mean()
            cells.append('  %6.1f x%-6.2f  ' % (m, m / max(r, 1e-3)))
        print('%-10s %7d' % ('%g-%s' % (lo, '' if hi > 1e8 else '%g' % hi), n) + ''.join(cells))


if __name__ == '__main__':
    main()
