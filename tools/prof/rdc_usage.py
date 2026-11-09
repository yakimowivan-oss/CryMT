# Where shaders are used in a RenderDoc capture: for each resource id in RDC_IDS (numbers, e.g. 46676) the events that
# use it, grouped by the pass they are in (CryMT's pass names), with the pixel shader of the first event of each pass.
#   set RDC_FILE=<capture.rdc> & set RDC_IDS=46676,1201 & set RDC_OUT=<report.txt> & qrenderdoc.exe --python tools\prof\rdc_usage.py
import os

import renderdoc as rd

path = os.environ.get('RDC_FILE', '')
ids = [int(x) for x in os.environ.get('RDC_IDS', '').split(',') if x.strip()]
out_path = os.environ.get('RDC_OUT') or path + '.usage.txt'
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
    where = {}  # event -> pass path

    def walk(actions, stack):
        for a in actions:
            name = a.GetName(sdf)
            if a.children:
                walk(a.children, stack + [name])
            else:
                where[a.eventId] = ' / '.join(stack)

    walk(ctrl.GetRootActions(), [])
    known = {int(str(r.resourceId).split('::')[-1]): r.resourceId for r in ctrl.GetResources()}
    # RDC_AT=<event>: also the shaders bound there.
    at = int(os.environ.get('RDC_AT', '0') or 0)
    if at:
        ctrl.SetFrameEvent(at, True)
        st = ctrl.GetPipelineState()
        for stage in (rd.ShaderStage.Vertex, rd.ShaderStage.Pixel):
            sid = st.GetShader(stage)
            lines.append('event %d %s shader %s' % (at, stage, sid))
            if sid != rd.ResourceId.Null():
                ids.append(int(str(sid).split('::')[-1]))
    for n in ids:
        rid = known.get(n)
        if rid is None:
            lines.append('%d: no such resource' % n)
            continue
        uses = ctrl.GetUsage(rid)
        passes = {}
        for u in uses:
            p = where.get(u.eventId, '?')
            passes.setdefault(p, []).append(u.eventId)
        lines.append('%d: %d uses' % (n, len(uses)))
        for p, evs in passes.items():
            lines.append('  %-60s %5d events, first %d last %d' % (p[:60], len(evs), min(evs), max(evs)))
    finish(0)
except Exception as e:
    lines.append('error: %r' % e)
    finish(1)
