r"""Reads 64-bit values at virtual addresses of a PE file on disk (vtable entries, pointers in data):
peptr.py <dll> <va hex> [<va hex> ...]"""
import struct
import sys

d = open(sys.argv[1], 'rb').read()
pe = struct.unpack_from('<I', d, 0x3C)[0]
nsec = struct.unpack_from('<H', d, pe + 6)[0]
optsize = struct.unpack_from('<H', d, pe + 20)[0]
base = struct.unpack_from('<Q', d, pe + 24 + 24)[0]
secs = []
for i in range(nsec):
    o = pe + 24 + optsize + 40 * i
    vsize, va, rawsize, raw = struct.unpack_from('<IIII', d, o + 8)
    secs.append((va, max(vsize, rawsize), raw))
for a in sys.argv[2:]:
    rva = int(a, 16) - base
    for va, size, raw in secs:
        if va <= rva < va + size:
            print('%s: %016x' % (a, struct.unpack_from('<Q', d, raw + rva - va)[0]))
            break
    else:
        print('%s: not in a section' % a)
