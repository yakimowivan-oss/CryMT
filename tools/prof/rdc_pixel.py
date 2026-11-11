# Pixel history of one pixel in the scene colour target of a CryMT RenderDoc capture: every event that touched it, with its
# pass, whether the tests passed, and the colour (and depth) after it. For "which draw made this pixel wrong" between a good
# and a bad capture of the same timedemo frame.
# Runs inside RenderDoc's Python:
#   set RDC_FILE=<capture.rdc> & set RDC_X=1550 & set RDC_Y=700 & set RDC_OUT=<report.txt>
#   [set RDC_MARKER=<pass name part whose first draw's colour target is used; default "GENERAL after water">]
#   qrenderdoc.exe --python tools\prof\rdc_pixel.py
import os
import traceback

import renderdoc as rd

path = os.environ.get('RDC_FILE', '')
out_path = os.environ.get('RDC_OUT') or path + '.pixel.txt'
px, py = int(os.environ.get('RDC_X', '0')), int(os.environ.get('RDC_Y', '0'))
marker = os.environ.get('RDC_MARKER', 'GENERAL after water')
lines = []


def say(text=''):
    lines.append(text)


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
        say('cannot open ' + path)
        finish(1)
    result, ctrl = cap.OpenCapture(rd.ReplayOptions(), None)
    if result != rd.ResultCode.Succeeded:
        say('replay failed: ' + str(result))
        finish(1)
    sdf = ctrl.GetStructuredFile()
    F = rd.ActionFlags

    # event id -> pass name, and the draws inside passes whose name contains `marker`
    pass_of = {}
    action_of = {}
    candidates = []

    def walk(a, stack):
        if a.flags & F.PushMarker:
            for c in a.children:
                walk(c, stack + [a.GetName(sdf)])
            return
        for c in a.children:
            walk(c, stack)
        name = ' / '.join(stack) if stack else '(no pass name)'
        pass_of[a.eventId] = name
        action_of[a.eventId] = a
        if (a.flags & F.Drawcall) and (marker == '*' or marker in name):
            candidates.append(a.eventId)

    for r in ctrl.GetRootActions():
        walk(r, [])
    targets = []
    first = [None]
    textures = {t.resourceId: t for t in ctrl.GetTextures()}
    if os.environ.get('RDC_LIST'):  # every colour target the draws use: format, size, events, draws (every 5th draw)
        seen = {}
        for ev in candidates[::5]:
            ctrl.SetFrameEvent(ev, True)
            for t in ctrl.GetPipelineState().GetOutputTargets():
                if t.resource == rd.ResourceId.Null():
                    continue
                s = seen.setdefault(t.resource, [ev, ev, 0, pass_of.get(ev, '?')])
                s[1] = ev
                s[2] += 1
        for rid, (a, b, n, p) in sorted(seen.items(), key=lambda kv: kv[1][0]):
            d = textures[rid]
            say('target %s %dx%d %s: events %d..%d, %d sampled draws, first pass %s' % (rid, d.width, d.height, d.format.Name(), a, b, n, p))
        finish(0)
    want_w = int(os.environ.get('RDC_WIDTH', '2560'))
    for ev in candidates[::10]:  # the first draw into a full-size 4-channel half-float target (the HDR scene colour)
        ctrl.SetFrameEvent(ev, True)
        targets = [t for t in ctrl.GetPipelineState().GetOutputTargets() if t.resource != rd.ResourceId.Null() and
                   textures[t.resource].format.compCount == 4 and textures[t.resource].format.compByteWidth == 2 and
                   textures[t.resource].width == want_w]
        if targets:
            first[0] = ev
            break
    if not targets:
        say('no draw with a colour target in a pass named like ' + marker)
        finish(1)
    tex = targets[0].resource
    desc = next(t for t in ctrl.GetTextures() if t.resourceId == tex)
    say('capture %s, pixel (%d, %d), colour target %s %dx%d %s (from event %d)' % (path, px, py, tex, desc.width, desc.height,
                                                                            desc.format.Name(), first[0]))
    sub = rd.Subresource(0, 0, 0)
    hist = ctrl.PixelHistory(tex, px, py, sub, rd.CompType.Typeless)
    say('%d modifications' % len(hist))
    png = os.environ.get('RDC_PNG')
    if png:
        last = [0]

        def deepest(a):
            last[0] = max(last[0], a.eventId)
            for c in a.children:
                deepest(c)

        for r in ctrl.GetRootActions():
            deepest(r)
        ctrl.SetFrameEvent(last[0], True)
        save = rd.TextureSave()
        save.resourceId = tex
        save.destType = rd.FileType.PNG
        save.alpha = rd.AlphaMapping.Discard
        ctrl.SaveTexture(save, png)
    for m in hist:
        a = action_of.get(m.eventId)
        name = a.GetName(sdf) if a else '?'
        c = m.postMod.col.floatValue
        fails = []
        for flag in ('sampleMasked', 'backfaceCulled', 'depthClipped', 'viewClipped', 'scissorClipped', 'shaderDiscarded',
                     'depthTestFailed', 'stencilTestFailed', 'predicationSkipped'):
            if getattr(m, flag, False):
                fails.append(flag)
        say('  ev %6d prim %4d %-60s | %-48s | post (%.3f %.3f %.3f %.3f) depth %.6f %s' % (
            m.eventId, m.primitiveID, pass_of.get(m.eventId, '?')[:60], name[:48], c[0], c[1], c[2], c[3], m.postMod.depth,
            ('FAIL ' + ','.join(fails)) if fails else 'ok'))
    finish(0)
except Exception:
    say(traceback.format_exc())
    finish(1)
