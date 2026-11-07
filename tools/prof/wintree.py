"""Top-down call tree of one sampled thread in two frame windows (ms per frame), symbolized like winfuncs.py.
wintree.py <prefix> <thread> <window a-b> <window c-d> [root=<name substring>] [min=0.1] [depth=6] [lap=1500]
Recursion is folded (a function directly below itself counts once). Without root the tree starts at the stack bottom."""
import bisect
import os
import sys
from collections import defaultdict

sys.argv[0] = os.path.join(os.path.dirname(os.path.abspath(__file__)), "winfuncs.py")
prefix, thread = sys.argv[1], sys.argv[2]
wins = [tuple(map(int, a.split("-"))) for a in sys.argv[3:5]]
opts = dict(a.split("=", 1) for a in sys.argv[5:] if "=" in a)
root, mn, depth, lap = opts.get("root"), float(opts.get("min", 0.1)), int(opts.get("depth", 6)), int(opts.get("lap", 1500))

# Reuse winfuncs' symbol tables without running its main part.
src = open(sys.argv[0]).read()
ns = {"__name__": "wintree", "os": os, "bisect": bisect, "struct": __import__("struct")}
exec(src[src.index("funcs_dir"):src.index("l = open(prefix")], ns)
name = ns["name"]

l = open(prefix + ".frames.txt").read().split()
freq, st = int(l[1]), [int(x) for x in l[2:]]
dur = [(st[i + 1] - st[i]) * 1000.0 / freq for i in range(len(st) - 1)]
sel = [{i for i, x in enumerate(dur) if a <= i % lap <= b and x < 100} for a, b in wins]
ms = [sum(dur[i] for i in s) / len(s) for s in sel]
tree = [defaultdict(int), defaultdict(int)]
tot = [0, 0]
cache = {}
for line in open(prefix + ".samples.txt"):
    p = line.rstrip("\n").split("\t")
    if len(p) < 3 or p[0] != thread:
        continue
    f = bisect.bisect_right(st, int(p[1])) - 1
    for k in (0, 1):
        if f not in sel[k]:
            continue
        tot[k] += 1
        path = []
        for x in p[2].split(";"):
            n = cache.get(x) or cache.setdefault(x, name(x))
            if n == "?" or (path and path[-1] == n):
                continue
            path.append(n)
        if root:
            hits = [i for i, n in enumerate(path) if root in n]
            if not hits:
                continue
            path = path[hits[0]:]
        for d in range(1, min(depth, len(path)) + 1):
            tree[k][tuple(path[:d])] += 1
print(f"thread {thread}, frame ms {ms[0]:.2f} / {ms[1]:.2f}, samples {tot[0]} / {tot[1]}")
val = lambda k, key: ms[k] * tree[k].get(key, 0) / max(tot[k], 1)


def walk(prefix_key):
    kids = {key for t in tree for key in t if len(key) == len(prefix_key) + 1 and key[:-1] == prefix_key}
    for key in sorted(kids, key=lambda c: -max(val(0, c), val(1, c))):
        a, b = val(0, key), val(1, key)
        if max(a, b) < mn:
            continue
        print(f"  {a:6.2f} -> {b:6.2f} ({b - a:+.2f})  {'  ' * (len(key) - 1)}{key[-1][:90]}")
        walk(key)


walk(())
