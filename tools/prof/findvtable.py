r"""Finds a vtable in a PE file by its first entries (function addresses): findvtable.py <dll> <va> [<va> ...]
Prints the vtable's virtual address (where the first given entry sits)."""
import struct
import sys

d = open(sys.argv[1], 'rb').read()
pe = struct.unpack_from('<I', d, 0x3C)[0]
nsec = struct.unpack_from('<H', d, pe + 6)[0]
optsize = struct.unpack_from('<H', d, pe + 20)[0]
base = struct.unpack_from('<Q', d, pe + 24 + 24)[0]
needle = b''.join(struct.pack('<Q', int(a, 16)) for a in sys.argv[2:])
for i in range(nsec):
    o = pe + 24 + optsize + 40 * i
    vsize, va, rawsize, raw = struct.unpack_from('<IIII', d, o + 8)
    pos = raw
    while True:
        pos = d.find(needle, pos, raw + rawsize)
        if pos < 0:
            break
        print('%x' % (base + va + pos - raw))
        pos += 8
