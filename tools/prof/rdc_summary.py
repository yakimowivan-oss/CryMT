# Summary of a RenderDoc capture of CryMT, as text: per named pass (CryMT's pass names: "L<level> <list>: <filters>",
# "CryMT GI", ...) the draws, dispatches, copies and GPU time measured by replaying the capture.
# Runs inside RenderDoc's own Python (it has the renderdoc module):
#   set RDC_FILE=<capture.rdc> & set RDC_OUT=<report.txt> & qrenderdoc.exe --python tools\prof\rdc_summary.py
# Writes RDC_OUT (default: the capture's name + .txt) and exits without opening the UI.
import os
import sys
import traceback

import renderdoc as rd

path = os.environ.get('RDC_FILE', '')
out_path = os.environ.get('RDC_OUT') or path + '.txt'
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
        pass  # qrenderdoc has initialised it already
    cap = rd.OpenCaptureFile()
    if cap.OpenFile(path, '', None) != rd.ResultCode.Succeeded:
        say('cannot open ' + path)
        finish(1)
    if not cap.LocalReplaySupport():
        say('capture cannot be replayed on this machine')
        finish(1)
    result, ctrl = cap.OpenCapture(rd.ReplayOptions(), None)
    if result != rd.ResultCode.Succeeded:
        say('replay failed: ' + str(result))
        finish(1)
    sdf = ctrl.GetStructuredFile()
    roots = ctrl.GetRootActions()

    duration = {}
    if rd.GPUCounter.EventGPUDuration in ctrl.EnumerateCounters():
        for r in ctrl.FetchCounters([rd.GPUCounter.EventGPUDuration]):
            duration[r.eventId] = r.value.d

    F = rd.ActionFlags
    passes = {}   # name -> [count, draws, dispatches, copies, gpu seconds, indices]
    order = []
    totals = [0, 0, 0, 0.0, 0]

    def tally(a, stack):
        kind = a.flags
        if kind & F.PushMarker:
            name = a.GetName(sdf)
            key = ' / '.join(stack + [name])
            if key not in passes:
                passes[key] = [0, 0, 0, 0, 0.0, 0]
                order.append(key)
            passes[key][0] += 1
            for c in a.children:
                tally(c, stack + [name])
            return
        if a.children:
            for c in a.children:
                tally(c, stack)
            return
        key = ' / '.join(stack) if stack else '(no pass name)'
        if key not in passes:
            passes[key] = [0, 0, 0, 0, 0.0, 0]
            order.append(key)
        p = passes[key]
        t = duration.get(a.eventId, 0.0)
        if kind & F.Drawcall:
            p[1] += 1
            p[5] += a.numIndices * max(1, a.numInstances)
            totals[0] += 1
            totals[4] += a.numIndices * max(1, a.numInstances)
        elif kind & F.Dispatch:
            p[2] += 1
            totals[1] += 1
        elif kind & (F.Copy | F.Resolve | F.Clear):
            p[3] += 1
            totals[2] += 1
        p[4] += t
        totals[3] += t

    for a in roots:
        tally(a, [])

    # RDC_TREE=<depth>: the action tree in event order (markers and runs of draws) instead of the table.
    tree_depth = int(os.environ.get('RDC_TREE', '0') or 0)
    if tree_depth:
        def walk(a, depth):
            if a.flags & F.PushMarker:
                kids = list(a.children)
                draws = sum(1 for k in kids if k.flags & F.Drawcall)
                say('%s[%d..%d] %s  (%d children, %d draws direct)' % ('  ' * depth, a.eventId, kids[-1].eventId if kids else a.eventId,
                                                                    a.GetName(sdf), len(kids), draws))
                if depth + 1 < tree_depth:
                    run = []
                    for k in kids:
                        if k.flags & F.PushMarker:
                            if run:
                                say('%s  %d draws/calls %d..%d' % ('  ' * depth, len(run), run[0], run[-1]))
                                run = []
                            walk(k, depth + 1)
                        else:
                            run.append(k.eventId)
                    if run:
                        say('%s  %d draws/calls %d..%d' % ('  ' * depth, len(run), run[0], run[-1]))
        run = []
        for a in roots:
            if a.flags & F.PushMarker:
                if run:
                    say('%d unnamed calls %d..%d' % (len(run), run[0], run[-1]))
                    run = []
                walk(a, 0)
            else:
                run.append(a.eventId)
        if run:
            say('%d unnamed calls %d..%d' % (len(run), run[0], run[-1]))
        ctrl.Shutdown()
        cap.Shutdown()
        finish(0)
    say('capture: ' + path)
    say('GPU time per event: ' + ('measured by replay (EventGPUDuration)' if duration else 'not available'))
    say('totals: %d draws, %d dispatches, %d copies/clears, %.2f M indices, GPU %.2f ms (sum of events)'
        % (totals[0], totals[1], totals[2], totals[4] / 1e6, totals[3] * 1000))
    say('')
    say('%-70s %6s %6s %5s %5s %8s %8s' % ('pass (in order of first use)', 'times', 'draws', 'disp', 'copy', 'Mindex', 'GPU ms'))
    for key in order:
        p = passes[key]
        if p[1] or p[2] or p[3] or p[4] > 0:
            say('%-70s %6d %6d %5d %5d %8.2f %8.3f' % (key[-70:], p[0], p[1], p[2], p[3], p[5] / 1e6, p[4] * 1000))
    say('')
    say('top 12 by GPU time:')
    for key in sorted(order, key=lambda k: -passes[k][4])[:12]:
        p = passes[key]
        say('  %8.3f ms  %5d draws  %s' % (p[4] * 1000, p[1], key))
    ctrl.Shutdown()
    cap.Shutdown()
    finish(0)
except Exception:
    say(traceback.format_exc())
    finish(2)
