# Where a CryMT RenderDoc capture's draws go: counted by their colour / depth targets (shadow maps, z-prepass, scene,
# reflections, post) and by pass marker when the capture has them (CRYMT_MARKERS=1). No replay per draw (each action
# lists its targets), so it takes seconds.
#   set RDC_FILE=<capture.rdc> & set RDC_OUT=<report.txt> & qrenderdoc.exe --python tools\prof\rdc_drawsplit.py
import os
import traceback
from collections import Counter, OrderedDict

import renderdoc as rd

path = os.environ.get('RDC_FILE', '')
out = open(os.environ.get('RDC_OUT') or path + '.drawsplit.txt', 'w', encoding='utf-8')


def say(text=''):
    out.write(text + '\n')
    out.flush()


try:
    try:
        rd.InitialiseReplay(rd.GlobalEnvironment(), [])
    except Exception:
        pass
    cap = rd.OpenCaptureFile()
    cap.OpenFile(path, '', None)
    result, ctrl = cap.OpenCapture(rd.ReplayOptions(), None)
    sdf = ctrl.GetStructuredFile()
    F = rd.ActionFlags
    tex = {t.resourceId: t for t in ctrl.GetTextures()}
    by_target = OrderedDict()
    by_marker = Counter()
    kinds = Counter()

    def name_of(rid):
        t = tex.get(rid)
        if rid == rd.ResourceId.Null() or t is None:
            return '-'
        return '%dx%d %s%s' % (t.width, t.height, t.format.Name(), ' cube' if t.cubemap else '')

    def walk(a, stack):
        for c in a.children:
            walk(c, stack + ([a.GetName(sdf)] if a.flags & F.PushMarker else []))
        if a.flags & F.Drawcall:
            kinds['draws'] += 1
            if a.flags & F.Instanced:
                kinds['instanced draws'] += 1
            colour = a.outputs[0] if len(a.outputs) else rd.ResourceId.Null()
            key = (name_of(colour), name_of(a.depthOut), str(colour), str(a.depthOut))
            entry = by_target.setdefault(key, [0, a.eventId, a.eventId])
            entry[0] += 1
            entry[2] = a.eventId
            by_marker[stack[0] if stack else '(no marker)'] += 1
        elif a.flags & F.Dispatch:
            kinds['dispatches'] += 1
        elif a.flags & F.Clear:
            kinds['clears'] += 1
        elif a.flags & F.Copy:
            kinds['copies'] += 1

    for r in ctrl.GetRootActions():
        walk(r, [])
    say('capture %s' % path)
    say(', '.join('%s %d' % kv for kv in kinds.items()))
    say('')
    say('draws by colour target / depth target (events first..last):')
    for (c, d, cid, did), (n, first, last) in sorted(by_target.items(), key=lambda kv: -kv[1][0]):
        say('  %6d  colour %-34s depth %-30s ev %d..%d  [%s / %s]' % (n, c, d, first, last, cid, did))
    if len(by_marker) > 1 or '(no marker)' not in by_marker:
        say('')
        say('draws by top pass marker:')
        for m, n in by_marker.most_common():
            say('  %6d  %s' % (n, m))
except Exception:
    say(traceback.format_exc())
out.close()
os._exit(0)
