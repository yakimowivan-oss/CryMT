"""Per-window means of the CryMT frame split (crymt_detail.txt) next to frame times (crymt_frames.txt).
detail.py <detail file> <frames file> <window a-b> [<window c-d> ...] [lap=1500]"""
import sys

args = [a for a in sys.argv[3:] if not a.startswith("lap=")]
lap = int(next((a[4:] for a in sys.argv[3:] if a.startswith("lap=")), 1500))
rows = {}
for line in open(sys.argv[1]).read().splitlines()[1:]:
    p = line.split()
    rows[int(p[0])] = [float(x) for x in p[1:6]] + [float(x) for x in p[9:13]]
toks = open(sys.argv[2]).read().split()
freq = int(toks[1])
stamps = [int(x) for x in toks[2:]]
dur = {i: (stamps[i + 1] - stamps[i]) * 1000.0 / freq for i in range(len(stamps) - 1)}
extra = all(len(r) == 9 for r in rows.values())
names = ["frame", "waitEnd", "waitBarrier", "waitOther", "rtPre", "rtFlush", "mainWork"]
names += ["rtStart", "rtPreproc", "rtShadow", "shadowDraw"] if extra else []
print("window       n  " + "  ".join(f"{n:>11}" for n in names))
for w in args:
    a, b = map(int, w.split("-"))
    sel = [i for i in rows if i in dur and a <= i % lap <= b and dur[i] < 100 and i >= lap]
    if not sel:
        continue
    cols = []
    for k in range(5):
        cols.append(sum(rows[i][k] for i in sel) / len(sel))
    frame = sum(dur[i] for i in sel) / len(sel)
    work = frame - cols[0] - cols[1] - cols[2]
    more = [sum(rows[i][k] for i in sel) / len(sel) for k in range(5, 9)] if extra else []
    print(f"{w:>10} {len(sel):4d}  " + "  ".join(f"{v:11.3f}" for v in [frame] + cols + [work] + more))
