# CryMT's GI geometry pass in a RenderDoc capture (the draws inside "CryMT GI voxels"): for a sample of its draws, the
# vertex shader's output positions after the orthographic replacement: whether they land in the grid (x, y in -1..1,
# depth in 0..1 and not at the first or last slice), split by the pixel shader (plain: no textures; material: textures).
#   set RDC_FILE=<capture.rdc> & set RDC_OUT=<report.txt> & qrenderdoc.exe --python tools\prof\rdc_voxgeom.py
import os
import struct

import renderdoc as rd

path = os.environ.get('RDC_FILE', '')
out_path = os.environ.get('RDC_OUT') or path + '.voxgeom.txt'
lines = []


def finish(code):
    with open(out_path, 'w', encoding='utf-8') as f:
        f.write('\n'.join(lines) + '\n')
    os._exit(code)


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
    draws = []

    def walk(actions, inside):
        for a in actions:
            now = inside or os.environ.get('RDC_MARKER', 'CryMT GI voxels') in a.GetName(sdf)
            if now and (a.flags & rd.ActionFlags.Drawcall):
                draws.append(a.eventId)
            walk(a.children, now)

    walk(ctrl.GetRootActions(), False)
    lines.append('%s: %d draws in "%s"' % (os.path.basename(path), len(draws), os.environ.get('RDC_MARKER', 'CryMT GI voxels')))
    kinds = {'plain': [0, 0, 0, 0, []], 'material': [0, 0, 0, 0, []]}  # draws, vertices, in grid, depth outside, samples
    for eid in draws[::max(1, len(draws) // 400)]:
        ctrl.SetFrameEvent(eid, False)
        state = ctrl.GetPipelineState()
        ps = state.GetShaderReflection(rd.ShaderStage.Pixel)
        kind = 'material' if ps and len(ps.readOnlyResources) > 0 else 'plain'
        k = kinds[kind]
        if k[0] >= 60:
            continue
        mesh = ctrl.GetPostVSData(0, 0, rd.MeshDataStage.VSOut)
        if mesh.vertexResourceId == rd.ResourceId.Null() or mesh.vertexByteStride < 16:
            continue
        n = min(mesh.numIndices, 300)
        data = ctrl.GetBufferData(mesh.vertexResourceId, mesh.vertexByteOffset, n * mesh.vertexByteStride)
        k[0] += 1
        for i in range(n):
            o = i * mesh.vertexByteStride
            if o + 16 > len(data):
                break
            x, y, z, w = struct.unpack_from('<4f', data, o)
            k[1] += 1
            if w != 0 and abs(x / w) <= 1 and abs(y / w) <= 1 and 0 <= z / w <= 1:
                k[2] += 1
            elif w != 0 and not (0 <= z / w <= 1):
                k[3] += 1
            if len(k[4]) < 4 and i == 0:
                k[4].append('eid %d (%.2f %.2f %.3f %.2f)' % (eid, x, y, z, w))
    for name, k in kinds.items():
        lines.append('%s: %d draws, %d vertices, in the grid %d (%.0f%%), depth outside 0..1 %d; first vertices: %s' % (
            name, k[0], k[1], k[2], 100.0 * k[2] / max(k[1], 1), k[3], '; '.join(k[4])))
    finish(0)
except Exception as e:
    lines.append('error: %r' % e)
    finish(1)
