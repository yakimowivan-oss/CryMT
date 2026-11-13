import os, sys
import numpy as np
sys.path.insert(0, r'D:\Games\Crysis-RE\tools\prof')
from dlaa_seq import read_f16, lum, shift
# outwobble.py <dir> <tag> [x0 x1]: the DLAA output's own move from frame n-1 to n (median over tiles) against what the
# vectors say the picture moved (-mv), and the same for the input with the jitter's change taken out. A still picture
# should give the output's move = the vectors'; the difference is the output's wobble ("textures swimming").
folder, tag = sys.argv[1], sys.argv[2]
info = {}
for line in open(os.path.join(folder, f'{tag}_mvinfo.txt')):
    t = line.split()
    if len(t) < 4 or t[1] != 'jitter':
        continue
    info[int(t[0])] = (float(t[2]), float(t[3]))
prev_o = prev_i = None
rows = []
for n in range(32):
    p = os.path.join(folder, f'{tag}_mv{n:02d}_out.f16')
    if not os.path.exists(p):
        break
    lo = lum(read_f16(p, 4))
    li = lum(read_f16(os.path.join(folder, f'{tag}_mv{n:02d}_in.f16'), 4))
    mv = read_f16(os.path.join(folder, f'{tag}_mv{n:02d}_mvec.f16'), 2)
    h, w = lo.shape
    x0 = int(sys.argv[3]) if len(sys.argv) > 3 else 64
    x1 = int(sys.argv[4]) if len(sys.argv) > 4 else w // 2
    if prev_o is not None:
        T = 128
        so, si, sv = [], [], []
        for y in range(64, h - T - 64, T):
            for x in range(x0, x1 - T, T):
                a, b = prev_o[y:y + T, x:x + T], lo[y:y + T, x:x + T]
                if b.std() < 0.01:
                    continue
                m = mv[y:y + T, x:x + T] * np.array([w, h], np.float32)
                dx, dy, pk = shift(a, b)
                ex, ey, pk2 = shift(prev_i[y:y + T, x:x + T], li[y:y + T, x:x + T])
                if pk > 0.15 and pk2 > 0.15:
                    so.append((dx, dy))
                    si.append((ex, ey))
                    sv.append((-m[..., 0].mean(), -m[..., 1].mean()))
        so, si, sv = np.median(np.array(so), axis=0), np.median(np.array(si), axis=0), np.median(np.array(sv), axis=0)
        jx0, jy0 = info[n - 1]
        jx1, jy1 = info[n]
        si = si - np.array([jx1 - jx0, jy1 - jy0])
        rows.append(np.hypot(*(so - sv)))
        print(f'{n - 1}-{n}: vectors say {sv[0]:+.3f} {sv[1]:+.3f}; input moved {si[0]:+.3f} {si[1]:+.3f}; DLAA output moved {so[0]:+.3f} {so[1]:+.3f}  '
              f'-> output wobble {so[0] - sv[0]:+.3f} {so[1] - sv[1]:+.3f}')
    prev_o, prev_i = lo, li
print(f'{tag}: output wobble |median| mean {np.mean(rows):.3f} px')
