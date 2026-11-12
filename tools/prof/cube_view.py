r"""The cube (dumps <prefix>_envcube0..5.ppm, D3D addressing, world Z up) seen through the screen's camera, beside the
screen capture: a wrongly turned face shows at once. CryEngine camera: yaw about Z from +Y (positive to the left),
pitch up, vertical field of view. Each pixel is labelled by the face it reads (thin coloured edges between faces).
cube_view.py <prefix> <screen.ppm> <yaw> <pitch> <fov_v> <out.png>"""
import math
import sys
from PIL import Image, ImageDraw

prefix, screen, yaw, pitch, fov, out = sys.argv[1], sys.argv[2], float(sys.argv[3]), float(sys.argv[4]), float(sys.argv[5]), sys.argv[6]
faces = [Image.open('%s_envcube%d.ppm' % (prefix, i)).convert('RGB') for i in range(6)]
px = [f.load() for f in faces]
S = faces[0].size[0]
scr = Image.open(screen).convert('RGB')
W, H = 960, int(960 * scr.size[1] / scr.size[0])
scr = scr.resize((W, H), Image.LANCZOS)


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
img = Image.new('RGB', (W, H))
o = img.load()
facemap = [[0] * W for _ in range(H)]
for j in range(H):
    for i in range(W):
        x = (2 * (i + 0.5) / W - 1) * th * W / H
        y = (1 - 2 * (j + 0.5) / H) * th
        d = tuple(fwd[k] + x * right[k] + y * up[k] for k in range(3))
        f, s, t = lookup(d)
        u = min(S - 1, max(0, int((s + 1) / 2 * S)))
        v = min(S - 1, max(0, int((t + 1) / 2 * S)))
        o[i, j] = px[f][u, v]
        facemap[j][i] = f
colours = [(255, 0, 0), (0, 255, 255), (0, 255, 0), (255, 0, 255), (0, 0, 255), (255, 255, 0)]
names = ['+X', '-X', '+Y', '-Y', '+Z', '-Z']
for j in range(1, H):
    for i in range(1, W):
        if facemap[j][i] != facemap[j][i - 1] or facemap[j][i] != facemap[j - 1][i]:
            o[i, j] = colours[facemap[j][i]]
d = ImageDraw.Draw(img)
seen = {}
for j in range(0, H, 8):
    for i in range(0, W, 8):
        seen.setdefault(facemap[j][i], (i, j))
for f, (i, j) in seen.items():
    d.text((i + 4, j + 4), 'cube ' + names[f], fill=colours[f])
canvas = Image.new('RGB', (W, H * 2))
canvas.paste(scr, (0, 0))
canvas.paste(img, (0, H))
ImageDraw.Draw(canvas).text((6, 6), 'screen', fill=(255, 255, 0))
canvas.save(out)
print(out, sorted(names[f] for f in seen))
