r"""Print the C string at each VA of a DLL: dllstr.py <dll> <hexVA>..."""
import struct
import sys

d = open(sys.argv[1], 'rb').read()
pe = struct.unpack_from('<I', d, 0x3C)[0]
n = struct.unpack_from('<H', d, pe + 6)[0]
opt = struct.unpack_from('<H', d, pe + 20)[0]
base = struct.unpack_from('<Q', d, pe + 48)[0]
secs = [struct.unpack_from('<IIII', d, pe + 24 + opt + 40 * i + 8) for i in range(n)]
for a in sys.argv[2:]:
    va = int(a, 16)
    for vs, v, rs, rp in secs:
        if v <= va - base < v + max(vs, rs):
            off = va - base - v + rp
            s = d[off:off + 200].split(b'\0')[0]
            print('%x: %r' % (va, s))
            break
    else:
        print('%x: outside the sections' % va)
