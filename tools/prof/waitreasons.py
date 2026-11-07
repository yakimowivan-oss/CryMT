"""Main-thread waits per reason in frame windows, from the last two columns of crymt_detail.txt (topReason, topWait).
waitreasons.py <detail file> <window a-b> [<window c-d> ...] [lap=1500]
Reasons: renderer slot numbers, 295 = queue at dispatch, 297.. fences, 1024 + n = wrapped method n (see crymt_stats)."""
import sys
from collections import Counter, defaultdict

args = [a for a in sys.argv[2:] if not a.startswith("lap=")]
lap = int(next((a[4:] for a in sys.argv[2:] if a.startswith("lap=")), 1500))
rows = [l.split() for l in open(sys.argv[1]).read().splitlines()[1:]]
for w in args:
    a, b = map(int, w.split("-"))
    sel = [r for r in rows if len(r) >= 9 and a <= int(r[0]) % lap <= b and int(r[0]) >= lap]
    total, count = defaultdict(float), Counter()
    for r in sel:
        total[int(r[7])] += float(r[8])
        count[int(r[7])] += 1
    print(f"{w}: {len(sel)} frames, top wait per frame (ms per frame over the window)")
    for reason, t in sorted(total.items(), key=lambda x: -x[1])[:10]:
        print(f"   reason {reason:5d}: {t / max(len(sel), 1):6.3f} ms/frame, top in {count[reason]} frames")
