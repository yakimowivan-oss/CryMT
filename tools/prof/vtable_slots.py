"""Print function RVAs from a vtable in a PE file: vtable_slots.py <dll> <vtable VA> <count>"""
import struct
import sys

d = open(sys.argv[1], "rb").read()
vt, count = int(sys.argv[2], 16), int(sys.argv[3])
pe = struct.unpack_from("<I", d, 0x3C)[0]
base = struct.unpack_from("<Q", d, pe + 24 + 24)[0]
nsec = struct.unpack_from("<H", d, pe + 6)[0]
optsize = struct.unpack_from("<H", d, pe + 20)[0]
secs = []
for i in range(nsec):
    vs, va, rs, rp = struct.unpack_from("<IIII", d, pe + 24 + optsize + 40 * i + 8)
    secs.append((va, max(vs, rs), rp))
rva = vt - base
off = next(rva - va + rp for va, sz, rp in secs if va <= rva < va + sz)
for i in range(count):
    print(f"{i} {struct.unpack_from('<Q', d, off + i * 8)[0] - base:X}")
