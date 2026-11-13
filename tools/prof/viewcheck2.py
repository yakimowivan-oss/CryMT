import os, sys
import numpy as np
sys.path.insert(0, r'D:\Games\Crysis-RE\tools\prof')
from dlaa_seq import read_f16, lum, shift
from mv_check import read_pgm
# viewcheck2.py <dir> <tag>: per pair, the input picture's move on tiles without foliage (mask) and without moving
# objects (exact object motion), jitter change out, against the vectors on the same tiles; and the DLAA output's move
# on the same tiles. Prints medians (px, previous minus current).
folder, tag = sys.argv[1], sys.argv[2]
jit = {}
for line in open(os.path.join(folder, f'{tag}_mvinfo.txt')):
    t = line.split()
    if len(t) > 3 and t[1] == 'jitter':
        jit[int(t[0])] = (float(t[2]), float(t[3]))
prev = prevo = None
acc = []
for n in range(40):
    p = os.path.join(folder, f'{tag}_mv{n:02d}_in.f16')
    if not os.path.exists(p):
        break
    li = lum(read_f16(p, 4))
    lo = lum(read_f16(os.path.join(folder, f'{tag}_mv{n:02d}_out.f16'), 4))
    h, w = li.shape
    mv = read_f16(os.path.join(folder, f'{tag}_mv{n:02d}_mvec.f16'), 2) * np.array([w, h], np.float32)
    fp = os.path.join(folder, f'{tag}_mv{n:02d}_foliage.pgm')
    fol = (read_pgm(fp) > 0) if os.path.exists(fp) else np.zeros((h, w), bool)
    op = os.path.join(folder, f'{tag}_mv{n:02d}_objmv.f16')
    obj = (read_f16(op, 4)[..., 3] > 0.25) if os.path.exists(op) else np.zeros((h, w), bool)
    if prev is not None:
        T = 96
        pic, out, vec = [], [], []
        for y in range(48, h - T - 48, T):
            for x in range(48, w - T - 48, T):
                if fol[y:y + T, x:x + T].mean() > 0.02 or obj[y:y + T, x:x + T].any():
                    continue
                b = li[y:y + T, x:x + T]
                if b.std() < 0.01:
                    continue
                m = mv[y:y + T, x:x + T]
                if np.ptp(m[..., 0]) > 0.3 or np.ptp(m[..., 1]) > 0.3:
                    continue
                dx, dy, pk = shift(prev[y:y + T, x:x + T], b)
                ox, oy, pk2 = shift(prevo[y:y + T, x:x + T], lo[y:y + T, x:x + T])
                if pk > 0.2 and pk2 > 0.2:
                    pic.append((-dx, -dy))
                    out.append((-ox, -oy))
                    vec.append((m[..., 0].mean(), m[..., 1].mean()))
        if len(pic) >= 3:
            dj = np.array(jit[n]) - np.array(jit[n - 1])
            pm = np.median(np.array(pic), axis=0) + dj  # picture move with the jitter's change out (as previous minus current)
            om = np.median(np.array(out), axis=0)
            vm = np.median(np.array(vec), axis=0)
            acc.append(np.concatenate([pm - vm, om - vm]))
            print(f'{n - 1:2d}-{n:2d}: tiles {len(pic):3d}  vectors {vm[0]:+.3f} {vm[1]:+.3f}  picture {pm[0]:+.3f} {pm[1]:+.3f}  '
                  f'DLAA output {om[0]:+.3f} {om[1]:+.3f}')
        else:
            print(f'{n - 1:2d}-{n:2d}: tiles {len(pic)}')
    prev, prevo = li, lo
if acc:
    a = np.array(acc)
    print(f'picture - vectors: mean {a[:, 0].mean():+.3f} {a[:, 1].mean():+.3f}, rms {np.sqrt((a[:, :2] ** 2).mean()):.3f}; '
          f'output - vectors: mean {a[:, 2].mean():+.3f} {a[:, 3].mean():+.3f}, rms {np.sqrt((a[:, 2:] ** 2).mean()):.3f}')
