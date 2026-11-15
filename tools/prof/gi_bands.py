"""The GI's two parts by distance band at one spot (user_spots.ps1 captures): the share of the engine's ambient taken away
(the GI texture's a, its flags removed) and the bounce light added (rgb luminance), per band. With bands.py's frame
ratios it tells whether a band is dark from occlusion or from too little bounce.
  python tools/prof/gi_bands.py <spot> <variant> [<variant> ...]
Files: logs/spot/out_<spot>_<variant>_gi.f16 (8-byte header w, h; RGBA half, full size) and _dist.r32 (GI size)."""
import struct
import sys
import numpy as np

BANDS = [0, 4, 8, 16, 24, 32, 48, 75, 120, 1e9]
LUM = np.array([0.2126, 0.7152, 0.0722], np.float32)

def load_gi(path):
    b = open(path, 'rb').read()
    w, h = np.frombuffer(b[:8], np.uint32)
    return np.frombuffer(b[8:], np.float16).reshape(h, w, 4).astype(np.float32)

def load_dist(path):
    b = open(path, 'rb').read()
    w, h = struct.unpack('<II', b[:8])
    return np.frombuffer(b[8:8 + w * h * 4], np.float32).reshape(h, w)

spot, variants = sys.argv[1], sys.argv[2:]
print(f'spot {spot}: per band, the ambient share taken away (occ) and the bounce added (lum)')
rows = {}
for v in variants:
    g = load_gi(rf'D:\Games\Crysis-RE\logs\spot\out_{spot}_{v}_gi.f16')
    d = load_dist(rf'D:\Games\Crysis-RE\logs\spot\out_{spot}_{v}_dist.r32')
    H, W = g.shape[:2]
    ys = (np.arange(H) * d.shape[0] // H)[:, None]
    xs = (np.arange(W) * d.shape[1] // W)[None, :]
    dd = d[ys, xs]
    occ = np.nan_to_num(np.abs(g[..., 3]))
    occ = np.where(occ > 1, occ - np.floor(occ), occ)  # flags above 1 (thin features etc.): keep the fraction
    bounce = np.nan_to_num(g[..., :3] @ LUM)
    rows[v] = []
    for a, b in zip(BANDS, BANDS[1:]):
        m = (dd >= a) & (dd < b) & (dd > 0)
        rows[v].append((int(m.sum()), float(occ[m].mean()) if m.any() else 0, float(bounce[m].mean()) if m.any() else 0))
print('band m     ' + ''.join(f'{v:>24s}' for v in variants))
for i, (a, b) in enumerate(zip(BANDS, BANDS[1:])):
    name = f'{a}-{b if b < 1e8 else ""}'
    line = f'{name:10s}'
    for v in variants:
        n, o, l = rows[v][i]
        line += f'   occ {o:.3f} lum {l:.4f}' if n else ' ' * 24
    print(line)
