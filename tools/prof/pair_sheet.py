r"""Side-by-side sheet of captures for variant pairs, with the mean luminance of each (frame, and of the GI's bounce
light and occlusion dumps when given), e.g. GI v2 against the cones at several places:
  python tools\prof\pair_sheet.py <dir> <out.png> <scale> <spot> [<spot> ...] --pair <variantA> <variantB> [--kinds frame,girgb,giocc]
Files: <dir>/out_<spot>_<variant>_<kind>.ppm.
"""
import os
import sys

import numpy as np
from PIL import Image


def lum(img):
    a = np.asarray(img.convert('RGB'), dtype=np.float32)
    return float((0.2126 * a[..., 0] + 0.7152 * a[..., 1] + 0.0722 * a[..., 2]).mean())


def main():
    args = sys.argv[1:]
    pair = args[args.index('--pair') + 1:args.index('--pair') + 3]
    kinds = ['frame']
    if '--kinds' in args:
        kinds = args[args.index('--kinds') + 1].split(',')
    folder, out, scale = args[0], args[1], float(args[2])
    spots = [a for a in args[3:args.index('--pair')]]
    rows = []
    for s in spots:
        for k in kinds:
            ims = []
            for v in pair:
                p = os.path.join(folder, 'out_%s_%s_%s.ppm' % (s, v, k))
                im = Image.open(p)
                print('%-5s %-6s %-12s mean %.1f' % (s, k, v, lum(im)))
                ims.append(im.resize((int(im.width * scale), int(im.height * scale))))
            rows.append(ims)
    w, h = rows[0][0].size
    sheet = Image.new('RGB', (w * 2 + 6, (h + 6) * len(rows)), (255, 255, 255))
    for r, ims in enumerate(rows):
        for c, im in enumerate(ims):
            sheet.paste(im.resize((w, h)), (c * (w + 6), r * (h + 6)))
    sheet.save(out)


if __name__ == '__main__':
    main()
