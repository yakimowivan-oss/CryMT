r"""Two captures of the same view (PPM): mean luminance of each, of their difference, and where it lies (a 4x4 grid of
the frame, rows top to bottom), plus a picture of B - A (grey 128 = same, darker = B darker, x4).
  python tools\prof\ppm_diff.py A.ppm B.ppm out.png
"""
import sys

import numpy as np
from PIL import Image


def lum(path):
    a = np.asarray(Image.open(path).convert('RGB'), dtype=np.float32)
    return 0.2126 * a[..., 0] + 0.7152 * a[..., 1] + 0.0722 * a[..., 2]


def main():
    a, b = lum(sys.argv[1]), lum(sys.argv[2])
    d = b - a
    print('mean luminance A %.2f, B %.2f, B - A %.2f; |B - A| > 4: %.1f%% of pixels' % (a.mean(), b.mean(), d.mean(),
                                                                                       100.0 * (np.abs(d) > 4).mean()))
    h, w = d.shape
    print('B - A by region (rows top to bottom, 4 x 4):')
    for r in range(4):
        print('  ' + '  '.join('%6.2f' % d[r * h // 4:(r + 1) * h // 4, c * w // 4:(c + 1) * w // 4].mean() for c in range(4)))
    Image.fromarray(np.clip(128 + 4 * d, 0, 255).astype(np.uint8)).resize((w // 2, h // 2)).save(sys.argv[3])


if __name__ == '__main__':
    main()
