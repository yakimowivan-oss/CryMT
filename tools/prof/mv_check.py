"""Motion vector check on a CRYMT_MVSEQ=1 capture (crymt_capture N or the capture key): for each pair of consecutive
frames the previous scene colour (DLSS's input, before tone mapping) is moved by the current frame's vectors and compared
with the current one, in 1 - exp(-x) (the picture's scale). Pixels that changed between the frames (|now - before| over
the threshold) and stay wrong after the move have motion the vectors miss (or light that changed: shadows, particles,
water, GI noise; a still camera with nothing moving gives the floor).

    python tools\\prof\\mv_check.py <capture dir> <tag> [threshold 0.03] [out dir]

Files: <tag>_mvNN_in.f16 (RGBA16F), _mvNN_mvec.f16 (RG16F: previous minus current, screen fractions), _mvNN_omb.f16 (the
engine's object motion blur target, RGBA16F; missing when it drew nothing), _mvNN_objmv.f16 (CryMT's exact object
motion from the patched pass: xy before minus now, z distance / far, w 1 exact / 0.5 shutter factor in), _mvNN_foliage.pgm
(the z-prepass foliage mask). Classes: foliage (mask), objects (the motion blur target holds a velocity or an id there, or
the exact target was written), rest; "exact" lines: the exact target's pixels and their mean move.
Writes <out>/<tag>_mverr.png for the last pair: grey the current frame, red where the vectors miss (wrong after the move),
green where they fixed a change, blue outline the motion blur objects."""
import os
import sys

import numpy as np
from PIL import Image


def read_f16(path, channels):
    with open(path, 'rb') as f:
        w, h = np.frombuffer(f.read(8), dtype=np.uint32)
        data = np.frombuffer(f.read(), dtype=np.float16)
    return data[: w * h * channels].reshape(h, w, channels).astype(np.float32)


def read_pgm(path):
    with open(path, 'rb') as f:
        magic = f.readline().strip()
        line = f.readline()
        while line.startswith(b'#'):
            line = f.readline()
        w, h = map(int, line.split())
        f.readline()
        return np.frombuffer(f.read(), dtype=np.uint8)[: w * h].reshape(h, w)


def bilinear(img, x, y):
    h, w = img.shape[:2]
    x = np.clip(x, 0, w - 1.001)
    y = np.clip(y, 0, h - 1.001)
    x0 = np.floor(x).astype(np.int32)
    y0 = np.floor(y).astype(np.int32)
    fx = (x - x0)[..., None]
    fy = (y - y0)[..., None]
    a = img[y0, x0]
    b = img[y0, x0 + 1]
    c = img[y0 + 1, x0]
    d = img[y0 + 1, x0 + 1]
    return (a * (1 - fx) + b * fx) * (1 - fy) + (c * (1 - fx) + d * fx) * fy


def main():
    folder, tag = sys.argv[1], sys.argv[2]
    thr = float(sys.argv[3]) if len(sys.argv) > 3 else 0.03
    out = sys.argv[4] if len(sys.argv) > 4 else folder
    frames = []
    for n in range(64):
        p = os.path.join(folder, f'{tag}_mv{n:02d}_in.f16')
        if not os.path.exists(p):
            break
        frames.append(n)
    if len(frames) < 2:
        print('fewer than 2 frames for', tag)
        return
    print(f'{tag}: {len(frames)} frames, threshold {thr}')
    print('pair  class      pixels%   changed%  missed%  fixed%   mean err  mean |mv| px')
    totals = {}
    last = None
    for n in frames[1:]:
        prev = 1 - np.exp(-np.maximum(read_f16(os.path.join(folder, f'{tag}_mv{n - 1:02d}_in.f16'), 4)[..., :3], 0))
        cur = 1 - np.exp(-np.maximum(read_f16(os.path.join(folder, f'{tag}_mv{n:02d}_in.f16'), 4)[..., :3], 0))
        mv = read_f16(os.path.join(folder, f'{tag}_mv{n:02d}_mvec.f16'), 2)
        h, w = cur.shape[:2]
        ys, xs = np.mgrid[0:h, 0:w].astype(np.float32)
        warped = bilinear(prev, xs + mv[..., 0] * w, ys + mv[..., 1] * h)
        err = np.abs(cur - warped).mean(axis=2)
        err0 = np.abs(cur - prev).mean(axis=2)
        fol_path = os.path.join(folder, f'{tag}_mv{n:02d}_foliage.pgm')
        foliage = read_pgm(fol_path) > 0 if os.path.exists(fol_path) else np.zeros((h, w), bool)
        if foliage.shape != (h, w):
            foliage = np.zeros((h, w), bool)
        omb_path = os.path.join(folder, f'{tag}_mv{n:02d}_omb.f16')
        objects = np.zeros((h, w), bool)
        if os.path.exists(omb_path):
            omb = read_f16(omb_path, 4)
            if omb.shape[:2] == (h, w):
                objects = (np.abs(omb[..., 0]) + np.abs(omb[..., 1]) > 1e-6) | (omb[..., 3] != 0)
        objmv_path = os.path.join(folder, f'{tag}_mv{n:02d}_objmv.f16')
        if os.path.exists(objmv_path):
            om = read_f16(objmv_path, 4)
            if om.shape[:2] == (h, w):
                ex = om[..., 3] > 0.75
                ap = (om[..., 3] > 0.25) & ~ex
                objects = objects | ex | ap
                mpx = np.hypot(om[..., 0] * w, om[..., 1] * h)
                print(f'{n - 1:02d}-{n:02d} exact     {100 * ex.sum() / (h * w):8.2f}% exact, {100 * ap.sum() / (h * w):.2f}% approx; '
                      f'mean |move| exact {float(mpx[ex].mean()) if ex.any() else 0:.3f} px, max {float(mpx[ex].max()) if ex.any() else 0:.2f} px')
        classes = {'all': np.ones((h, w), bool), 'foliage': foliage & ~objects, 'objects': objects,
                   'rest': ~foliage & ~objects}
        mvpx = np.hypot(mv[..., 0] * w, mv[..., 1] * h)
        for name, m in classes.items():
            cnt = int(m.sum())
            if cnt == 0:
                print(f'{n - 1:02d}-{n:02d} {name:9s}  0')
                continue
            changed = (err0 > thr) & m
            missed = (err > thr) & m
            fixed = changed & (err <= thr)
            row = (cnt, changed.sum(), missed.sum(), fixed.sum(), float(err[m].mean()), float(mvpx[m].mean()))
            t = totals.setdefault(name, [0, 0, 0, 0, 0.0, 0.0, 0])
            for i in range(4):
                t[i] += row[i]
            t[4] += row[4] * cnt
            t[5] += row[5] * cnt
            t[6] += 1
            print(f'{n - 1:02d}-{n:02d} {name:9s} {100 * cnt / (h * w):8.2f} {100 * row[1] / cnt:9.3f} {100 * row[2] / cnt:8.3f} '
                  f'{100 * row[3] / cnt:7.3f} {row[4]:10.5f} {row[5]:10.3f}')
        last = (cur, err, err0, objects)
    print('total')
    for name, t in totals.items():
        cnt = t[0]
        print(f'      {name:9s} {"":8s} {100 * t[1] / cnt:9.3f} {100 * t[2] / cnt:8.3f} {100 * t[3] / cnt:7.3f} '
              f'{t[4] / cnt:10.5f} {t[5] / cnt:10.3f}')
    cur, err, err0, objects = last
    grey = (cur.mean(axis=2) * 0.6 * 255).astype(np.uint8)
    img = np.stack([grey, grey, grey], axis=2)
    img[(err0 > thr) & (err <= thr)] = (0, 200, 0)
    img[err > thr] = (255, 0, 0)
    edge = objects & ~np.roll(objects, 1, axis=0) | objects & ~np.roll(objects, 1, axis=1)
    img[edge] = (0, 128, 255)
    os.makedirs(out, exist_ok=True)
    Image.fromarray(img).save(os.path.join(out, f'{tag}_mverr.png'))
    print('picture', os.path.join(out, f'{tag}_mverr.png'))


if __name__ == '__main__':
    main()
