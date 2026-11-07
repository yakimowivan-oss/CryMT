"""Per-function time of one sampled thread in two frame windows (ms per frame), symbolized with Ghidra exports.
winfuncs.py <prefix> <thread> <window a-b> <window c-d> [lap=1500] [top=25] [incl=1]
<prefix>: CrySampler2 output (.frames.txt, .samples.txt); functions: logs\dump\funcs\<module>.funcs.txt
(ExportFunctions.java). Exclusive time by default; incl=1 counts every function on the stack once."""
import bisect
import os
import struct
import sys
from collections import Counter

prefix, thread = sys.argv[1], sys.argv[2]
wins = [tuple(map(int, a.split("-"))) for a in sys.argv[3:5]]
opts = {a.split("=")[0]: int(a.split("=")[1]) for a in sys.argv[5:] if "=" in a}
lap, top, incl = opts.get("lap", 1500), opts.get("top", 25), opts.get("incl", 0)
funcs_dir = r"D:\Games\Crysis-RE\logs\dump\funcs"
bin_dir = r"D:\Games\Crysis\Bin64"
tables = {}


def table(module):
    if module not in tables:
        tables[module] = None
        path = os.path.join(funcs_dir, module + ".funcs.txt")
        dll = os.path.join(bin_dir, module)
        if os.path.exists(path) and os.path.exists(dll):
            d = open(dll, "rb").read(4096)
            pe = struct.unpack_from("<I", d, 0x3C)[0]
            base = struct.unpack_from("<Q", d, pe + 48)[0]
            rows = sorted((int(a, 16) - base, int(s, 16), n) for a, s, n in (l.split(None, 2) for l in open(path)))
            tables[module] = ([r[0] for r in rows], rows)
    return tables[module]


def name(frame):
    if frame == "?" or "!+" not in frame:
        return "?"
    module, off = frame.split("!+")
    off = int(off, 16)
    t = table(module)
    if t:
        i = bisect.bisect_right(t[0], off) - 1
        if i >= 0 and off < t[1][i][0] + max(t[1][i][1], 1):
            return f"{module.replace('.dll', '')}:{t[1][i][2].strip()}"
    return f"{module.replace('.dll', '')}+{off >> 8:x}xx"


l = open(prefix + ".frames.txt").read().split()
freq, st = int(l[1]), [int(x) for x in l[2:]]
dur = [(st[i + 1] - st[i]) * 1000.0 / freq for i in range(len(st) - 1)]
sel = [{i for i, x in enumerate(dur) if a <= i % lap <= b and x < 100} for a, b in wins]
ms = [sum(dur[i] for i in s) / len(s) for s in sel]
cnt = [Counter(), Counter()]
tot = [0, 0]
for line in open(prefix + ".samples.txt"):
    p = line.rstrip("\n").split("\t")
    if len(p) < 3 or p[0] != thread:
        continue
    f = bisect.bisect_right(st, int(p[1])) - 1
    for k in (0, 1):
        if f in sel[k]:
            tot[k] += 1
            frames = p[2].split(";")
            keys = set(name(x) for x in frames) if incl else {name(frames[-1])}
            for key in keys:
                cnt[k][key] += 1
print(f"thread {thread}, frame ms {ms[0]:.2f} / {ms[1]:.2f}, samples {tot[0]} / {tot[1]}, {'inclusive' if incl else 'exclusive'}")
rows = []
for key in set(cnt[0]) | set(cnt[1]):
    a = ms[0] * cnt[0][key] / max(tot[0], 1)
    b = ms[1] * cnt[1][key] / max(tot[1], 1)
    rows.append((b - a, a, b, key))
for d, a, b, key in sorted(rows, reverse=True)[:top]:
    print(f"  {a:6.2f} -> {b:6.2f} ({d:+.2f})  {key[:110]}")
