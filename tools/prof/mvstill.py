import os, sys
import numpy as np
sys.path.insert(0, r'D:\Games\Crysis-RE\tools\prof')
from dlaa_seq import read_f16
from mv_check import read_pgm
# mvstill.py <dir> <tag>...: on a (nearly) still camera, the vectors on static pixels (no foliage, no moving objects;
# the lower half: the near ground) fitted as zoom + shift + depth-dependent part: per frame the zoom at the edge (px),
# and the vectors' frame-to-frame change (px rms) - noise there is what makes DLSS's history slide under textures.
folder = sys.argv[1]
for tag in sys.argv[2:]:
    fields = []
    for n in range(40):
        p = os.path.join(folder, f'{tag}_mv{n:02d}_mvec.f16')
        if not os.path.exists(p):
            break
        mv = read_f16(p, 2)
        h, w = mv.shape[:2]
        px = mv * np.array([w, h], np.float32)
        fp = os.path.join(folder, f'{tag}_mv{n:02d}_foliage.pgm')
        fol = (read_pgm(fp) > 0) if os.path.exists(fp) else np.zeros((h, w), bool)
        op = os.path.join(folder, f'{tag}_mv{n:02d}_objmv.f16')
        obj = (read_f16(op, 4)[..., 3] > 0.25) if os.path.exists(op) else np.zeros((h, w), bool)
        keep = ~fol & ~obj
        keep[: h // 2] = False
        keep[:, w // 2:] = False  # the weapon is on the right
        fields.append((px, keep))
    zooms, changes = [], []
    for i, (px, keep) in enumerate(fields):
        h, w = keep.shape
        ys, xs = np.nonzero(keep[::4, ::4])
        ys, xs = ys * 4, xs * 4
        v = px[ys, xs]
        rx, ry = xs - w / 2, ys - h / 2
        A = np.concatenate([np.stack([rx, np.ones_like(rx), np.zeros_like(rx)], 1), np.stack([ry, np.zeros_like(ry), np.ones_like(ry)], 1)]).astype(np.float64)
        (s, tx, ty), *_ = np.linalg.lstsq(A, np.concatenate([v[:, 0], v[:, 1]]).astype(np.float64), rcond=None)
        zooms.append(s * w / 2)
        if i:
            pv, pk = fields[i - 1]
            k = keep & pk
            changes.append(np.sqrt(((px[k] - pv[k]) ** 2).sum(axis=1).mean()))
    print(f'{tag}: frames {len(fields)}; vectors on the near ground: zoom at the edge px mean {np.mean(zooms):+.4f} (|max| {np.max(np.abs(zooms)):.4f}); '
          f'change frame to frame rms {np.mean(changes):.4f} px; |vector| mean {np.mean([np.hypot(*f[0][f[1]].T).mean() for f in fields]):.4f} px')
