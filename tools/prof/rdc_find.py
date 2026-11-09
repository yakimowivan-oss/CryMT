# Finds the draws of a RenderDoc capture whose shaders use the named constants or resources (the game's parameter and
# sampler names, e.g. WaterLevel), with the pass they are in (CryMT's pass names), their place among the pass's draws,
# the render targets' formats and whether they write depth.
# Runs inside RenderDoc's own Python:
#   set RDC_FILE=<capture.rdc> & set RDC_FIND=WaterLevel,WaterParams & set RDC_OUT=<report.txt> &
#   qrenderdoc.exe --python tools\prof\rdc_find.py
import os

import renderdoc as rd

path = os.environ.get('RDC_FILE', '')
names = [n.strip() for n in os.environ.get('RDC_FIND', 'WaterLevel').split(',') if n.strip()]
out_path = os.environ.get('RDC_OUT') or path + '.find.txt'
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
    F = rd.ActionFlags
    draws = []  # (eid, marker path, index in the pass, action name)

    def walk(actions, stack, counter):
        for a in actions:
            if a.flags & F.PushMarker or a.children:
                name = a.GetName(sdf)
                walk(a.children, stack + [name], {'n': 0})
                continue
            if a.flags & F.Drawcall:
                counter['n'] += 1
                draws.append((a.eventId, ' / '.join(stack), counter['n'], a.GetName(sdf)))

    walk(ctrl.GetRootActions(), [], {'n': 0})
    per_pass = {}
    for d in draws:
        per_pass[d[1]] = max(per_pass.get(d[1], 0), d[2])
    lines.append('%s: %d draws; looking for %s' % (os.path.basename(path), len(draws), ', '.join(names)))
    found = 0
    for eid, where, index, action in draws:
        ctrl.SetFrameEvent(eid, False)
        state = ctrl.GetPipelineState()
        hits = []
        for stage in (rd.ShaderStage.Vertex, rd.ShaderStage.Pixel):
            refl = state.GetShaderReflection(stage)
            if not refl:
                continue
            used = set()
            for block in refl.constantBlocks:
                for var in block.variables:
                    used.add(var.name)
            for res in list(refl.readOnlyResources) + list(refl.samplers):
                used.add(res.name)
            for n in names:
                if n in used:
                    hits.append('%s:%s' % ('VS' if stage == rd.ShaderStage.Vertex else 'PS', n))
        if not hits:
            continue
        found += 1
        targets = [str(t.format.Name()) for t in state.GetOutputTargets() if t.resource != rd.ResourceId.Null()]
        depth = state.GetDepthTarget()
        d12 = ctrl.GetD3D12PipelineState()
        writes = d12.outputMerger.depthStencilState.depthWrites if d12 else None
        lines.append('eid %d  draw %d of %d in [%s]  %s  targets %s  depth %s writes %s  (%s)' % (
            eid, index, per_pass.get(where, 0), where, ' '.join(hits), targets,
            depth.resource != rd.ResourceId.Null(), writes, action))
    lines.append('found %d' % found)
    finish(0)
except Exception as e:
    lines.append('error: %r' % e)
    finish(1)
