import os, sys
import numpy as np
sys.path.insert(0, r'D:\Games\Crysis-RE\tools\prof')
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from dlaa_seq import read_f16
from objshift import label
# centroid.py <dir> <tag>: per object blob (exact object motion target), the move of its silhouette's centroid from frame
# n-1 to n against the mean of its vectors (previous minus current), matched by the nearest blob after the vector move.
folder, tag = sys.argv[1], sys.argv[2]

def blobs(n):
    om = read_f16(os.path.join(folder, f'{tag}_mv{n:02d}_objmv.f16'), 4)
    mv = read_f16(os.path.join(folder, f'{tag}_mv{n:02d}_mvec.f16'), 2)
    h, w = om.shape[:2]
    mvp = mv * np.array([w, h], np.float32)
    lab, cnt = label(om[..., 3] > 0.75)
    out = []
    for k in range(1, cnt + 1):
        m = lab == k
        if m.sum() < 300:
            continue
        ys, xs = np.nonzero(m)
        touches = xs.min() == 0 or ys.min() == 0 or xs.max() == w - 1 or ys.max() == h - 1
        out.append((xs.mean(), ys.mean(), mvp[m].mean(axis=0), m.sum(), touches))
    return out

prev = None
for n in range(40):
    if not os.path.exists(os.path.join(folder, f'{tag}_mv{n:02d}_objmv.f16')):
        break
    cur = blobs(n)
    if prev is not None:
        for cx, cy, v, size, touch in cur:
            if touch:
                continue
            px, py = cx + v[0], cy + v[1]  # where the vectors say it was
            cand = [b for b in prev if not b[4]]
            if not cand:
                continue
            b = min(cand, key=lambda b: (b[0] - px) ** 2 + (b[1] - py) ** 2)
            real = (b[0] - cx, b[1] - cy)
            print(f'{n - 1}-{n}: blob {size} px at {cx:.0f},{cy:.0f}: vectors {v[0]:+.2f} {v[1]:+.2f}  centroid moved {real[0]:+.2f} {real[1]:+.2f}  '
                  f'(ratio x {real[0] / v[0] if abs(v[0]) > 0.5 else float("nan"):.3f})')
    prev = cur
