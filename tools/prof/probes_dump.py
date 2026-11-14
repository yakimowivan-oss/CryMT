r"""GI v2's probe atlases from a capture (<tag>_probes.raw, <tag>_probedist.raw; crymt_capture with CRYMT_GI_PROBES=1):
how many probes are traced, their light (rgb) and occlusion (a) over their tiles, the share of 'inside a wall' probes,
the mean distances; then the probes nearest a world point (default: the eye from the probe grid's centre).
  python tools\prof\probes_dump.py <dir or tag prefix path> [x y z]
"""
import glob
import os
import sys

import numpy as np


def load(path):
    raw = open(path, 'rb').read()
    w, h, bpp = np.frombuffer(raw[:12], dtype=np.uint32)
    ch = int(bpp) // 2
    a = np.frombuffer(raw[12:], dtype=np.float16).astype(np.float32).reshape(int(h), int(w), ch)
    return a


def main():
    arg = sys.argv[1]
    if os.path.isdir(arg):
        files = sorted(glob.glob(os.path.join(arg, '*_probes.raw')), key=os.path.getmtime)
        base = files[-1][:-len('_probes.raw')]
    else:
        base = arg
    light = load(base + '_probes.raw')
    dist = load(base + '_probedist.raw')
    print('atlas', light.shape, 'distances', dist.shape, 'from', base)
    # Cascades (2026-11-14): 1024 columns each, 0.5 / 1 / 2 m apart; CASCADE=n picks one.
    cascade = int(os.environ.get('CASCADE', '0')) if light.shape[1] > 1024 else 0
    spacing = 0.5 * 2 ** cascade if light.shape[1] > 1024 else 1.0  # 0.5, 1, 2 m
    light = light[:, 1024 * cascade:1024 * (cascade + 1), :]
    if dist.shape[1] > 1024:
        dist = dist[:, 1024 * cascade:1024 * (cascade + 1), :]
    print('cascade %d, %.1f m apart' % (cascade, spacing))
    states = light[1024:1152, :128, :]  # slot = x + 128 y
    n = 32 * 32 * 16
    st = states.reshape(-1, 4)[:n]
    valid = st[:, 3] >= 1
    inside = st[:, 3] - 1
    print('probes traced %d of %d; inside share mean %.3f, over 0.6: %d' % (valid.sum(), n, inside[valid].mean() if valid.any() else 0,
                                                                         (inside[valid] > 0.6).sum()))
    tiles = light[:1024, :1024, :].reshape(128, 8, 128, 8, 4).transpose(0, 2, 1, 3, 4).reshape(128 * 128, 8, 8, 4)[:n]
    inner = tiles[:, 1:7, 1:7, :].reshape(n, 36, 4)
    mean = inner.mean(axis=1)
    v = mean[valid]
    print('light rgb mean %.4f %.4f %.4f (max %.3f), occlusion mean %.3f (min %.3f max %.3f)' % (
        v[:, 0].mean(), v[:, 1].mean(), v[:, 2].mean(), v[:, :3].max(), v[:, 3].mean(), v[:, 3].min(), v[:, 3].max()))
    t = 16 if dist.shape[0] == 2048 else 8  # distance tiles: 16 x 16 before 2026-11-14 ~04:00, then as the light's
    dt = dist[:128 * t, :128 * t, :].reshape(128, t, 128, t, 2).transpose(0, 2, 1, 3, 4).reshape(128 * 128, t, t, 2)[:n]
    dmean = dt[:, 1:t - 1, 1:t - 1, 0].reshape(n, -1).mean(axis=1)
    print('distance mean over the sphere: %.3f m (min %.3f max %.3f)' % (dmean[valid].mean(), dmean[valid].min(), dmean[valid].max()))
    # world coordinate per slot from the states (mod 1024) and the probes nearest a point
    key = st[:, :3]
    if len(sys.argv) >= 5:
        p = np.array([float(x) for x in sys.argv[2:5]])
    else:
        p = (np.median(key[valid], axis=0) + 0.5) * spacing
    pos = (key + 0.5) * spacing  # world mod 1024 x spacing
    d2 = ((pos - (p % (1024 * spacing))) ** 2).sum(axis=1)
    d2[~valid] = 1e9
    count = int(sys.argv[5]) if len(sys.argv) >= 6 else 12
    # 'up': the four centre texels of the 6x6 interior (octahedral +Z), 'down': its four corners (-Z)
    up = inner.reshape(n, 6, 6, 4)[:, 2:4, 2:4, :].reshape(n, 4, 4).mean(axis=1)
    corners = inner.reshape(n, 6, 6, 4)[:, [0, 0, 5, 5], [0, 5, 0, 5], :]
    down = corners.mean(axis=1)
    c0 = t // 2 - 1
    dup = dt[:, c0:c0 + 2, c0:c0 + 2, 0].reshape(n, -1).mean(axis=1)  # the distance tile's centre: +Z
    ddown = dt[:, [1, 1, t - 2, t - 2], [1, t - 2, 1, t - 2], 0].mean(axis=1)
    print('nearest probes to %.1f %.1f %.1f (mod 1024):' % tuple(p))
    for i in np.argsort(d2)[:count]:
        print('  slot %5d at %6.1f %6.1f %6.1f  inside %.2f  light %.3f  up %.3f/occ %.2f/%.2fm  down %.3f/occ %.2f/%.2fm  occ %.3f' % (
            i, *pos[i], inside[i], mean[i, :3].mean(), up[i, :3].mean(), up[i, 3], dup[i], down[i, :3].mean(), down[i, 3], ddown[i], mean[i, 3]))


if __name__ == '__main__':
    main()
