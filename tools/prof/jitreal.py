import os, sys
import numpy as np
sys.path.insert(0, r'D:\Games\Crysis-RE\tools\prof')
from dlaa_seq import read_f16, lum, shift
# jitreal.py <dir> <tag>...: how far the jittered INPUT picture really moved between frames beyond the vectors' motion,
# against the change of the jitter told to DLSS. Fit per axis: real = s * told + c. s = 1, c = 0 when the projection's
# jitter is the told one. Only pairs whose vectors move under max_move px (default 1).
folder = sys.argv[1]
pairs = []
for tag in sys.argv[2:]:
    info = {}
    for line in open(os.path.join(folder, f'{tag}_mvinfo.txt')):
        t = line.split()
        if len(t) < 4 or t[1] != 'jitter':
            continue
        info[int(t[0])] = (float(t[2]), float(t[3]))
    prev = None
    for n in range(40):
        p = os.path.join(folder, f'{tag}_mv{n:02d}_in.f16')
        if not os.path.exists(p):
            break
        li = lum(read_f16(p, 4))
        mv = read_f16(os.path.join(folder, f'{tag}_mv{n:02d}_mvec.f16'), 2)
        h, w = li.shape
        if prev is not None:
            T = 128
            s, v = [], []
            for y in range(64, h - T - 64, T):
                for x in range(64, w // 2 - T, T):
                    b = li[y:y + T, x:x + T]
                    if b.std() < 0.01:
                        continue
                    m = mv[y:y + T, x:x + T] * np.array([w, h], np.float32)
                    if np.ptp(m[..., 0]) > 0.3 or np.ptp(m[..., 1]) > 0.3:
                        continue
                    dx, dy, pk = shift(prev[y:y + T, x:x + T], b)
                    if pk > 0.2:
                        s.append((dx, dy))
                        v.append((-m[..., 0].mean(), -m[..., 1].mean()))
            if len(s) >= 5:
                s, v = np.median(np.array(s), axis=0), np.median(np.array(v), axis=0)
                if np.hypot(*v) < 1.0:
                    dj = np.array(info[n]) - np.array(info[n - 1])
                    pairs.append((tag[-30:], n, dj[0], dj[1], s[0] - v[0], s[1] - v[1]))
        prev = li
P = np.array([p[2:] for p in pairs], np.float64)
for p in pairs:
    print(f'{p[0]} {p[1]:2d}: told dj {p[2]:+.3f} {p[3]:+.3f}  real {p[4]:+.3f} {p[5]:+.3f}')
for ax, name in ((0, 'x'), (1, 'y')):
    A = np.stack([P[:, ax], np.ones(len(P))], axis=1)
    (s, c), *_ = np.linalg.lstsq(A, P[:, 2 + ax], rcond=None)
    res = P[:, 2 + ax] - (s * P[:, ax] + c)
    print(f'{name}: real = {s:.3f} x told {c:+.3f}   (residual rms {np.sqrt(np.mean(res ** 2)):.3f} px, {len(P)} pairs)')
