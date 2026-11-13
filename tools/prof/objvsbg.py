import os, sys
import numpy as np
# objvsbg.py <dir> <tag> x0 x1 y0 y1: in each frame the final vectors (px) on the exact object pixels inside the box vs the
# background ring around it (camera motion only)
folder, tag = sys.argv[1], sys.argv[2]
x0, x1, y0, y1 = map(int, sys.argv[3:7])

def read_f16(path, ch):
    with open(path, 'rb') as f:
        w, h = np.frombuffer(f.read(8), dtype=np.uint32)
        d = np.frombuffer(f.read(), dtype=np.float16)
    return d[: w * h * ch].reshape(h, w, ch).astype(np.float32)

for n in range(16):
    m = os.path.join(folder, f'{tag}_mv{n:02d}_mvec.f16')
    p = os.path.join(folder, f'{tag}_mv{n:02d}_objmv.f16')
    if not os.path.exists(m):
        break
    mv = read_f16(m, 2)
    h, w = mv.shape[:2]
    px = mv * np.array([w, h], np.float32)
    o = read_f16(p, 4) if os.path.exists(p) else np.zeros((h, w, 4), np.float32)
    box = np.zeros((h, w), bool)
    box[y0:y1 + 1, x0:x1 + 1] = True
    ring = np.zeros((h, w), bool)
    ring[max(0, y0 - 12):y1 + 13, max(0, x0 - 12):x1 + 13] = True
    obj = box & (o[..., 3] > 0.75)
    bg = ring & ~box & (o[..., 3] == 0)
    a = px[obj].mean(axis=0) if obj.any() else (0, 0)
    b = px[bg].mean(axis=0) if bg.any() else (0, 0)
    print(f'{n:02d} object {obj.sum():4d} px mv ({a[0]:7.2f},{a[1]:7.2f}) | background {bg.sum():4d} px mv ({b[0]:7.2f},{b[1]:7.2f}) | difference ({a[0] - b[0]:6.2f},{a[1] - b[1]:6.2f})')
