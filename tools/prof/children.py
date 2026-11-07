"""Time below a function split by its direct callees, for one thread and a frame window.
children.py <prefix> <thread index> <function key> <window a-b> <lap> [depth]"""
import bisect
import sys
from collections import Counter

prefix, tidx, target, window, lap = sys.argv[1], int(sys.argv[2]), sys.argv[3], sys.argv[4], int(sys.argv[5])
depth = int(sys.argv[6]) if len(sys.argv) > 6 else 1
a, b = map(int, window.split("-"))
l = open(prefix + ".frames.txt").read().split()
freq = int(l[1]); stamps = [int(x) for x in l[2:]]
dur = [(stamps[i + 1] - stamps[i]) * 1000.0 / freq for i in range(len(stamps) - 1)]
frames_in = {i for i, x in enumerate(dur) if a <= i % lap <= b and x < 100}
total = 0
under = 0
paths = Counter()
for line in open(prefix + ".samples.txt"):
    p = line.rstrip("\n").split("\t")
    if len(p) < 3 or int(p[0]) != tidx:
        continue
    f = bisect.bisect_right(stamps, int(p[1])) - 1
    if f not in frames_in:
        continue
    total += 1
    st = p[2].split(";")
    if target not in st:
        continue
    under += 1
    i = len(st) - 1 - st[::-1].index(target)
    paths[" > ".join(st[i + 1:i + 1 + depth]) or "<self>"] += 1
ms = sum(dur[i] for i in frames_in) / len(frames_in)
print(f"thread {tidx}: {under} of {total} samples under {target}; frame {ms:.2f} ms")
for k, v in paths.most_common(25):
    print(f"  {100.0 * v / total:5.1f}% of frame ({ms * v / total:5.2f} ms)  {k}")
