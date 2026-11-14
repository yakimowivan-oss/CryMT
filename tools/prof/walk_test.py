r"""Walk test results (walk_test.ps1): the target's brightness at the screen's centre per distance and variant, and its
ratio to the reference variant (no GI). A GI that treats a surface the same at every distance keeps the ratio flat.
  python tools\prof\walk_test.py <dir> <tag> <reference> <variant> [<variant> ...] [--window 0.04]
Files: <dir>/<tag>_<variant>_<distance>.ppm.
"""
import glob
import os
import re
import sys

import numpy as np
from PIL import Image


def centre(path, frac):
    a = np.asarray(Image.open(path).convert('RGB'), dtype=np.float32) @ np.array([0.2126, 0.7152, 0.0722], dtype=np.float32)
    h, w = a.shape
    r = max(int(h * frac), 4)
    # a little above the crosshair (the weapon's sight sits at the centre's lower edge)
    cy = int(h * 0.47)
    return float(a[cy - r:cy + r, w // 2 - r:w // 2 + r].mean())


def main():
    args = sys.argv[1:]
    frac = 0.04
    if '--window' in args:
        frac = float(args[args.index('--window') + 1])
        args = args[:args.index('--window')] + args[args.index('--window') + 2:]
    folder, tag, ref = args[0], args[1], args[2]
    variants = args[3:]
    dists = sorted({float(re.search(r'_([0-9.]+)\.ppm$', p).group(1)) for p in glob.glob(os.path.join(folder, '%s_%s_*.ppm' % (tag, ref)))})
    print('%-8s' % 'dist m' + '  %-10s' % ref + ''.join('  %-18s' % v for v in variants))
    for d in dists:
        ds = ('%g' % d)
        r = centre(os.path.join(folder, '%s_%s_%s.ppm' % (tag, ref, ds)), frac)
        row = '%-8s  %-10.1f' % (ds, r)
        for v in variants:
            p = os.path.join(folder, '%s_%s_%s.ppm' % (tag, v, ds))
            if os.path.exists(p):
                m = centre(p, frac)
                row += '  %6.1f x%-9.2f' % (m, m / max(r, 1e-3))
            else:
                row += '  %-18s' % '-'
        print(row)


if __name__ == '__main__':
    main()
