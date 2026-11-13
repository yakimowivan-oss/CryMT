import sys
import numpy as np
# precision.py <mvinfo>: RunMotion's CPU reprojection (RigidInverse, MatMul in float32, as the C++ does) against the same in
# float64, for a still camera (the same view twice) and for the recorded pairs: the motion vector error (px at 5120x1440,
# vertical 1247 px/rad) of a surface 1.5 m and 10 m ahead; also with the camera moved to the user's place (2166, 2328, 227).
mats = {}
for line in open(sys.argv[1]):
    t = line.split()
    if len(t) == 18:
        mats.setdefault(int(t[0]), {})[t[1]] = np.array(list(map(float, t[2:])), np.float32)

def rigid_inverse(m, dt):
    m = m.astype(dt)
    out = np.zeros(16, dt)
    for r in range(3):
        for c in range(3):
            out[r * 4 + c] = m[c * 4 + r]
    for c in range(3):
        out[12 + c] = -(m[12] * m[c * 4] + m[13] * m[c * 4 + 1] + m[14] * m[c * 4 + 2])
    out[15] = dt(1)
    return out

def matmul(a, b, dt):
    a, b = a.astype(dt), b.astype(dt)
    out = np.zeros(16, dt)
    for r in range(4):
        for c in range(4):
            out[r * 4 + c] = a[r * 4] * b[c] + a[r * 4 + 1] * b[4 + c] + a[r * 4 + 2] * b[8 + c] + a[r * 4 + 3] * b[12 + c]
    return out

def mv_px(view, prev_view, proj, dist, dt):
    to_world = rigid_inverse(view, dt)
    reproj = matmul(matmul(to_world, prev_view, dt), proj, dt).astype(np.float32).reshape(4, 4)
    # points straight ahead and at the corners' directions, at the given distance (view space, x, y, -distance)
    pts = []
    for ndc in ((0, 0), (0.5, -0.5), (-0.5, -0.5), (0.8, 0.3)):
        pts.append((ndc[0] / proj[0] * dist, ndc[1] / proj[5] * dist, -dist, 1.0))
    P = np.array(pts, np.float32)
    clip = P @ reproj
    prev_ndc = clip[:, :2] / clip[:, 3:4]
    now_ndc = np.array([(p[0] * proj[0] / dist, p[1] * proj[5] / dist) for p in pts], np.float32)
    return (prev_ndc - now_ndc) * np.array([2560, 720], np.float32)  # ndc -> px at 5120x1440

def with_camera_at(view, pos):
    v = view.astype(np.float64).copy()
    R = v.reshape(4, 4)[:3, :3]
    t = -np.array(pos, np.float64) @ R
    v[12:15] = t
    return v.astype(np.float32)

n0 = min(mats)
m = mats[n0]
proj = m['proj'].astype(np.float32)
for label, view in (('recorded place', m['view']), ("the user's place", with_camera_at(m['view'], (2166.3513, 2328.2822, 227.3869)))):
    for dist in (1.5, 10.0):
        e32 = mv_px(view, view, proj, dist, np.float32)
        print(f'{label}, still camera, {dist:4.1f} m: vector error px (should be 0): {np.round(e32, 3).tolist()}')
diffs = []
for n in sorted(mats):
    if n - 1 in mats:
        a = mv_px(mats[n]['view'], mats[n - 1]['view'], proj, 1.5, np.float32)
        b = mv_px(mats[n]['view'], mats[n - 1]['view'], proj, 1.5, np.float64)
        diffs.append(np.abs(a - b).max())
print('recorded pairs, 1.5 m, |float32 - float64| max px:', np.round(diffs, 3).tolist())
