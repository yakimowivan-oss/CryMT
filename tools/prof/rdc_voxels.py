# CryMT's voxel grid in a RenderDoc capture: after the last "CryMT GI voxels" pass, every 3D texture of 128^3 cells is
# read back and summarised: the voxels (RGBA16F: opacity a), the geometry stamps (R32_UINT: frame * 256 + opacity).
# Per (x, y) column of a grid the lowest opaque cell is taken as the ground (the grid is toroidal; z is up), and the
# report says how many of those have a geometry stamp (the z-prepass draws voxelized them) or only the screen's colour.
#   set RDC_FILE=<capture.rdc> & set RDC_OUT=<report.txt> & qrenderdoc.exe --python tools\prof\rdc_voxels.py
import array
import os
import struct

import renderdoc as rd

path = os.environ.get('RDC_FILE', '')
out_path = os.environ.get('RDC_OUT') or path + '.voxels.txt'
lines = []


def finish(code):
    with open(out_path, 'w', encoding='utf-8') as f:
        f.write('\n'.join(lines) + '\n')
    os._exit(code)


def half(h):
    return struct.unpack('<e', struct.pack('<H', h))[0]


try:
    try:
        rd.InitialiseReplay(rd.GlobalEnvironment(), [])
    except Exception:
        pass
    cap = rd.OpenCaptureFile()
    if cap.OpenFile(path, '', None) != rd.ResultCode.Succeeded:
        lines.append('cannot open ' + path)
        finish(1)
    result, ctrl = cap.OpenCapture(rd.ReplayOptions(), None)
    if result != rd.ResultCode.Succeeded:
        lines.append('replay failed: ' + str(result))
        finish(1)
    sdf = ctrl.GetStructuredFile()
    last = [0]

    def walk(actions, inside):
        for a in actions:
            name = a.GetName(sdf)
            now = inside or name == 'CryMT GI voxels'
            if now and a.eventId > last[0]:
                last[0] = a.eventId
            walk(a.children, now)

    walk(ctrl.GetRootActions(), False)
    if not last[0]:
        lines.append('no "CryMT GI voxels" pass')
        finish(1)
    # The voxel update dispatches in the pass (one per level): their root constants (kVoxShader VoxConstants: toWorldX.w,
    # Y.w, Z.w at 35, 39, 43 = the camera; voxelSize at 50; gridMin at 52..55).
    updates = []

    def dispatches(actions, inside):
        for a in actions:
            name = a.GetName(sdf)
            now = inside or name == 'CryMT GI voxels'
            if now and (a.flags & rd.ActionFlags.Dispatch):
                updates.append(a.eventId)
            dispatches(a.children, now)

    dispatches(ctrl.GetRootActions(), False)
    levels = {}
    for eid in updates:
        ctrl.SetFrameEvent(eid, False)
        d12 = ctrl.GetD3D12PipelineState()
        consts = None
        params = d12.rootSignature.parameters
        for j in range(len(params)):
            raw = bytes(params[j].constants)
            if len(raw) >= 240:
                consts = struct.unpack('<60f', raw[:240])
                break
        if consts and consts[50] > 0 and consts[55] == 128:
            vs = consts[50]
            if vs not in levels:
                levels[vs] = (consts[35], consts[39], consts[43], consts[52], consts[53], consts[54])
    lines.append('update dispatches %d; levels by cell size: %s' % (len(updates), ', '.join('%.2f m: eye (%.1f %.1f %.1f) first (%d %d %d)' % (
        k, v[0], v[1], v[2], v[3], v[4], v[5]) for k, v in sorted(levels.items()))))
    ctrl.SetFrameEvent(last[0], True)
    lines.append('%s: after event %d' % (os.path.basename(path), last[0]))
    N = 128
    grids = []
    for t in ctrl.GetTextures():
        if t.width == N and t.height == N and t.depth == N:
            grids.append(t)
    lines.append('3D textures of 128^3: %d' % len(grids))
    voxels, stamps = [], []
    for t in grids:
        fmt = t.format.Name()
        sub = rd.Subresource(0, 0, 0)
        data = ctrl.GetTextureData(t.resourceId, sub)
        lines.append('  %s %s %d bytes' % (t.resourceId, fmt, len(data)))
        if 'R16G16B16A16' in fmt and len(data) == N * N * N * 8:
            a = array.array('H')
            a.frombytes(bytes(data))
            voxels.append((t.resourceId, a))
        elif 'R32_UINT' in fmt and len(data) == N * N * N * 4:
            a = array.array('I')
            a.frombytes(bytes(data))
            stamps.append((t.resourceId, a))
    # Per voxel grid: opaque cells (a >= 0.5: half-float bits 0x3800..0x7BFF) and their tops (the cell above is open:
    # ground, roofs, the upper side of things); with the grid's stamps (levels 0-2 in order), how many of those tops the
    # z-prepass voxelization stamped (else only the screen gave them).
    NN = N * N
    for sid, st in stamps:
        nz = [x for x in st if x]
        frames = sorted(set(x >> 8 for x in nz))
        lines.append('stamps %s: non-zero %d, frames %s .. %s (%d distinct), opacities %s' % (
            sid, len(nz), frames[0] if frames else '-', frames[-1] if frames else '-', len(frames), sorted(set(x & 255 for x in nz))[:8]))
    for i, (vid, v) in enumerate(voxels):
        st = stamps[i][1] if i < len(stamps) else None
        opq = [0x3800 <= h < 0x7C00 for h in v[3::4]]
        total = sum(opq)
        tops = stamped = 0
        for k in range(N * NN):
            if opq[k]:
                up = k + NN if k < (N - 1) * NN else k - (N - 1) * NN
                if not opq[up]:
                    tops += 1
                    if st is not None and st[k] >> 8:
                        stamped += 1
        lines.append('grid %d (%s): opaque cells %d, tops %d, tops with a geometry stamp %s' % (
            i, vid, total, tops, ('%d (%.0f%%)' % (stamped, 100.0 * stamped / max(tops, 1))) if st is not None else 'n/a'))
    # Level i has cells of the smallest size x 2^i, its first cell floor(eye / size) - N / 2 (as RunVoxelUpdate), the eye
    # from any update in the pass.
    if not levels:
        lines.append('no update constants')
        finish(1)
    base = min(levels)
    ex, ey, ez = levels[base][0], levels[base][1], levels[base][2]
    import math
    for i, (vid, v) in enumerate(voxels):
        vs = base * (1 << i)
        fx, fy, fz = (math.floor(c / vs) - N // 2 for c in (ex, ey, ez))
        ground = ez - 1.8
        rings = [(0, 8), (8, 16), (16, 24), (24, 32), (32, 48), (48, 64)]
        stats = []
        for r0, r1 in rings:
            cols = found = 0
            for gx in range(int(fx), int(fx) + N):
                for gy in range(int(fy), int(fy) + N):
                    cx, cy = (gx + 0.5) * vs, (gy + 0.5) * vs
                    dist = ((cx - ex) ** 2 + (cy - ey) ** 2) ** 0.5
                    if not (r0 <= dist < r1):
                        continue
                    cols += 1
                    z0, z1 = int((ground - 4) / vs), int((ground + 4) / vs)
                    for gz in range(z0, z1 + 1):
                        if gz < fz or gz >= fz + N:
                            continue
                        k = (((gz % N) * N + (gy % N)) * N + (gx % N))
                        if 0x3800 <= v[k * 4 + 3] < 0x7C00:
                            found += 1
                            break
            if cols:
                stats.append('%d-%d m %.0f%%' % (r0, r1, 100.0 * found / cols))
        lines.append('level %d (%.2f m): columns with ground near the camera height: %s' % (i, vs, ', '.join(stats)))
    finish(0)
except Exception as e:
    lines.append('error: %r' % e)
    finish(1)
