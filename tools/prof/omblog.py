import sys
from collections import defaultdict
# omblog.py <crymt_omb.txt>: per moving object (tracked by its place), whether the previous place the engine gives in frame
# n equals the place it was drawn at in frame n-1, and how far it moved.
rows = []
for line in open(sys.argv[1]):
    t = line.split()
    if len(t) < 14:
        continue
    fr = int(t[0])
    now = tuple(map(float, t[6:9]))
    prev = tuple(map(float, t[10:13]))
    rows.append((fr, t[2], now, prev, float(t[14])))
by_frame = defaultdict(list)
for r in rows:
    by_frame[r[0]].append(r)
frames = sorted(by_frame)
print('frames', len(frames), frames[0], frames[-1], 'lines', len(rows))

def d(a, b):
    return sum((x - y) ** 2 for x, y in zip(a, b)) ** 0.5

moving = [r for r in rows if d(r[2], r[3]) > 0.005 and d(r[2], r[3]) < 3]
print('moving entries', len(moving))
# for moving objects: find in the previous logged frame the object nearest to this prev place
shown = 0
stats = []
for r in moving:
    fr = r[0]
    i = frames.index(fr)
    if i == 0:
        continue
    pf = frames[i - 1]
    cands = [q for q in by_frame[pf] if d(q[2], r[3]) < 1.0]
    if not cands:
        continue
    q = min(cands, key=lambda q: d(q[2], r[3]))
    step = d(r[2], r[3])
    gap = d(q[2], r[3])
    # also: is the prev closer to the place two frames back?
    two = None
    if i >= 2:
        c2 = [s for s in by_frame[frames[i - 2]] if d(s[2], r[3]) < 1.0]
        if c2:
            two = min(d(s[2], r[3]) for s in c2)
    stats.append((step, gap, two, fr - pf))
    if shown < 25:
        print(f'frame {fr} (prev logged {pf}): moved {step:.4f} m; prev vs drawn place last frame {gap:.4f} m; vs two frames back {two if two is None else round(two, 4)}; f {r[4]:.3f}')
        shown += 1
if stats:
    import statistics
    print('median moved', statistics.median(s[0] for s in stats), 'median gap to last frame', statistics.median(s[1] for s in stats),
          'frame gaps', sorted(set(s[3] for s in stats))[:10])
