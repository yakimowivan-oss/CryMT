# Contact shadows A/B from one sss_ab.ps1 run: the newest 6 key_*_w1_c*.ppm in CryMT\logs\play are, in time order,
# SSS x2, the old 12-step contact shadows x2, none x2. Prints per mode the mean brightness, the pixels darker than
# without contact shadows (by > 8 levels of the luma), the noise floor (the two frames of one mode against each other),
# and the share of the SSS-darkened pixels the old ones darken too. Writes a mask of where SSS darkens (white) next to
# the captures (<first>_sssmask.png) and crops with the most SSS darkening side by side (<first>_sss_crops.png).
#   python tools\prof\sss_compare.py [play dir]
import glob
import os
import sys

import numpy as np
from PIL import Image

play = sys.argv[1] if len(sys.argv) > 1 else r'D:\Games\Crysis\CryMT\logs\play'
files = sorted(glob.glob(os.path.join(play, 'key_*_w1_c*.ppm')), key=os.path.getmtime)[-6:]
if len(files) < 6:
    sys.exit('need 6 captures, found %d' % len(files))
names = ['sss a', 'sss b', 'old a', 'old b', 'off a', 'off b']
img = [np.asarray(Image.open(f).convert('RGB'), dtype=np.float32) for f in files]
luma = [i[..., 0] * 0.299 + i[..., 1] * 0.587 + i[..., 2] * 0.114 for i in img]
for n, f in zip(names, files):
    print('%-6s %s' % (n, os.path.basename(f)))
total = luma[0].size
off = luma[4]


def darker(a, b):
    return (a < b - 8)


print('noise floor (frame a vs b, |d| > 8): sss %.2f%%, old %.2f%%, off %.2f%%' % tuple(
    100.0 * np.mean(np.abs(luma[k] - luma[k + 1]) > 8) for k in (0, 2, 4)))
for k, n in ((0, 'sss'), (2, 'old')):
    d = darker(luma[k], off)
    print('%s: mean luma %.2f (off %.2f); darker than off by > 8: %.2f%% of the screen (%d px); mean darkening there %.1f' % (
        n, luma[k].mean(), off.mean(), 100.0 * d.mean(), d.sum(), float((off - luma[k])[d].mean()) if d.any() else 0.0))
noise = darker(luma[5], off) | darker(off, luma[5])
print('off b vs off a darker by > 8 (the same comparison on noise alone): %.2f%%' % (100.0 * darker(luma[5], off).mean()))
ds, do = darker(luma[0], off), darker(luma[2], off)
print('overlap: of the SSS-darkened pixels the old ones darken %.1f%%; of the old-darkened SSS darkens %.1f%%' % (
    100.0 * (ds & do).sum() / max(ds.sum(), 1), 100.0 * (ds & do).sum() / max(do.sum(), 1)))
base = os.path.splitext(files[0])[0]
Image.fromarray((ds * 255).astype(np.uint8)).save(base + '_sssmask.png')
# Crops: the 3 256x256 tiles with the most SSS darkening, each sss | old | off.
h, w = off.shape
t = 256
best = []
for y in range(0, h - t + 1, t // 2):
    for x in range(0, w - t + 1, t // 2):
        best.append((int(ds[y:y + t, x:x + t].sum()), x, y))
best.sort(reverse=True)
picked = []
for s, x, y in best:
    if all(abs(x - px) >= t or abs(y - py) >= t for _, px, py in picked):
        picked.append((s, x, y))
    if len(picked) == 3:
        break
sheet = np.zeros((t * len(picked), t * 3, 3), dtype=np.uint8)
for r, (s, x, y) in enumerate(picked):
    for c, k in enumerate((0, 2, 4)):
        sheet[r * t:(r + 1) * t, c * t:(c + 1) * t] = img[k][y:y + t, x:x + t].astype(np.uint8)
    print('crop %d at %d,%d: %d SSS-darkened px' % (r + 1, x, y, s))
Image.fromarray(sheet).save(base + '_sss_crops.png')
print('wrote', base + '_sssmask.png', base + '_sss_crops.png')
