r"""Checks the orientation of a cube map's six faces (PPM dumps <prefix>_envcube0..5.ppm, D3D order +X -X +Y -Y +Z -Z;
the world Z up, as the engine's cube is looked up by a world direction).
1. Seams: along each face's four edges, the texel just inside against the neighbour face's texel just outside, by D3D
   cube addressing; per face, the mean difference for each of the 8 ways the face could be turned or mirrored (the
   others as they are). A correctly stored face has its smallest error as it is ("id").
2. With a screen capture (optional): screen.ppm yaw pitch fov_v: the screen's top part (sky, land; the HUD and the
   panel left out) against the cube looked up by each pixel's world direction (CryEngine camera: yaw about Z from +Y,
   positive to the left; vertical field of view): luminance correlation per face, for each of the 8 ways.
cube_check.py <prefix> [screen.ppm yaw pitch fov_v]"""
import math
import sys
from PIL import Image

prefix = sys.argv[1]
faces = []
for i in range(6):
    im = Image.open('%s_envcube%d.ppm' % (prefix, i)).convert('RGB')
    faces.append((im.size[0], im.load()))
S = faces[0][0]


def lum(c):
    return 0.2126 * c[0] + 0.7152 * c[1] + 0.0722 * c[2]


def face_dir(f, s, t):
    return [(1, -t, -s), (-1, -t, s), (s, 1, t), (s, -1, -t), (s, -t, 1), (-s, -t, -1)][f]


def lookup(d):
    x, y, z = d
    ax, ay, az = abs(x), abs(y), abs(z)
    if ax >= ay and ax >= az:
        return (0, -z / ax, -y / ax) if x > 0 else (1, z / ax, -y / ax)
    if ay >= az:
        return (2, x / ay, z / ay) if y > 0 else (3, x / ay, -z / ay)
    return (4, x / az, -y / az) if z > 0 else (5, -x / az, -y / az)


OPS = [('id', lambda s, t: (s, t)), ('mirror s', lambda s, t: (-s, t)), ('mirror t', lambda s, t: (s, -t)),
       ('rot 180', lambda s, t: (-s, -t)), ('transpose', lambda s, t: (t, s)), ('rot +90', lambda s, t: (-t, s)),
       ('rot -90', lambda s, t: (t, -s)), ('anti-transpose', lambda s, t: (-t, -s))]


ENGINE = __import__('os').environ.get('CUBE_ENGINE') == '1'  # the engine's raw faces: flipped vertically (CryMT's lookup)


def texel(f, s, t, op=None):
    if op:
        s, t = op(s, t)
    if ENGINE:
        t = -t
    u = min(S - 1, max(0, int((s + 1) / 2 * S)))
    v = min(S - 1, max(0, int((t + 1) / 2 * S)))
    return faces[f][1][u, v]


def sample(d, ops):
    f, s, t = lookup(d)
    return texel(f, s, t, ops.get(f))


def seam_error(f, op):
    ops = {f: op}
    err = n = 0
    h = 1.0 / S
    for k in range(S):
        a = -1 + (2 * k + 1) / S
        for s, t, so, to in ((1 - h, a, 1 + h, a), (-1 + h, a, -1 - h, a), (a, 1 - h, a, 1 + h), (a, -1 + h, a, -1 - h)):
            inside = texel(f, s, t, op)
            outside = sample(face_dir(f, so, to), ops)
            err += abs(lum(inside) - lum(outside))
            n += 1
    return err / n


print('seams: mean luminance difference across each face\'s edges (0..255), by how the face is taken')
names = ['+X', '-X', '+Y', '-Y', '+Z', '-Z']
for f in range(6):
    errs = [(seam_error(f, op), name) for name, op in OPS]
    best = min(errs)
    print('  face %d %s: %s; best %s' % (f, names[f], ' '.join('%s %.1f' % (n, e) for e, n in errs), best[1]))

if len(sys.argv) > 2:
    scr = Image.open(sys.argv[2]).convert('RGB')
    W, H = scr.size
    sp = scr.load()
    yaw, pitch, fov = float(sys.argv[3]), float(sys.argv[4]), float(sys.argv[5])
    fwd = (-math.sin(yaw) * math.cos(pitch), math.cos(yaw) * math.cos(pitch), math.sin(pitch))
    right = (math.cos(yaw), math.sin(yaw), 0.0)
    up = (fwd[1] * right[2] - fwd[2] * right[1], fwd[2] * right[0] - fwd[0] * right[2], fwd[0] * right[1] - fwd[1] * right[0])
    up = tuple(-c for c in up) if up[2] < 0 else up
    th = math.tan(fov / 2)
    pts = []
    for py in range(0, int(H * 0.45), 4):
        for px in range(int(W * 0.30), W, 4):  # the panel and the minimap are on the left
            x = (2 * (px + 0.5) / W - 1) * th * W / H
            y = (1 - 2 * (py + 0.5) / H) * th
            d = tuple(fwd[i] + x * right[i] + y * up[i] for i in range(3))
            pts.append((d, lum(sp[px, py])))
    print('screen: %d points; correlation with the cube by face (rows: the face taken as ...)' % len(pts))
    seen = sorted({lookup(d)[0] for d, _ in pts})
    for f in seen:
        sub = [(d, l) for d, l in pts if lookup(d)[0] == f]
        res = []
        for name, op in OPS:
            a = [lum(sample(d, {f: op})) for d, _ in sub]
            b = [l for _, l in sub]
            ma, mb = sum(a) / len(a), sum(b) / len(b)
            cov = sum((p - ma) * (q - mb) for p, q in zip(a, b))
            va = math.sqrt(sum((p - ma) ** 2 for p in a)) or 1
            vb = math.sqrt(sum((q - mb) ** 2 for q in b)) or 1
            res.append((cov / va / vb, name))
        print('  face %d %s (%d points): %s; best %s' % (f, names[f], len(sub), ' '.join('%s %.2f' % (n, c) for c, n in res), max(res)[1]))
