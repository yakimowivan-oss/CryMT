r"""How each face of a cube map dump (<prefix>_envcube0..5.ppm, D3D order) is stored against the world, from screen
captures taken at the cube's place: every screen pixel's world direction (CryEngine camera: yaw about Z from +Y,
positive to the left; pitch up; vertical field of view) is looked up in the cube by D3D addressing with each face taken
in one of 8 ways (turned / mirrored in its own image); per face, the luminance correlation with the screen for each way.
The HUD corners, the panel and the weapon are left out.
cube_vs_screens.py <prefix> <fov_v> <screen.ppm>:<yaw>:<pitch> [...]"""
import math
import sys
from PIL import Image

prefix, fov = sys.argv[1], float(sys.argv[2])
faces = [Image.open('%s_envcube%d.ppm' % (prefix, i)).convert('L') for i in range(6)]
px = [f.load() for f in faces]
S = faces[0].size[0]
OPS = [('id', lambda s, t: (s, t)), ('mirror s', lambda s, t: (-s, t)), ('mirror t', lambda s, t: (s, -t)),
       ('rot 180', lambda s, t: (-s, -t)), ('transpose', lambda s, t: (t, s)), ('rot +90', lambda s, t: (-t, s)),
       ('rot -90', lambda s, t: (t, -s)), ('anti-transpose', lambda s, t: (-t, -s))]
names = ['+X', '-X', '+Y', '-Y', '+Z', '-Z']


def lookup(d):
    x, y, z = d
    ax, ay, az = abs(x), abs(y), abs(z)
    if ax >= ay and ax >= az:
        return (0, -z / ax, -y / ax) if x > 0 else (1, z / ax, -y / ax)
    if ay >= az:
        return (2, x / ay, z / ay) if y > 0 else (3, x / ay, -z / ay)
    return (4, x / az, -y / az) if z > 0 else (5, -x / az, -y / az)


samples = {f: [] for f in range(6)}  # (s, t, screen luminance)
ENGINE = __import__('os').environ.get('CUBE_ENGINE') == '1'  # look up as CryMT does for the engine's cube
for arg in sys.argv[3:]:
    path, yaw, pitch = arg.rsplit(':', 2)
    yaw, pitch = float(yaw), float(pitch)
    scr = Image.open(path).convert('L')
    W, H = 320, int(320 * scr.size[1] / scr.size[0])
    scr = scr.resize((W, H), Image.LANCZOS)
    sp = scr.load()
    fwd = (-math.sin(yaw) * math.cos(pitch), math.cos(yaw) * math.cos(pitch), math.sin(pitch))
    right = (math.cos(yaw), math.sin(yaw), 0.0)
    up = (right[1] * fwd[2] - right[2] * fwd[1], right[2] * fwd[0] - right[0] * fwd[2], right[0] * fwd[1] - right[1] * fwd[0])
    th = math.tan(fov / 2)
    for j in range(H):
        for i in range(W):
            u, v = i / W, j / H
            if (u < 0.3 and v > 0.35) or (u > 0.42 and v > 0.5) or (u > 0.45 and v < 0.13):
                continue
            x = (2 * (i + 0.5) / W - 1) * th * W / H
            y = (1 - 2 * (j + 0.5) / H) * th
            d = tuple(fwd[k] + x * right[k] + y * up[k] for k in range(3))
            f, s, t = lookup(d)
            if ENGINE:  # CryMT's lookup of the engine's cube: the face's t flipped (Water.cfx)
                t = -t
            samples[f].append((s, t, sp[i, j]))


def corr(a, b):
    ma, mb = sum(a) / len(a), sum(b) / len(b)
    va = math.sqrt(sum((p - ma) ** 2 for p in a)) or 1
    vb = math.sqrt(sum((q - mb) ** 2 for q in b)) or 1
    return sum((p - ma) * (q - mb) for p, q in zip(a, b)) / va / vb


for f in range(6):
    pts = samples[f]
    if len(pts) < 500:
        print('face %d %s: %d points, not enough' % (f, names[f], len(pts)))
        continue
    res = []
    for name, op in OPS:
        a = []
        for s, t, _ in pts:
            s2, t2 = op(s, t)
            a.append(px[f][min(S - 1, max(0, int((s2 + 1) / 2 * S))), min(S - 1, max(0, int((t2 + 1) / 2 * S)))])
        res.append((corr(a, [l for _, _, l in pts]), name))
    best = max(res)
    print('face %d %s (%d points): %s; best %s' % (f, names[f], len(pts), ' '.join('%s %.2f' % (n, c) for c, n in res), best[1]))
