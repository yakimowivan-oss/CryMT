import os, sys
import numpy as np
# mv_vs_shift2.py <dir> <tag> [tile 64]: for every pair of a CRYMT_MVSEQ capture, per tile with one depth (the vectors
# vary < 0.5 px in it), the true shift of the input picture from frame n-1 to n (phase correlation; the jitter's change
# taken out using <tag>_mvinfo.txt) against the vectors' mean. Prints per pair the ratio along the move and the error,
# and the error against the vectors' size (near surfaces move more under a translation).
folder, tag = sys.argv[1], sys.argv[2]
T = int(sys.argv[3]) if len(sys.argv) > 3 else 64

def read_f16(path, ch):
    with open(path, 'rb') as f:
        w, h = np.frombuffer(f.read(8), dtype=np.uint32)
        d = np.frombuffer(f.read(), dtype=np.float16)
    return d[: w * h * ch].reshape(h, w, ch).astype(np.float32)

def lum(p):
    c = 1 - np.exp(-np.maximum(read_f16(p, 4)[..., :3], 0))
    return c @ np.array([0.299, 0.587, 0.114], np.float32)

def shift(p, q):
    win = np.outer(np.hanning(p.shape[0]), np.hanning(p.shape[1]))
    A = np.fft.fft2((p - p.mean()) * win)
    B = np.fft.fft2((q - q.mean()) * win)
    R = A * np.conj(B)
    R /= np.abs(R) + 1e-9
    r = np.real(np.fft.ifft2(R))
    iy, ix = np.unravel_index(np.argmax(r), r.shape)
    N, M = r.shape
    def sub(c, l, rr):
        d = l - 2 * c + rr
        return 0.0 if d == 0 else 0.5 * (l - rr) / d
    dy = iy + sub(r[iy, ix], r[(iy - 1) % N, ix], r[(iy + 1) % N, ix])
    dx = ix + sub(r[iy, ix], r[iy, (ix - 1) % M], r[iy, (ix + 1) % M])
    if dy > N / 2: dy -= N
    if dx > M / 2: dx -= M
    return dx, dy, r.max()

jit = {}
p = os.path.join(folder, f'{tag}_mvinfo.txt')
if os.path.exists(p):
    for line in open(p):
        t = line.split()
        if len(t) < 4 or t[1] != 'jitter':
            continue
        jit[int(t[0])] = (float(t[2]), float(t[3]))
allrows = []
for n in range(1, 64):
    if not os.path.exists(os.path.join(folder, f'{tag}_mv{n:02d}_in.f16')):
        break
    a = lum(os.path.join(folder, f'{tag}_mv{n - 1:02d}_in.f16'))
    b = lum(os.path.join(folder, f'{tag}_mv{n:02d}_in.f16'))
    mv = read_f16(os.path.join(folder, f'{tag}_mv{n:02d}_mvec.f16'), 2)
    h, w = a.shape
    mvp = mv * np.array([w, h], np.float32)
    j0, j1 = jit.get(n - 1, (0, 0)), jit.get(n, (0, 0))
    djx, djy = j1[0] - j0[0], j1[1] - j0[1]
    rows = []
    for y in range(0, h - T + 1, T):
        for x in range(0, w - T + 1, T):
            p_, q_ = a[y:y + T, x:x + T], b[y:y + T, x:x + T]
            if q_.std() < 0.02:
                continue
            m = mvp[y:y + T, x:x + T]
            if np.ptp(m[..., 0]) > 0.5 or np.ptp(m[..., 1]) > 0.5:
                continue
            dx, dy, pk = shift(q_, p_)
            if pk < 0.3:
                continue
            rows.append((dx - djx, dy - djy, -m[..., 0].mean(), -m[..., 1].mean()))
    if not rows:
        print(f'pair {n - 1}-{n}: no tiles')
        continue
    r = np.array(rows)
    allrows.append(r)
    err = np.hypot(r[:, 0] - r[:, 2], r[:, 1] - r[:, 3])
    mag = np.hypot(r[:, 2], r[:, 3])
    ratio = np.sum(r[:, 0] * r[:, 2] + r[:, 1] * r[:, 3]) / max(np.sum(r[:, 2] ** 2 + r[:, 3] ** 2), 1e-9)
    print(f'pair {n - 1:2d}-{n:2d}: tiles {len(r):4d}, |vector| mean {mag.mean():6.2f} max {mag.max():6.2f} px, ratio true/vector {ratio:6.3f}, '
          f'|error| p50 {np.percentile(err, 50):.3f} p90 {np.percentile(err, 90):.3f}')
if allrows:
    r = np.concatenate(allrows)
    mag = np.hypot(r[:, 2], r[:, 3])
    err = np.hypot(r[:, 0] - r[:, 2], r[:, 1] - r[:, 3])
    for lo, hi in ((0, 0.5), (0.5, 2), (2, 5), (5, 20), (20, 1e9)):
        s = (mag >= lo) & (mag < hi)
        if s.sum() >= 5:
            rr = r[s]
            ratio = np.sum(rr[:, 0] * rr[:, 2] + rr[:, 1] * rr[:, 3]) / max(np.sum(rr[:, 2] ** 2 + rr[:, 3] ** 2), 1e-9)
            print(f'vectors {lo:g}-{hi:g} px: tiles {s.sum():5d}, ratio true/vector {ratio:.3f}, |error| p50 {np.percentile(err[s], 50):.3f} p90 {np.percentile(err[s], 90):.3f}')
