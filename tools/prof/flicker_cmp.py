r"""Frame-to-frame flicker of several series of consecutive frames of one still view (frames_spot.ps1), compared:
per pixel the spread (std) of luminance over the frames; reported over the view's upper 60% without the panel box
(left) for all pixels and for foliage (green ones), with the share of pixels over 4 and 8 levels, and the first
frame's mean brightness. Optionally saves a picture of the spread (x8) for each series side by side.
  python tools\prof\flicker_cmp.py <dir> <out.png|-> <name>=<prefix> ...   (files <dir>/<prefix>_c1..cN.ppm)
"""
import glob
import os
import re
import sys

import numpy as np
from PIL import Image


def lum(a):
    return a[..., 0] * 0.2126 + a[..., 1] * 0.7152 + a[..., 2] * 0.0722


def main():
    d, out, items = sys.argv[1], sys.argv[2], sys.argv[3:]
    pics = []
    print('%-10s %6s  %7s %7s %7s   %7s %7s %7s' % ('series', 'mean', 'all std', '>4', '>8', 'leaf std', '>4', '>8'))
    for it in items:
        name, prefix = it.split('=', 1)
        exact = re.compile(re.escape(os.path.basename(prefix)) + r'_c(\d+)\.ppm$')
        files = sorted((f for f in glob.glob(os.path.join(d, prefix + '_c*.ppm')) if exact.match(os.path.basename(f))),
                       key=lambda p: int(exact.match(os.path.basename(p)).group(1)))
        frames = np.stack([np.asarray(Image.open(f).convert('RGB'), dtype=np.float32) for f in files])
        h, w = frames.shape[1:3]
        L = lum(frames)
        std = L.std(axis=0)
        mask = np.zeros((h, w), bool)
        mask[int(0.08 * h):int(0.6 * h), :] = True  # below the overlay text, above the weapon
        mask[int(0.35 * h):, :int(0.28 * w)] = False  # the panel box
        first = frames[0]
        green = (first[..., 1] > first[..., 0] * 1.08) & (first[..., 1] > first[..., 2] * 1.08) & mask
        s_all, s_leaf = std[mask], std[green]
        print('%-10s %6.1f  %7.2f %6.1f%% %6.1f%%   %7.2f %6.1f%% %6.1f%%  (%d frames, leaves %.0f%%)' % (
            name, first.mean(), s_all.mean(), (s_all > 4).mean() * 100, (s_all > 8).mean() * 100,
            s_leaf.mean(), (s_leaf > 4).mean() * 100, (s_leaf > 8).mean() * 100, len(files), green.sum() / mask.sum() * 100))
        pics.append(np.clip(std * 8, 0, 255).astype(np.uint8))
    if out != '-':
        sw = pics[0].shape[1] // 3
        sh = pics[0].shape[0] // 3
        sheet = Image.new('L', (sw * len(pics), sh))
        for i, p in enumerate(pics):
            sheet.paste(Image.fromarray(p).resize((sw, sh)), (i * sw, 0))
        sheet.save(out)


main()
