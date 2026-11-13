import os, sys
import numpy as np
# objmv_where.py <dir> <tag>: per frame, where CryMT's exact object motion and the engine's motion blur target have pixels
folder, tag = sys.argv[1], sys.argv[2]

def read_f16(path, ch):
    with open(path, 'rb') as f:
        w, h = np.frombuffer(f.read(8), dtype=np.uint32)
        d = np.frombuffer(f.read(), dtype=np.float16)
    return d[: w * h * ch].reshape(h, w, ch).astype(np.float32)

for n in range(16):
    p = os.path.join(folder, f'{tag}_mv{n:02d}_objmv.f16')
    q = os.path.join(folder, f'{tag}_mv{n:02d}_omb.f16')
    m = os.path.join(folder, f'{tag}_mv{n:02d}_mvec.f16')
    if not os.path.exists(m):
        break
    line = f'{n:02d}'
    if os.path.exists(p):
        o = read_f16(p, 4)
        h, w = o.shape[:2]
        for name, sel in (('exact', o[..., 3] > 0.75), ('approx', (o[..., 3] > 0.25) & (o[..., 3] <= 0.75))):
            if sel.any():
                ys, xs = np.nonzero(sel)
                px = np.hypot(o[..., 0] * w, o[..., 1] * h)[sel]
                line += f' | {name} {sel.sum()} px box x {xs.min()}-{xs.max()} y {ys.min()}-{ys.max()} move mean {px.mean():.2f} max {px.max():.2f} dist/far {o[..., 2][sel].mean():.5f}'
            else:
                line += f' | {name} 0'
    else:
        line += ' | no objmv'
    if os.path.exists(q):
        e = read_f16(q, 4)
        sel = e[..., 3] != 0
        line += f' | engine id px {sel.sum()}'
        if sel.any():
            ys, xs = np.nonzero(sel)
            line += f' box x {xs.min()}-{xs.max()} y {ys.min()}-{ys.max()} |v| max {np.abs(e[..., :2][sel]).max():.4f}'
    else:
        line += ' | no omb'
    print(line)
