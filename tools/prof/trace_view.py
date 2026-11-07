"""Compress a CryMT call trace into runs: trace_view.py <trace> <vt_rva> <slot names> [max lines]"""
import re
import sys

names = {}
for line in open(sys.argv[3], encoding="utf-8", errors="replace"):
    m = re.match(r"(\d+): virtual\s+(.*)", line)
    if m:
        n = re.search(r"([A-Za-z_0-9]+)\s*\(", m.group(2))
        names[int(m.group(1))] = n.group(1) if n else "?"
limit = int(sys.argv[4]) if len(sys.argv) > 4 else 200
rows = []
for line in open(sys.argv[1]):
    p = line.split()
    if len(p) != 4 or not re.match(r"[\d.]+$", p[0]):
        print(line.rstrip())
        continue
    rows.append((float(p[0]), int(p[1]), p[2], p[3]))
out = 0
i = 0
while i < len(rows) and out < limit:
    t, slot, arg, ret = rows[i]
    j = i
    mod = ret.split("+")[0]
    while j + 1 < len(rows) and rows[j + 1][1] == slot and rows[j + 1][3].split("+")[0] == mod:
        j += 1
    tail = f" x{j - i + 1}" if j > i else ""
    print(f"{t:8.3f} {slot:4} {names.get(slot + 1, '?'):26.26} arg={arg[-8:]} {ret}{tail}")
    i = j + 1
    out += 1
print(f"... {len(rows)} calls, {i} consumed")
