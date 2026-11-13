import os, sys
import numpy as np
from PIL import Image
# dlaa_look.py <dir> <tag> <out dir>: the DLSS input / output of a capture-key shot: vectors' statistics, a per-tile
# shift of the output against the input (phase correlation: where the output's picture sits relative to this frame's),
# a sharpness ratio, and downscaled pictures.
folder, tag, out = sys.argv[1], sys.argv[2], sys.argv[3]
os.makedirs(out, exist_ok=True)

def read_f16(path, ch):
    with open(path, 'rb') as f:
        w, h = np.frombuffer(f.read(8), dtype=np.uint32)
        d = np.frombuffer(f.read(), dtype=np.float16)
    return d[: w * h * ch].reshape(h, w, ch).astype(np.float32)

def ppm(path):
    return np.asarray(Image.open(path).convert('RGB')).astype(np.float32) / 255

cin = ppm(os.path.join(folder, f'{tag}_next_dlss_in.ppm'))
cout = ppm(os.path.join(folder, f'{tag}_next_dlss_out.ppm'))
mv = read_f16(os.path.join(folder, f'{tag}_next_mvec.f16'), 2)
h, w = cin.shape[:2]
px = mv * np.array([w, h], np.float32)
mag = np.hypot(px[..., 0], px[..., 1])
print(f'{tag}: {w}x{h}; vectors px: mean {mag.mean():.3f}, p50 {np.percentile(mag, 50):.3f}, p99 {np.percentile(mag, 99):.3f}, '
      f'max {mag.max():.2f}; mean x {px[..., 0].mean():.3f} y {px[..., 1].mean():.3f}')

def lum(c):
    return c @ np.array([0.299, 0.587, 0.114], np.float32)

li, lo = lum(cin), lum(cout)

def grad_energy(l):
    gx = np.diff(l, axis=1)[:-1]
    gy = np.diff(l, axis=0)[:, :-1]
    return np.sqrt(gx * gx + gy * gy)

gi_, go = grad_energy(li), grad_energy(lo)
print(f'sharpness (mean gradient) in {gi_.mean():.5f} out {go.mean():.5f} ratio {go.mean() / gi_.mean():.3f}')

def shift(a, b):
    # sub-pixel shift of b against a by phase correlation (parabola around the peak)
    A = np.fft.fft2(a - a.mean())
    B = np.fft.fft2(b - b.mean())
    R = A * np.conj(B)
    R /= np.abs(R) + 1e-9
    r = np.real(np.fft.ifft2(R))
    iy, ix = np.unravel_index(np.argmax(r), r.shape)
    n, m = r.shape
    def sub(c, l, rr):
        d = l - 2 * c + rr
        return 0.0 if d == 0 else 0.5 * (l - rr) / d
    dy = iy + sub(r[iy, ix], r[(iy - 1) % n, ix], r[(iy + 1) % n, ix])
    dx = ix + sub(r[iy, ix], r[iy, (ix - 1) % m], r[iy, (ix + 1) % m])
    if dy > n / 2: dy -= n
    if dx > m / 2: dx -= m
    return dx, dy, r.max()

T = 128
rows = []
for y in range(0, h - T + 1, T):
    for x in range(0, w - T + 1, T):
        a, b = li[y:y + T, x:x + T], lo[y:y + T, x:x + T]
        if a.std() < 0.02:
            continue
        dx, dy, pk = shift(b, a)
        rows.append((x, y, dx, dy, pk, float(mag[y:y + T, x:x + T].mean())))
rows = np.array(rows)
good = rows[rows[:, 4] > 0.2]
print(f'tiles {len(rows)}, with a clear peak {len(good)}: output vs input shift px: mean dx {good[:, 2].mean():.3f} dy {good[:, 3].mean():.3f}, '
      f'|shift| p50 {np.percentile(np.hypot(good[:, 2], good[:, 3]), 50):.3f} p90 {np.percentile(np.hypot(good[:, 2], good[:, 3]), 90):.3f} max {np.hypot(good[:, 2], good[:, 3]).max():.2f}')
big = good[np.hypot(good[:, 2], good[:, 3]) > 0.5]
for r in big[:12]:
    print(f'  tile {int(r[0])},{int(r[1])}: shift {r[2]:.2f},{r[3]:.2f} peak {r[4]:.2f} mean |mv| {r[5]:.2f}')
small = lambda c: Image.fromarray((np.clip(c, 0, 1) * 255).astype(np.uint8)).resize((w // 2, h // 2), Image.LANCZOS)
small(cout).save(os.path.join(out, f'{tag}_out_half.png'))
Image.fromarray((np.clip(mag / 4, 0, 1) * 255).astype(np.uint8)).resize((w // 4, h // 4)).save(os.path.join(out, f'{tag}_mv_mag.png'))
