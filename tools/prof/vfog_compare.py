# Volumetric fog A/B from toggle_frames captures (logs\spot\<tag>_<step>_c<k>.ppm): per step against the base step,
# mean abs difference, share of pixels changed by more than 2 / 8 levels, mean luminance (whole frame and by thirds of
# the height), and the within-step noise (c1 vs c2) as the floor.
#   python tools\prof\vfog_compare.py vf3 off zero on on2
import sys
import numpy as np

def ppm(path):
    with open(path, 'rb') as f:
        data = f.read()
    parts, pos = [], 0
    while len(parts) < 4:
        while data[pos:pos + 1].isspace():
            pos += 1
        if data[pos:pos + 1] == b'#':
            pos = data.index(b'\n', pos) + 1
            continue
        end = pos
        while not data[end:end + 1].isspace():
            end += 1
        parts.append(data[pos:end])
        pos = end
    w, h = int(parts[1]), int(parts[2])
    return np.frombuffer(data[pos + 1:pos + 1 + w * h * 3], np.uint8).reshape(h, w, 3).astype(np.float32)

def lum(img):
    return img @ np.array([0.2126, 0.7152, 0.0722], np.float32)

tag, base, steps = sys.argv[1], sys.argv[2], sys.argv[3:]
root = 'logs/spot/'
b1 = ppm(f'{root}{tag}_{base}_c1.ppm')
b2 = ppm(f'{root}{tag}_{base}_c2.ppm')
h = b1.shape[0]
thirds = [(0, h // 3), (h // 3, 2 * h // 3), (2 * h // 3, h)]

def row(name, a, ref):
    d = np.abs(a - ref)
    la, lr = lum(a), lum(ref)
    parts = ' '.join(f'{la[y0:y1].mean() - lr[y0:y1].mean():+.2f}' for y0, y1 in thirds)
    print(f'{name:>10}: mean|d| {d.mean():.3f}, >2 {100 * (d.max(2) > 2).mean():.2f}%, >8 {100 * (d.max(2) > 8).mean():.2f}%, '
          f'lum {la.mean():.2f} ({la.mean() - lr.mean():+.2f}; thirds top/mid/bottom {parts})')

print(f'{tag}: base {base}, lum {lum(b1).mean():.2f}')
row(f'{base} c2', b2, b1)
for s in steps:
    a = ppm(f'{root}{tag}_{s}_c1.ppm')
    row(s, a, b1)
