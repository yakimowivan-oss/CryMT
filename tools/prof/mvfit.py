import os, sys
import numpy as np
sys.path.insert(0, r'D:\Games\Crysis-RE\tools\prof')
from dlaa_seq import read_f16, lum, shift
# mvfit.py <dir> <tag>: per frame, the vector field (px) fitted as mv = s * (pixel - centre) + t (a zoom s and a shift
# t), and the input picture's own move between frames fitted the same way (per-tile phase correlation, the jitter's change
# taken out): if the projection changes, both should agree.
folder, tag = sys.argv[1], sys.argv[2]
info = {}
for line in open(os.path.join(folder, f'{tag}_mvinfo.txt')):
    t = line.split()
    if len(t) < 4 or t[1] != 'jitter':
        continue
    info[int(t[0])] = (float(t[2]), float(t[3]))
prev = None
for n in range(40):
    p = os.path.join(folder, f'{tag}_mv{n:02d}_mvec.f16')
    if not os.path.exists(p):
        break
    mv = read_f16(p, 2)
    h, w = mv.shape[:2]
    px = mv * np.array([w, h], np.float32)
    ys, xs = np.mgrid[0:h:8, 0:w:8]
    sub = px[0:h:8, 0:w:8]
    X = np.stack([xs.ravel() - w / 2, ys.ravel() - h / 2], axis=1).astype(np.float64)
    # mv (previous minus current) = s * r + t: the picture moved by -mv
    A = np.concatenate([np.stack([X[:, 0], np.ones(len(X)), np.zeros(len(X))], 1), np.stack([X[:, 1], np.zeros(len(X)), np.ones(len(X))], 1)])
    bvec = np.concatenate([sub[..., 0].ravel(), sub[..., 1].ravel()])
    (s, tx, ty), *_ = np.linalg.lstsq(A, bvec, rcond=None)
    line = f'{n}: vectors: zoom {s * 1e4:+.3f}e-4 (edge {s * w / 2:+.3f} px) shift {tx:+.3f} {ty:+.3f}'
    li = lum(read_f16(os.path.join(folder, f'{tag}_mv{n:02d}_in.f16'), 4))
    if prev is not None:
        T = 128
        rows = []
        for y in range(64, h - T - 64, T):
            for x in range(64, w - T - 64, T):
                b = li[y:y + T, x:x + T]
                if b.std() < 0.01:
                    continue
                dx, dy, pk = shift(prev[y:y + T, x:x + T], b)
                if pk > 0.2:
                    rows.append((x + T / 2 - w / 2, y + T / 2 - h / 2, dx, dy))
        R = np.array(rows, np.float64)
        dj = np.array(info[n]) - np.array(info[n - 1])
        A2 = np.concatenate([np.stack([R[:, 0], np.ones(len(R)), np.zeros(len(R))], 1), np.stack([R[:, 1], np.zeros(len(R)), np.ones(len(R))], 1)])
        b2 = np.concatenate([R[:, 2] - dj[0], R[:, 3] - dj[1]])
        (s2, tx2, ty2), *_ = np.linalg.lstsq(A2, -b2, rcond=None)  # as "previous minus current"
        line += f' | picture: zoom {s2 * 1e4:+.3f}e-4 (edge {s2 * w / 2:+.3f} px) shift {tx2:+.3f} {ty2:+.3f} ({len(R)} tiles)'
    print(line)
    prev = li
