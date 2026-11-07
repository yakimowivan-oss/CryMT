"""Compare what each sampled thread does in slow frames versus all frames.
slowframes.py <prefix> [slow percentile, default 95]
Reads <prefix>.frames.txt (QPC per dispatched frame), .threads.txt and .samples.txt from CrySampler2."""
import bisect
import sys
from collections import Counter, defaultdict

prefix = sys.argv[1]
pct = float(sys.argv[2]) if len(sys.argv) > 2 else 95.0
# optional: <slow window a-b> <normal window c-d> <lap length>: compare frames by position in the lap
window = sys.argv[3] if len(sys.argv) > 3 else None
lines = open(prefix + ".frames.txt").read().split()
freq = int(lines[1])
stamps = [int(x) for x in lines[2:]]
dur = [(stamps[i + 1] - stamps[i]) * 1000.0 / freq for i in range(len(stamps) - 1)]
srt = sorted(dur)
cut = srt[int(len(srt) * pct / 100)]
med = srt[len(srt) // 2]
slow = {i for i, d in enumerate(dur) if d >= cut}
normal_only = None
if window:
    a, b = map(int, window.split("-"))
    c, d = map(int, sys.argv[4].split("-"))
    lap = int(sys.argv[5])
    slow = {i for i, x in enumerate(dur) if a <= i % lap <= b and x < 100}
    normal_only = {i for i, x in enumerate(dur) if c <= i % lap <= d and x < 100}
    print("window mean ms: slow %.2f normal %.2f" % (sum(dur[i] for i in slow) / len(slow), sum(dur[i] for i in normal_only) / len(normal_only)))
print(f"frames={len(dur)} median={med:.2f} ms p{pct:.0f}={cut:.2f} ms max={srt[-1]:.2f} ms slow frames={len(slow)}")
# runs of consecutive slow frames show where the spikes happen
runs, start = [], None
for i in range(len(dur)):
    if i in slow and start is None:
        start = i
    if i not in slow and start is not None:
        if i - start >= 3:
            runs.append((start, i - 1, sum(dur[start:i]) / (i - start)))
        start = None
print("longest slow runs (first frame, last frame, avg ms):", sorted(runs, key=lambda r: -(r[1] - r[0]))[:8])

threads = {}
for line in open(prefix + ".threads.txt"):
    idx, tid, start_key, cpu = line.rstrip("\n").split("\t")
    threads[int(idx)] = f"t{idx} tid={tid} {start_key} {cpu}"

IDLE = ("CryMT.dll",)
incl = defaultdict(lambda: [Counter(), Counter()])
tot = defaultdict(lambda: [0, 0])
for line in open(prefix + ".samples.txt"):
    parts = line.rstrip("\n").split("\t")
    if len(parts) < 3 or not parts[2]:
        continue
    t = int(parts[0])
    q = int(parts[1])
    f = bisect.bisect_right(stamps, q) - 1
    if f < 0 or f >= len(dur):
        continue
    if normal_only is not None and f not in slow and f not in normal_only:
        continue
    bucket = 1 if f in slow else 0
    frames = parts[2].split(";")
    tot[t][bucket] += 1
    for k in set(frames):
        incl[t][bucket][k] += 1
    leaf_mod = frames[-1].split("!")[0]
    incl[t][bucket]["<leaf module> " + leaf_mod] += 1

for t in sorted(threads):
    n0, n1 = tot[t]
    if not n0 or not n1:
        continue
    print(f"\n== {threads[t]}  samples normal={n0} slow={n1}")
    rows = []
    for k in set(incl[t][0]) | set(incl[t][1]):
        a = 100.0 * incl[t][0][k] / n0
        b = 100.0 * incl[t][1][k] / n1
        if max(a, b) >= 3:
            rows.append((b - a, a, b, k))
    for delta, a, b, k in sorted(rows, reverse=True)[:22]:
        print(f"  slow {b:5.1f}%  normal {a:5.1f}%  delta {delta:+5.1f}  {k}")
