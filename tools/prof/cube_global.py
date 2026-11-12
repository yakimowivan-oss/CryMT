r"""Which global turn or mirror of the cube (48 signed axis permutations G: the cube looked up by G * the world direction)
matches the screen capture: luminance and gradient correlation over the screen's upper part (the panel left out).
cube_global.py <prefix> <screen.ppm> <yaw> <pitch> <fov_v> [top N]"""
import itertools
import math
import sys
from PIL import Image

prefix, screen, yaw, pitch, fov = sys.argv[1], sys.argv[2], float(sys.argv[3]), float(sys.argv[4]), float(sys.argv[5])
top = int(sys.argv[6]) if len(sys.argv) > 6 else 8
faces = [Image.open('%s_envcube%d.ppm' % (prefix, i)).convert('L') for i in range(6)]
px = [f.load() for f in faces]
S = faces[0].size[0]
scr = Image.open(screen).convert('L')
W, H = 240, int(240 * scr.size[1] / scr.size[0])
scr = scr.resize((W, H), Image.LANCZOS)
sp = scr.load()


def lookup(d):
    x, y, z = d
    ax, ay, az = abs(x), abs(y), abs(z)
    if ax >= ay and ax >= az:
        return (0, -z / ax, -y / ax) if x > 0 else (1, z / ax, -y / ax)
    if ay >= az:
        return (2, x / ay, z / ay) if y > 0 else (3, x / ay, -z / ay)
    return (4, x / az, -y / az) if z > 0 else (5, -x / az, -y / az)


fwd = (-math.sin(yaw) * math.cos(pitch), math.cos(yaw) * math.cos(pitch), math.sin(pitch))
right = (math.cos(yaw), math.sin(yaw), 0.0)
up = (right[1] * fwd[2] - right[2] * fwd[1], right[2] * fwd[0] - right[0] * fwd[2], right[0] * fwd[1] - right[1] * fwd[0])
th = math.tan(fov / 2)
rows = int(H * 0.5)
grid = []
for j in range(rows):
    for i in range(W):
        if i < W * 0.28 and H * 0.36 < j:  # the panel
            continue
        x = (2 * (i + 0.5) / W - 1) * th * W / H
        y = (1 - 2 * (j + 0.5) / H) * th
        grid.append((i, j, tuple(fwd[k] + x * right[k] + y * up[k] for k in range(3))))


def corr(a, b):
    ma, mb = sum(a) / len(a), sum(b) / len(b)
    va = math.sqrt(sum((p - ma) ** 2 for p in a)) or 1
    vb = math.sqrt(sum((q - mb) ** 2 for q in b)) or 1
    return sum((p - ma) * (q - mb) for p, q in zip(a, b)) / va / vb


def grad(img):
    g = {}
    for (i, j), v in img.items():
        if (i + 1, j) in img and (i, j + 1) in img:
            g[(i, j)] = abs(img[(i + 1, j)] - v) + abs(img[(i, j + 1)] - v)
    return g


screen_l = {(i, j): sp[i, j] for i, j, _ in grid}
screen_g = grad(screen_l)
results = []
for perm in itertools.permutations(range(3)):
    for signs in itertools.product((1, -1), repeat=3):
        cube_l = {}
        for i, j, d in grid:
            g = tuple(signs[k] * d[perm[k]] for k in range(3))
            f, s, t = lookup(g)
            cube_l[(i, j)] = px[f][min(S - 1, max(0, int((s + 1) / 2 * S))), min(S - 1, max(0, int((t + 1) / 2 * S)))]
        cube_g = grad(cube_l)
        keys = list(screen_g)
        cl = corr([screen_l[k] for k in screen_l], [cube_l[k] for k in screen_l])
        cg = corr([screen_g[k] for k in keys], [cube_g[k] for k in keys])
        name = '(%s)' % ', '.join(('-' if signs[k] < 0 else '+') + 'xyz'[perm[k]] for k in range(3))
        results.append((cg, cl, name))
results.sort(reverse=True)
print('G * (x, y, z) -> looked up as; gradient corr, luminance corr (best first)')
for cg, cl, name in results[:top]:
    print('  %-16s grad %.3f lum %.3f' % (name, cg, cl))
ident = [r for r in results if r[2] == '(+x, +y, +z)'][0]
print('  identity: grad %.3f lum %.3f, rank %d of 48' % (ident[0], ident[1], results.index(ident) + 1))
