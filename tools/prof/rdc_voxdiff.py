# Two RenderDoc captures of CryMT, level 0 of the voxel grid compared cell by cell in the world (RDC_FILE then RDC_FILE2):
# the cells opaque in the second but not the first ("appeared") and the other way ("went"), by where their opacity
# comes from: a geometry stamp (solid 255, LOD dissolve 254, foliage 127) or the screen only (no stamp); with samples
# of the appeared ones relative to the camera (metres, z up).
#   set RDC_FILE=<a.rdc> & set RDC_FILE2=<b.rdc> & set RDC_OUT=<report.txt> & qrenderdoc.exe --python tools\prof\rdc_voxdiff.py
import array
import math
import os
import struct

import renderdoc as rd

out_path = os.environ.get('RDC_OUT') or 'voxdiff.txt'
lines = []
N = 128


def finish(code):
    with open(out_path, 'w', encoding='utf-8') as f:
        f.write('\n'.join(lines) + '\n')
    os._exit(code)


def load(path):
    cap = rd.OpenCaptureFile()
    if cap.OpenFile(path, '', None) != rd.ResultCode.Succeeded:
        raise RuntimeError('cannot open ' + path)
    result, ctrl = cap.OpenCapture(rd.ReplayOptions(), None)
    if result != rd.ResultCode.Succeeded:
        raise RuntimeError('replay failed ' + str(result))
    sdf = ctrl.GetStructuredFile()
    last = [0]
    updates = []

    def walk(actions, inside):
        for a in actions:
            now = inside or a.GetName(sdf) == 'CryMT GI voxels'
            if now and a.eventId > last[0]:
                last[0] = a.eventId
            if now and (a.flags & rd.ActionFlags.Dispatch):
                updates.append(a.eventId)
            walk(a.children, now)

    walk(ctrl.GetRootActions(), False)
    eye = None
    for eid in updates:
        ctrl.SetFrameEvent(eid, False)
        params = ctrl.GetD3D12PipelineState().rootSignature.parameters
        for j in range(len(params)):
            raw = bytes(params[j].constants)
            if len(raw) >= 240:
                c = struct.unpack('<60f', raw[:240])
                if c[55] == 128 and c[50] > 0:
                    eye = (c[35], c[39], c[43], c[50])
                    break
        if eye:
            break
    ctrl.SetFrameEvent(last[0], True)
    vox = st = None
    for t in ctrl.GetTextures():
        if t.width == N and t.height == N and t.depth == N:
            fmt = t.format.Name()
            if vox is None and 'R16G16B16A16' in fmt:
                vox = array.array('H')
                vox.frombytes(bytes(ctrl.GetTextureData(t.resourceId, rd.Subresource(0, 0, 0))))
            elif st is None and 'R32_UINT' in fmt:
                st = array.array('I')
                st.frombytes(bytes(ctrl.GetTextureData(t.resourceId, rd.Subresource(0, 0, 0))))
    ctrl.Shutdown()
    cap.Shutdown()
    return eye, vox, st


try:
    try:
        rd.InitialiseReplay(rd.GlobalEnvironment(), [])
    except Exception:
        pass
    a_path, b_path = os.environ['RDC_FILE'], os.environ['RDC_FILE2']
    ea, va, sa = load(a_path)
    eb, vb, sb = load(b_path)
    vs = ea[3]  # level 0's cell size (the smallest)
    lines.append('%s -> %s: eye (%.2f %.2f %.2f) -> (%.2f %.2f %.2f), cell %.2f m' % (
        os.path.basename(a_path), os.path.basename(b_path), ea[0], ea[1], ea[2], eb[0], eb[1], eb[2], vs))
    fa = [math.floor(c / vs) - N // 2 for c in ea[:3]]
    fb = [math.floor(c / vs) - N // 2 for c in eb[:3]]
    lo = [max(fa[i], fb[i]) for i in range(3)]
    hi = [min(fa[i], fb[i]) + N for i in range(3)]
    newest_b = max((x >> 8) for x in sb) if sb else 0

    def opaque(v, k):
        h = v[k * 4 + 3]
        return 0x3800 <= h < 0x7C00

    def kind(s, k, newest):
        x = s[k] if s else 0
        if not x:
            return 'screen only'
        o = x & 255
        age = newest - (x >> 8)
        name = {255: 'solid', 254: 'dissolve', 127: 'foliage'}.get(o, 'opacity %d' % o)
        return '%s stamp, %s' % (name, 'recent' if age < 30 else 'old (%d+ frames)' % 30)

    appeared, went, same = {}, {}, 0
    samples = []
    for gz in range(lo[2], hi[2]):
        for gy in range(lo[1], hi[1]):
            for gx in range(lo[0], hi[0]):
                k = ((gz % N) * N + (gy % N)) * N + (gx % N)
                oa, ob = opaque(va, k), opaque(vb, k)
                if oa and ob:
                    same += 1
                elif ob:
                    c = kind(sb, k, newest_b)
                    appeared[c] = appeared.get(c, 0) + 1
                    if len(samples) < 24:
                        samples.append('(%+.1f %+.1f %+.1f) %s' % ((gx + 0.5) * vs - eb[0], (gy + 0.5) * vs - eb[1], (gz + 0.5) * vs - eb[2], c))
                elif oa:
                    c = kind(sa, k, newest_b)
                    went[c] = went.get(c, 0) + 1
    lines.append('opaque in both: %d' % same)
    lines.append('appeared: %d  %s' % (sum(appeared.values()), ', '.join('%s %d' % kv for kv in sorted(appeared.items(), key=lambda kv: -kv[1]))))
    lines.append('went: %d  %s' % (sum(went.values()), ', '.join('%s %d' % kv for kv in sorted(went.items(), key=lambda kv: -kv[1]))))
    lines.append('appeared, samples (from the camera, m):')
    lines.extend('  ' + s for s in samples)
    finish(0)
except Exception as e:
    lines.append('error: %r' % e)
    finish(1)
