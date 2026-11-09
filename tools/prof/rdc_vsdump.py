# One draw of CryMT's GI geometry pass with the plain pixel shader (no textures): its vertex shader's disassembly (the
# lines writing o0, the position) and its constant buffers' values, to see which matrix the position comes from.
#   set RDC_FILE=<capture.rdc> & set RDC_OUT=<report.txt> & qrenderdoc.exe --python tools\prof\rdc_vsdump.py
import os
import struct

import renderdoc as rd

path = os.environ.get('RDC_FILE', '')
out_path = os.environ.get('RDC_OUT') or path + '.vsdump.txt'
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
    cap.OpenFile(path, '', None)
    result, ctrl = cap.OpenCapture(rd.ReplayOptions(), None)
    sdf = ctrl.GetStructuredFile()
    draws = []

    marker = os.environ.get('RDC_MARKER', 'CryMT GI voxels')  # draws inside the first pass whose name has this

    def walk(actions, inside):
        for a in actions:
            now = inside or (marker in a.GetName(sdf) and not draws)
            if now and (a.flags & rd.ActionFlags.Drawcall):
                draws.append(a.eventId)
            walk(a.children, now)

    walk(ctrl.GetRootActions(), False)
    done = 0
    want = int(os.environ.get('RDC_EID', '0') or 0)  # RDC_EID: that draw only
    order = draws if os.environ.get('RDC_FORWARD') else list(reversed(draws))  # in CryMT GI voxels the plain draws come last
    for eid in ([want] if want else order):
        ctrl.SetFrameEvent(eid, False)
        state = ctrl.GetPipelineState()
        ps = state.GetShaderReflection(rd.ShaderStage.Pixel)
        if not want and (not ps or len(ps.readOnlyResources) > 0):
            continue
        skipped = globals().get('skipped', 0)
        if not want and skipped < int(os.environ.get('RDC_SKIP', '0') or 0):  # RDC_SKIP: plain draws to pass over first
            globals()['skipped'] = skipped + 1
            continue
        vs = state.GetShaderReflection(rd.ShaderStage.Vertex)
        pipe = state.GetGraphicsPipelineObject()
        targets = ctrl.GetDisassemblyTargets(True)
        text = ctrl.DisassembleShader(pipe, vs, targets[0])
        lines.append('==== eid %d, vertex shader %s' % (eid, vs.resourceId))
        body = [l for l in text.split('\n') if l.strip() and not l.strip().startswith('//')]
        lines.extend(body[:60])
        for i, block in enumerate(vs.constantBlocks):
            bind = state.GetConstantBlock(rd.ShaderStage.Vertex, i, 0)
            desc = bind.descriptor
            data = ctrl.GetBufferData(desc.resource, desc.byteOffset, min(desc.byteSize, 1024)) if desc.resource != rd.ResourceId.Null() else b''
            floats = struct.unpack_from('<%df' % (len(data) // 4), data) if data else ()
            lines.append('cb %d (%s, reg %d): %d bytes' % (i, block.name, block.fixedBindNumber, len(data)))
            for r in range(0, min(len(floats), 64), 4):
                lines.append('  c%-3d %s' % (r // 4, ' '.join('%10.4f' % x for x in floats[r:r + 4])))
        done += 1
        if done >= 2:
            break
    finish(0)
except Exception as e:
    lines.append('error: %r' % e)
    finish(1)
