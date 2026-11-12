r"""Frame times at one place, per variant, from spot runs (tools\prof\spot_run.ps1 -> logs\spot): from the census reset
(crymt_statsreset) to "quit", frame numbers in crymt_focus.txt. Older runs without the reset: from <skip> s after the
camera was turned ("#g_localActor:SetAngles") to quit or the last frame the frame log wrote (blocks of 3000 frames).
spot_report.py <dir> <tag> <variant,variant,...> [skip=8]"""
import os
import re
import sys

d, tag, variants = sys.argv[1], sys.argv[2], sys.argv[3].split(',')
skip = float(sys.argv[4]) if len(sys.argv) > 4 else 8.0


def commands(focus):
    text = open(focus, encoding='utf-8', errors='replace').read()
    text = text[text.rfind("console command 'r_DynTexMaxSize"):]
    reset = re.search(r"frame (\d+): census counts reset", text)
    a = re.search(r"frame (\d+): console command '#g_localActor:SetAngles", text)
    b = re.search(r"frame (\d+): console command 'quit'", text)
    if reset and b:
        return int(reset.group(1)), int(b.group(1)), True
    return (int(a.group(1)), int(b.group(1)), False) if a and b else None


def pct(xs, p):
    xs = sorted(xs)
    return xs[min(len(xs) - 1, int(len(xs) * p))]


print('%-10s %6s %6s %7s %7s %7s %7s %13s | %8s %8s %8s %8s | %s' % (
    'variant', 'frames', 'sec', 'FPS', 'ms', 'p95', 'p99', 'FPS halves', 'mainWork', 'waitBar', 'rtPre', 'rtFlush', 'census (the window with a reset, else the session)'))
for v in variants:
    p = lambda f: os.path.join(d, '%s_%s_%s' % (tag, v, f))
    try:
        w = commands(p('crymt_focus.txt'))
        toks = open(p('crymt_frames.txt')).read().split()
        freq = int(toks[1])
        stamps = [int(x) for x in toks[2:]]
        rows = {}
        for line in open(p('crymt_detail.txt')).read().splitlines()[1:]:
            q = line.split()
            rows[int(q[0])] = [float(x) for x in q[1:6]]
    except (OSError, ValueError, IndexError) as e:
        print('%-10s missing: %s' % (v, e))
        continue
    if not w or w[0] >= len(stamps):
        print('%-10s no window' % v)
        continue
    a = w[0]
    while not w[2] and a < len(stamps) - 1 and (stamps[a] - stamps[w[0]]) / freq < skip:
        a += 1
    b = min(w[1] - 5, len(stamps) - 1)
    sel = list(range(a, b))
    if len(sel) < 100:
        print('%-10s only %d frames' % (v, len(sel)))
        continue
    ms = [(stamps[i + 1] - stamps[i]) * 1000.0 / freq for i in sel]
    mean = sum(ms) / len(ms)
    half = len(ms) // 2
    halves = '%5.1f %5.1f' % (1000.0 * half / sum(ms[:half]), 1000.0 * (len(ms) - half) / sum(ms[half:]))
    det = [i for i in sel if i in rows]
    cols = [sum(rows[i][k] for i in det) / max(1, len(det)) for k in range(5)]
    work = mean - cols[0] - cols[1] - cols[2]
    census = ''
    try:
        text = open(p('d3d10_census.txt'), encoding='utf-8', errors='replace').read()
        m = re.search(r'draws ([\d.]+) \(packets ([\d.]+)', text)
        census = 'draws %s (packets %s)' % m.groups() if m else ''
        m = re.search(r'GPU busy ([\d.]+)', text)
        census += ', GPU busy %s ms' % m.group(1) if m else ''
        m = re.search(r'stream thread busy ([\d.]+)', text)
        census += ', stream thread %s ms' % m.group(1) if m else ''
    except OSError:
        pass
    print('%-10s %6d %6.1f %7.1f %7.2f %7.2f %7.2f %13s | %8.2f %8.2f %8.2f %8.2f | %s' % (
        v, len(ms), sum(ms) / 1000.0, 1000.0 / mean, mean, pct(ms, 0.95), pct(ms, 0.99), halves, work, cols[1], cols[3], cols[4], census))
