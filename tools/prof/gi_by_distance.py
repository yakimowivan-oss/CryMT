r"""The GI by distance from a key capture (crymt_capture 3 / PageUp): <tag>_gi.f16 (full size, RGBA halves: rgb the bounce,
a the share of the ambient removed) and <tag>_dist.r32 (the GI's distances at its own size, metres). Per distance bin:
pixels, mean occlusion (a), mean bounce luminance; the voxel levels' half extents (16/32/64/128 m) are marked.
  python tools\prof\gi_by_distance.py <dir> <tag> [bin metres]
"""
import os
import struct
import sys

import numpy as np


def read_raw(path, dtype, channels):
    with open(path, 'rb') as f:
        w, h = struct.unpack('<II', f.read(8))
        data = np.frombuffer(f.read(), dtype=dtype)
    return data[:w * h * channels].reshape(h, w, channels) if channels > 1 else data[:w * h].reshape(h, w)


def main():
    folder, tag = sys.argv[1], sys.argv[2]
    step = float(sys.argv[3]) if len(sys.argv) > 3 else 4.0
    gi = read_raw(os.path.join(folder, tag + '_gi.f16'), np.float16, 4).astype(np.float32)
    dist = read_raw(os.path.join(folder, tag + '_dist.r32'), np.float32, 1)
    sy, sx = gi.shape[0] / dist.shape[0], gi.shape[1] / dist.shape[1]
    ys = np.minimum((np.arange(dist.shape[0]) * sy + sy / 2).astype(int), gi.shape[0] - 1)
    xs = np.minimum((np.arange(dist.shape[1]) * sx + sx / 2).astype(int), gi.shape[1] - 1)
    g = gi[ys][:, xs]
    a = g[..., 3]
    flags = np.floor(a * 0.5)
    occ = a - 2 * flags  # a = the share of the ambient removed + 2 x flags (kGiShader apply)
    bounce = 0.2126 * g[..., 0] + 0.7152 * g[..., 1] + 0.0722 * g[..., 2]
    valid = (dist > 0.5) & (dist < 1000) & np.isfinite(occ)
    print('%s: GI %dx%d, distances %dx%d, valid %d; flags seen %s' % (tag, gi.shape[1], gi.shape[0], dist.shape[1], dist.shape[0],
                                                                     valid.sum(), sorted(set(np.unique(flags[valid]).astype(int).tolist()))[:8]))
    print('  metres      pixels  occlusion (p10 p50 p90)  bounce')
    marks = {16, 32, 64, 128}
    lo = 0.0
    while lo < 130:
        hi = lo + step
        m = valid & (dist >= lo) & (dist < hi)
        if m.sum() >= 50:
            mark = ' <- level edge' if any(lo < e <= hi for e in marks) else ''
            p10, p50, p90 = np.percentile(occ[m], [10, 50, 90])
            print('  %5.1f-%-5.1f %7d   %6.3f (%.2f %.2f %.2f)   %7.4f%s' % (lo, hi, m.sum(), occ[m].mean(), p10, p50, p90,
                                                                      bounce[m].mean(), mark))
        lo = hi


if __name__ == '__main__':
    main()
