"""DLAA smear check on a CRYMT_MVSEQ=1 capture with the jitter on (crymt_capture N; the camera turning, then still):
per frame, how the DLAA output sits against its own input and how sharp it is, so smearing ("textures flowing after
the camera moves") shows as numbers.

    python tools\\prof\\dlaa_seq.py <capture dir> <tag> [x0 y0 x1 y1 region] [out dir]

Files: <tag>_mvNN_in.f16 / _mvNN_out.f16 (RGBA16F, DLSS's input and output), _mvNN_mvec.f16, <tag>_mvinfo.txt (jitter
per frame). Per frame (region, default the left half without the edges):
  move    mean |motion vector| px (the camera's turn);
  sharp   gradient energy of the output / of the input (the input is aliased, so ~0.8-1.0 when fine; lower = blur);
  shift   the output's picture against the input's, per 128 px tile, phase correlation, minus the jitter (the input is
          the jittered picture, the output the unjittered one): p50 / p90 of |residual| px (0 = in place; a smear or a lag
          behind the camera shows as a residual along the move);
  lag     the residual's mean along the move direction px (+: the output trails behind the motion).
Writes <out>/<tag>_dlaa_NN.png crops (input above, output below) for the first, the last moving and the last frames."""
import os
import sys

import numpy as np
from PIL import Image


def read_f16(path, ch):
    with open(path, 'rb') as f:
        w, h = np.frombuffer(f.read(8), dtype=np.uint32)
        d = np.frombuffer(f.read(), dtype=np.float16)
    return d[: w * h * ch].reshape(h, w, ch).astype(np.float32)


def tone(c):
    return 1 - np.exp(-np.maximum(c[..., :3], 0))


def lum(c):
    return tone(c) @ np.array([0.299, 0.587, 0.114], np.float32)


def shift(a, b):
    """How far b's picture is moved against a's (sub-pixel, phase correlation with a window)."""
    win = np.outer(np.hanning(a.shape[0]), np.hanning(a.shape[1]))
    A = np.fft.fft2((a - a.mean()) * win)
    B = np.fft.fft2((b - b.mean()) * win)
    R = B * np.conj(A)
    R /= np.abs(R) + 1e-9
    r = np.real(np.fft.ifft2(R))
    iy, ix = np.unravel_index(np.argmax(r), r.shape)
    n, m = r.shape

    def sub(c, l, rr):
        d = l - 2 * c + rr
        return 0.0 if d == 0 else 0.5 * (l - rr) / d

    dy = iy + sub(r[iy, ix], r[(iy - 1) % n, ix], r[(iy + 1) % n, ix])
    dx = ix + sub(r[iy, ix], r[iy, (ix - 1) % m], r[iy, (ix + 1) % m])
    if dy > n / 2:
        dy -= n
    if dx > m / 2:
        dx -= m
    return dx, dy, r.max()


def grad(l):
    gx = np.diff(l, axis=1)[:-1]
    gy = np.diff(l, axis=0)[:, :-1]
    return np.sqrt(gx * gx + gy * gy).mean()


def main():
    folder, tag = sys.argv[1], sys.argv[2]
    region = list(map(int, sys.argv[3:7])) if len(sys.argv) >= 7 else None
    out = sys.argv[7] if len(sys.argv) >= 8 else folder
    info = {}
    p = os.path.join(folder, f'{tag}_mvinfo.txt')
    if os.path.exists(p):
        for line in open(p):
            t = line.split()
            info[int(t[0])] = (float(t[2]), float(t[3]), int(t[5]), float(t[7]), float(t[9]))
    frames = []
    for n in range(100):
        if not os.path.exists(os.path.join(folder, f'{tag}_mv{n:02d}_out.f16')):
            break
        frames.append(n)
    print(f'{tag}: {len(frames)} frames')
    print(' n  jitter x    y   reset proj8      move px  sharp  shift p50  p90   lag px')
    last_moving = None
    for n in frames:
        cin = read_f16(os.path.join(folder, f'{tag}_mv{n:02d}_in.f16'), 4)
        cout = read_f16(os.path.join(folder, f'{tag}_mv{n:02d}_out.f16'), 4)
        mv = read_f16(os.path.join(folder, f'{tag}_mv{n:02d}_mvec.f16'), 2)
        h, w = cin.shape[:2]
        x0, y0, x1, y1 = region if region else (64, 64, w // 2, h - 64)
        li, lo = lum(cin)[y0:y1, x0:x1], lum(cout)[y0:y1, x0:x1]
        m = mv[y0:y1, x0:x1] * np.array([w, h], np.float32)
        move = float(np.hypot(m[..., 0], m[..., 1]).mean())
        mdir = -m.reshape(-1, 2).mean(axis=0)  # the picture moves by -mv
        mlen = np.hypot(*mdir)
        jx, jy, reset, p8, _ = info.get(n, (0.0, 0.0, 0, 0.0, 0.0))
        res = []
        T = 128
        for y in range(0, li.shape[0] - T + 1, T):
            for x in range(0, li.shape[1] - T + 1, T):
                a, b = li[y:y + T, x:x + T], lo[y:y + T, x:x + T]
                if a.std() < 0.01:
                    continue
                dx, dy, pk = shift(a, b)  # the output against the input
                if pk < 0.15:
                    continue
                # the input is moved by +jitter against the unjittered picture, so the output sits at -jitter from it
                res.append((dx + jx, dy + jy))
        res = np.array(res) if res else np.zeros((1, 2))
        mag = np.hypot(res[:, 0], res[:, 1])
        lag = float((-(res @ mdir) / mlen).mean()) if mlen > 0.05 else 0.0
        sharp = grad(lo) / max(grad(li), 1e-9)
        if move > 0.3:
            last_moving = n
        print(f'{n:2d} {jx:+.3f} {jy:+.3f}  {reset}  {p8:+.6f}  {move:7.2f}  {sharp:5.3f}  {np.percentile(mag, 50):6.3f} {np.percentile(mag, 90):6.3f}  {lag:+6.3f}')
    os.makedirs(out, exist_ok=True)
    for n in sorted({frames[0], last_moving if last_moving is not None else frames[0], frames[-1]}):
        cin = read_f16(os.path.join(folder, f'{tag}_mv{n:02d}_in.f16'), 4)
        cout = read_f16(os.path.join(folder, f'{tag}_mv{n:02d}_out.f16'), 4)
        h, w = cin.shape[:2]
        x0, y0, x1, y1 = region if region else (64, 64, w // 2, h - 64)
        cw, ch = min(512, x1 - x0), min(256, y1 - y0)
        cx, cy = x0 + (x1 - x0 - cw) // 2, y0 + (y1 - y0 - ch) // 2
        a = (tone(cin)[cy:cy + ch, cx:cx + cw] * 255).astype(np.uint8)
        b = (tone(cout)[cy:cy + ch, cx:cx + cw] * 255).astype(np.uint8)
        Image.fromarray(np.concatenate([a, b], axis=0)).resize((cw * 2, ch * 4), Image.NEAREST).save(os.path.join(out, f'{tag}_dlaa_{n:02d}.png'))
    print('crops in', out)


if __name__ == '__main__':
    main()
