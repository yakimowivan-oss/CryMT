import os, sys
import numpy as np
sys.path.insert(0, r'D:\Games\Crysis-RE\tools\prof')
from dlaa_seq import read_f16, tone
from mv_check import bilinear
# objshift.py <dir> <tag>: per moving object (connected blobs of the exact object motion target), the shift that best
# maps the previous frame onto its pixels (search around its mean vector, 0.25 px steps; also a per-pixel best of
# "vector" vs "vector + linear in position" for rotation) against the vectors. No jitter captures only.

def label(mask):
    """4-connected components (BFS)."""
    lab = np.zeros(mask.shape, np.int32)
    cnt = 0
    h, w = mask.shape
    for y0, x0 in zip(*np.nonzero(mask)):
        if lab[y0, x0]:
            continue
        cnt += 1
        stack = [(y0, x0)]
        lab[y0, x0] = cnt
        while stack:
            y, x = stack.pop()
            for yy, xx in ((y - 1, x), (y + 1, x), (y, x - 1), (y, x + 1)):
                if 0 <= yy < h and 0 <= xx < w and mask[yy, xx] and not lab[yy, xx]:
                    lab[yy, xx] = cnt
                    stack.append((yy, xx))
    return lab, cnt


def erode(m, k):
    for _ in range(k):
        m = m & np.roll(m, 1, 0) & np.roll(m, -1, 0) & np.roll(m, 1, 1) & np.roll(m, -1, 1)
    return m

def main():
    folder, tag = sys.argv[1], sys.argv[2]
    for n in range(1, 40):
        p = os.path.join(folder, f'{tag}_mv{n:02d}_in.f16')
        if not os.path.exists(p):
            break
        cur = tone(read_f16(p, 4)).mean(axis=2)
        prev = tone(read_f16(os.path.join(folder, f'{tag}_mv{n - 1:02d}_in.f16'), 4)).mean(axis=2)
        mv = read_f16(os.path.join(folder, f'{tag}_mv{n:02d}_mvec.f16'), 2)
        om = read_f16(os.path.join(folder, f'{tag}_mv{n:02d}_objmv.f16'), 4)
        h, w = cur.shape
        mvp = mv * np.array([w, h], np.float32)
        mask = om[..., 3] > 0.75
        lab, cnt = label(mask)
        ys, xs = np.mgrid[0:h, 0:w].astype(np.float32)
        out = []
        for k in range(1, cnt + 1):
            blob = erode(lab == k, 2)  # inner pixels: no edges against the background
            if blob.sum() < 200:
                continue
            v = mvp[blob].mean(axis=0)
            # error of the per-pixel vectors as they are
            e_vec = np.abs(cur[blob] - bilinear(prev[..., None], xs[blob] + mvp[blob][:, 0], ys[blob] + mvp[blob][:, 1])[:, 0]).mean()
            best = (1e9, 0, 0)
            for dx in np.arange(-4, 4.01, 0.25):
                for dy in np.arange(-4, 4.01, 0.25):
                    e = np.abs(cur[blob] - bilinear(prev[..., None], xs[blob] + mvp[blob][:, 0] + dx, ys[blob] + mvp[blob][:, 1] + dy)[:, 0]).mean()
                    if e < best[0]:
                        best = (e, dx, dy)
            e0 = np.abs(cur[blob] - prev[blob]).mean()
            out.append(f'blob {k} ({blob.sum()} px): vector {v[0]:+.2f} {v[1]:+.2f}; best correction {best[1]:+.2f} {best[2]:+.2f}; '
                       f'error: no move {e0:.4f}, vectors {e_vec:.4f}, best {best[0]:.4f}')
        print(f'{n - 1}-{n}: ' + ('; '.join(out) if out else 'no blobs'))


if __name__ == '__main__':
    main()
