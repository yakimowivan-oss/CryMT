# Every event that wrote a few texels of a texture found by size and format in a CryMT RenderDoc capture (pixel history):
# for "is this texture ever written, and with what".
#   set RDC_FILE=<capture.rdc> & set RDC_OUT=<report.txt> & set RDC_TEX=64x8 [& set RDC_TEXELS=0,0;1,0;4,0]
#   qrenderdoc.exe --python tools\prof\rdc_texhistory.py
import os
import traceback

import renderdoc as rd

path = os.environ.get('RDC_FILE', '')
out = open(os.environ.get('RDC_OUT') or path + '.texhistory.txt', 'w', encoding='utf-8')
w, h = (int(x) for x in os.environ.get('RDC_TEX', '64x8').split('x'))
texels = [tuple(int(v) for v in p.split(',')) for p in os.environ.get('RDC_TEXELS', '0,0;1,0;2,0;4,0').split(';')]


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
    names = {}

    def walk(a):
        names[a.eventId] = a.GetName(sdf)
        for c in a.children:
            walk(c)

    for r in ctrl.GetRootActions():
        walk(r)
    for t in ctrl.GetTextures():
        if t.width != w or t.height != h:
            continue
        say('texture %s %dx%d %s' % (t.resourceId, t.width, t.height, t.format.Name()))
        for x, y in texels:
            hist = ctrl.PixelHistory(t.resourceId, x, y, rd.Subresource(0, 0, 0), rd.CompType.Typeless)
            say('  texel (%d, %d): %d modifications' % (x, y, len(hist)))
            for m in hist[:12]:
                c = m.postMod.col.floatValue
                say('    ev %d %s: (%.4g %.4g %.4g %.4g)%s' % (m.eventId, names.get(m.eventId, '?')[:60], c[0], c[1], c[2], c[3],
                                                         ' discarded' if m.shaderDiscarded else ''))
    say('done')
except Exception:
    say(traceback.format_exc())
out.close()
os._exit(0)
