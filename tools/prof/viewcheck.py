import os, sys
import numpy as np
sys.path.insert(0, r'D:\Games\Crysis-RE\tools\prof')
from dlaa_seq import read_f16, lum, shift
# viewcheck.py <dir> <tag>: which view matrix explains the input picture's move between frames. For each pair the
# measured move (per-tile phase correlation, jitter change out, fitted as shift + zoom about the centre) and the move of
# far points predicted from: CryMT's view (info.view, captured at SetCamera), the renderer's +0x50 at the hand-over
# (viewLate), viewLate against the engine's own previous (+0xD0), and the modelview stack top at the hand-over.
folder, tag = sys.argv[1], sys.argv[2]
jit, mats = {}, {}
for line in open(os.path.join(folder, f'{tag}_mvinfo.txt')):
    t = line.split()
    if len(t) > 3 and t[1] == 'jitter':
        jit[int(t[0])] = (float(t[2]), float(t[3]))
    elif len(t) == 18:
        mats.setdefault(int(t[0]), {})[t[1]] = np.array(list(map(float, t[2:])), np.float64).reshape(4, 4)

def predict(v0, v1, proj, w, h):
    """Far points (direction only) seen at the grid in frame 1, where they were in frame 0 (px), as shift + zoom."""
    ys, xs = np.mgrid[0:h:32, 0:w:32].astype(np.float64)
    ndc_x = (xs + 0.5) / w * 2 - 1
    ndc_y = 1 - (ys + 0.5) / h * 2
    # view-space directions (right-handed, looking down -z): x = ndc_x / p00, y = ndc_y / p11, z = -1
    d = np.stack([ndc_x / proj[0, 0], ndc_y / proj[1, 1], -np.ones_like(ndc_x), np.zeros_like(ndc_x)], -1)
    # row vectors: world = view_dir * inverse(v1) (rotation only: translation drops with w = 0)
    world = d @ np.linalg.inv(v1)
    v0d = world @ v0
    px0 = (v0d[..., 0] * proj[0, 0] / -v0d[..., 2] + 1) / 2 * w - 0.5
    py0 = (1 - v0d[..., 1] * proj[1, 1] / -v0d[..., 2]) / 2 * h - 0.5
    mvx, mvy = px0 - xs, py0 - ys  # previous minus current
    return fit(xs.ravel() - w / 2, ys.ravel() - h / 2, mvx.ravel(), mvy.ravel())

def fit(rx, ry, mx, my):
    A = np.concatenate([np.stack([rx, np.ones_like(rx), np.zeros_like(rx)], 1), np.stack([ry, np.zeros_like(ry), np.ones_like(ry)], 1)])
    (s, tx, ty), *_ = np.linalg.lstsq(A, np.concatenate([mx, my]), rcond=None)
    return s, tx, ty

prev = None
for n in range(40):
    p = os.path.join(folder, f'{tag}_mv{n:02d}_in.f16')
    if not os.path.exists(p):
        break
    li = lum(read_f16(p, 4))
    h, w = li.shape
    if prev is not None and n in mats and n - 1 in mats:
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
        dj = np.array(jit[n]) - np.array(jit[n - 1])
        meas = fit(R[:, 0], R[:, 1], -(R[:, 2] - dj[0]), -(R[:, 3] - dj[1]))
        m0, m1 = mats[n - 1], mats[n]
        proj = m1['proj']
        out = [('picture', meas), ('CryMT view', predict(m0['view'], m1['view'], proj, w, h)),
               ('renderer +0x50', predict(m0['viewLate'], m1['viewLate'], proj, w, h)),
               ('+0x50 vs +0xD0', predict(m1['viewPrevLate'], m1['viewLate'], proj, w, h)),
               ('stack top', predict(m0['viewStackLate'], m1['viewStackLate'], proj, w, h))]
        print(f'{n - 1}-{n}: ' + '  |  '.join(f'{k}: {s * w / 2:+.3f} ({tx:+.3f},{ty:+.3f})' for k, (s, tx, ty) in out))
    prev = li
print('(each: zoom at the edge px, (shift x, shift y) px; previous minus current)')
m = mats[min(mats)]
for k in ('view', 'viewLate', 'viewPrevLate', 'viewStackLate'):
    print(k, np.round(m[k], 4).tolist())
