"""Exclusive (leaf) time per function for one thread: explosion window versus calm window, in ms per frame.
leaves.py <prefix> <thread> <slow a-b> <normal c-d> <lap>"""
import bisect, sys
from collections import Counter
prefix, t = sys.argv[1], int(sys.argv[2])
(a, b), (c, d), lap = map(int, sys.argv[3].split("-")), map(int, sys.argv[4].split("-")), int(sys.argv[5])
l = open(prefix + ".frames.txt").read().split(); freq = int(l[1]); st = [int(x) for x in l[2:]]
dur = [(st[i + 1] - st[i]) * 1000.0 / freq for i in range(len(st) - 1)]
win = {0: {i for i, x in enumerate(dur) if c <= i % lap <= d and x < 100}, 1: {i for i, x in enumerate(dur) if a <= i % lap <= b and x < 100}}
ms = {k: sum(dur[i] for i in v) / len(v) for k, v in win.items()}
cnt = {0: Counter(), 1: Counter()}; tot = {0: 0, 1: 0}
for line in open(prefix + ".samples.txt"):
    p = line.rstrip("\n").split("\t")
    if len(p) < 3 or int(p[0]) != t or not p[2]: continue
    f = bisect.bisect_right(st, int(p[1])) - 1
    for k in (0, 1):
        if f in win[k]:
            tot[k] += 1; cnt[k][p[2].split(";")[-1]] += 1
print(f"frame ms: explosion {ms[1]:.2f} calm {ms[0]:.2f}")
rows = []
for key in set(cnt[0]) | set(cnt[1]):
    x0 = ms[0] * cnt[0][key] / tot[0]; x1 = ms[1] * cnt[1][key] / tot[1]
    rows.append((x1 - x0, x1, x0, key))
for dlt, x1, x0, key in sorted(rows, reverse=True)[:25]:
    print(f"  {x1:5.2f} ms vs {x0:5.2f} ms  ({dlt:+.2f})  {key}")
