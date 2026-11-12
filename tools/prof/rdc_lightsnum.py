# The single-pass light loop's inputs in a CryMT RenderDoc capture: for a sample of draws whose pixel shader declares
# LightsNum_DB, the constant's raw bits (as int and float), LightInfoTC_DB, and the light info texture's first texels.
# Fast: every RDC_STEP-th draw only (each SetFrameEvent replays the frame up to it), at most RDC_MAX hits and RDC_SEC
# seconds; lines are written as they come.
#   set RDC_FILE=<capture.rdc> & set RDC_OUT=<report.txt> [& set RDC_STEP=40 & set RDC_MAX=10 & set RDC_SEC=150]
#   qrenderdoc.exe --python tools\prof\rdc_lightsnum.py
import os
import struct
import time
import traceback

import renderdoc as rd

path = os.environ.get('RDC_FILE', '')
out = open(os.environ.get('RDC_OUT') or path + '.lightsnum.txt', 'w', encoding='utf-8')
step = int(os.environ.get('RDC_STEP', '40'))
most = int(os.environ.get('RDC_MAX', '10'))
limit = float(os.environ.get('RDC_SEC', '150'))
t0 = time.time()


def say(text=''):
    out.write(text + '\n')
    out.flush()


def flat(vs, prefix=''):
    for v in vs:
        if v.members:
            yield from flat(v.members, prefix + v.name + '.')
        else:
            yield prefix + v.name, v


try:
    try:
        rd.InitialiseReplay(rd.GlobalEnvironment(), [])
    except Exception:
        pass
    cap = rd.OpenCaptureFile()
    if cap.OpenFile(path, '', None) != rd.ResultCode.Succeeded:
        say('cannot open ' + path)
        os._exit(1)
    result, ctrl = cap.OpenCapture(rd.ReplayOptions(), None)
    if result != rd.ResultCode.Succeeded:
        say('replay failed: ' + str(result))
        os._exit(1)
    say('opened in %.0f s' % (time.time() - t0))
    sdf = ctrl.GetStructuredFile()
    F = rd.ActionFlags
    draws = []

    def walk(a, stack):
        for c in a.children:
            walk(c, stack + ([a.GetName(sdf)] if a.flags & F.PushMarker else []))
        if a.flags & F.Drawcall:
            draws.append((a.eventId, ' / '.join(stack)))

    for r in ctrl.GetRootActions():
        walk(r, [])
    draws.sort()
    textures = {t.resourceId: t for t in ctrl.GetTextures()}
    say('draws %d, sampling every %d' % (len(draws), step))
    found = 0
    seen_tex = set()
    for ev, pas in draws[::step]:
        if found >= most or time.time() - t0 > limit:
            break
        ctrl.SetFrameEvent(ev, False)
        pipe = ctrl.GetPipelineState()
        refl = pipe.GetShaderReflection(rd.ShaderStage.Pixel)
        if not refl:
            continue
        blocks = [i for i, b in enumerate(refl.constantBlocks) if any('LightsNum' in v.name for v in b.variables)]
        if not blocks:
            continue
        found += 1
        text = []
        for i in blocks:
            bound = pipe.GetConstantBlock(rd.ShaderStage.Pixel, i, 0)
            desc = getattr(bound, 'descriptor', bound)
            vs = ctrl.GetCBufferVariableContents(pipe.GetGraphicsPipelineObject(), refl.resourceId, rd.ShaderStage.Pixel,
                                                 pipe.GetShaderEntryPoint(rd.ShaderStage.Pixel), i, desc.resource,
                                                 desc.byteOffset, desc.byteSize)
            for name, v in flat(vs):
                if 'LightsNum' in name or 'LightInfoTC' in name:
                    u = list(v.value.u32v)[:4]
                    f = [struct.unpack('<f', struct.pack('<I', x))[0] for x in u]
                    text.append('%s (cb %d) u32 %s f32 %s' % (name, i, u, ['%.4g' % x for x in f]))
        say('ev %d [%s]: %s' % (ev, pas, '; '.join(text)))
        try:
            names = [r.name for r in refl.readOnlyResources]
            for k, b in enumerate(pipe.GetReadOnlyResources(rd.ShaderStage.Pixel)):
                desc = getattr(b, 'descriptor', None)
                rid = desc.resource if desc is not None else b.resources[0].resourceId
                idx = getattr(getattr(b, 'access', None), 'index', k)
                name = names[idx] if idx < len(names) else '?'
                if 'LightInfo' not in name or rid == rd.ResourceId.Null() or str(rid) in seen_tex:
                    continue
                seen_tex.add(str(rid))
                t = textures.get(rid)
                if t is None:
                    continue
                say('  %s: %s %dx%d %s' % (name, rid, t.width, t.height, t.format.Name()))
                if t.format.compByteWidth == 4 and t.format.compCount == 4:
                    data = ctrl.GetTextureData(rid, rd.Subresource(0, 0, 0))
                    for y in range(min(8, t.height)):
                        row = [struct.unpack_from('<4f', data, 16 * (y * t.width + x)) for x in range(min(12, t.width))]
                        say('    row %d: %s' % (y, ' | '.join('%.3g %.3g %.3g %.3g' % r for r in row)))
        except Exception:
            say('  texture read failed: ' + traceback.format_exc().splitlines()[-1])
    say('done: %d hits in %.0f s' % (found, time.time() - t0))
except Exception:
    say(traceback.format_exc())
out.close()
os._exit(0)
