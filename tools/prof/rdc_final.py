# The final picture of a CryMT RenderDoc capture (the swap chain's back buffer at the last event) as a PNG.
#   set RDC_FILE=<capture.rdc> & set RDC_PNG=<out.png> & qrenderdoc.exe --python tools\prof\rdc_final.py
import os
import traceback

import renderdoc as rd

path = os.environ.get('RDC_FILE', '')
png = os.environ.get('RDC_PNG') or path + '.png'
log = []
try:
    try:
        rd.InitialiseReplay(rd.GlobalEnvironment(), [])
    except Exception:
        pass
    cap = rd.OpenCaptureFile()
    cap.OpenFile(path, '', None)
    result, ctrl = cap.OpenCapture(rd.ReplayOptions(), None)
    last = [0]

    def deepest(a):
        last[0] = max(last[0], a.eventId)
        for c in a.children:
            deepest(c)

    for r in ctrl.GetRootActions():
        deepest(r)
    ctrl.SetFrameEvent(last[0], True)
    swap = [t for t in ctrl.GetTextures() if t.creationFlags & rd.TextureCategory.SwapBuffer]
    log.append('event %d, swap buffers %d' % (last[0], len(swap)))
    if swap:
        save = rd.TextureSave()
        save.resourceId = swap[0].resourceId
        save.destType = rd.FileType.PNG
        save.alpha = rd.AlphaMapping.Discard
        ctrl.SaveTexture(save, png)
        log.append('saved %s %dx%d' % (png, swap[0].width, swap[0].height))
except Exception:
    log.append(traceback.format_exc())
with open(png + '.txt', 'w', encoding='utf-8') as f:
    f.write('\n'.join(log) + '\n')
os._exit(0)
