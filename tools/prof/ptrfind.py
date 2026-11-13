import struct, sys
# ptrfind.py <dll> <VA hex>: where the 8-byte pointer to VA sits in the image, and the table around it.
d = open(sys.argv[1], 'rb').read()
va = int(sys.argv[2], 16)
pe = struct.unpack_from('<I', d, 0x3C)[0]
nsec = struct.unpack_from('<H', d, pe + 6)[0]
opt = struct.unpack_from('<H', d, pe + 20)[0]
base = struct.unpack_from('<Q', d, pe + 24 + 24)[0]
secs = [struct.unpack_from('<IIII', d, pe + 24 + opt + 40 * i + 8) for i in range(nsec)]
def va_of(off):
    for vs, sva, rs, rp in secs:
        if rp <= off < rp + rs:
            return base + sva + off - rp
def off_of(v):
    for vs, sva, rs, rp in secs:
        if sva <= v - base < sva + max(vs, rs):
            return v - base - sva + rp
def cstr(v):
    o = off_of(v)
    if o is None:
        return None
    e = d.find(b'\0', o)
    s = d[o:e]
    return s.decode('latin1') if 0 < len(s) < 64 and all(32 <= c < 127 for c in s) else None
needle = struct.pack('<Q', va)
at = 0
while (at := d.find(needle, at)) >= 0:
    print('pointer at', hex(va_of(at)))
    for k in range(-6, 7):
        o = at + k * 16
        a, b = struct.unpack_from('<QQ', d, o)
        print('  %+d %s: %x %x %s' % (k, hex(va_of(o)), a, b, cstr(a)))
    at += 8
