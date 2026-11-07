# Strings a code range references through RIP-relative lea/mov: a quick way to name a function.
# Usage: python fnstrings.py <dll> <start va> <end va>
import re
import struct
import sys

path, start, end = sys.argv[1], int(sys.argv[2], 16), int(sys.argv[3], 16)
d = open(path, 'rb').read()
pe = struct.unpack_from('<I', d, 0x3C)[0]
base = struct.unpack_from('<Q', d, pe + 24 + 24)[0]
count = struct.unpack_from('<H', d, pe + 6)[0]
opt = struct.unpack_from('<H', d, pe + 20)[0]
secs = [struct.unpack_from('<IIII', d, pe + 24 + opt + 40 * i + 8) for i in range(count)]


def off(va):
    r = va - base
    for vs, sva, rs, raw in secs:
        if sva <= r < sva + max(vs, rs):
            return r - sva + raw


o, e = off(start), off(end)
seen = set()
for i in range(o, e - 7):
    if d[i] in (0x48, 0x4C) and d[i + 1] in (0x8D, 0x8B) and (d[i + 2] & 0xC7) == 0x05:
        target = start + (i - o) + 7 + struct.unpack_from('<i', d, i + 3)[0]
        to = off(target)
        if to is None or target in seen:
            continue
        m = re.match(rb'[\x20-\x7e]{4,}', d[to:to + 120])
        if m:
            seen.add(target)
            print(hex(start + i - o), hex(target), m.group().decode()[:100])
