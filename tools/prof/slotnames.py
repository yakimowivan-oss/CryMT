"""Join gate timeline with IRenderer slot names: slotnames.py <stats> <slots>. Header index = vtable slot + 1 near EF_EndEf3D; names past overload groups may drift."""
import re
import sys

names = {}
for line in open(sys.argv[2], encoding="utf-8", errors="replace"):
    m = re.match(r"(\d+): virtual\s+(.*)", line)
    if m:
        sig = m.group(2)
        n = re.search(r"([A-Za-z_0-9]+)\s*\(", sig)
        names[int(m.group(1))] = n.group(1) if n else sig[:40]
for line in open(sys.argv[1]):
    m = re.match(r"\s*(\d+)\s+([\d.]+)\s+([\d.]+)\s+([\d.]+)", line)
    if not m:
        print(line.rstrip())
        continue
    s, fr, calls, first = m.groups()
    print(f"{int(s):4} {float(first):7.3f}ms {float(calls):8.1f}/fr {float(fr):5.0f}%  {names.get(int(s) + 1, '?')}")
