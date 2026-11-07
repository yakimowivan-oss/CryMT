"""List every RTTI vtable of an x64 MSVC PE: all_vtables.py <dll> <out file>
Writes lines "<vtable VA hex> <slot count> <class name>"; slots are the leading pointers into executable sections."""
import struct
import sys

d = open(sys.argv[1], "rb").read()
pe = struct.unpack_from("<I", d, 0x3C)[0]
nsec = struct.unpack_from("<H", d, pe + 6)[0]
optsize = struct.unpack_from("<H", d, pe + 20)[0]
base = struct.unpack_from("<Q", d, pe + 24 + 24)[0]
secs = []
for i in range(nsec):
    vs, va, rs, rp = struct.unpack_from("<IIII", d, pe + 24 + optsize + 40 * i + 8)
    flags = struct.unpack_from("<I", d, pe + 24 + optsize + 40 * i + 36)[0]
    secs.append((va, max(vs, rs), rp, rs, flags))


def rva2off(r):
    for va, size, rp, rs, fl in secs:
        if va <= r < va + size and r - va < rs:
            return r - va + rp
    return None


def is_code(va):
    r = va - base
    return any(v <= r < v + size and fl & 0x20 for v, size, rp, rs, fl in secs)


# Complete object locator (VS2005 x64: signature 0, no self field): signature, offset, cdOffset,
# type descriptor rva, class hierarchy rva. The type descriptor's name starts with ".?A".
cols = {}
for va, size, rp, rs, fl in secs:
    if fl & 0x20:
        continue
    for o in range(rp, rp + rs - 20, 4):
        sig, off, cd, td, chd = struct.unpack_from("<IIIII", d, o)
        if sig > 1 or off > 0x10000 or not td or not chd:
            continue
        to = rva2off(td + 16)
        if to is None or d[to:to + 3] != b".?A" or rva2off(chd) is None:
            continue
        name = d[to:d.index(b"\0", to)].decode("latin1")
        cols[base + o - rp + va] = (name, off)
out = open(sys.argv[2], "w")
count = 0
for va, size, rp, rs, fl in secs:
    if fl & 0x20:
        continue
    for o in range(rp, rp + rs - 8, 8):
        col = struct.unpack_from("<Q", d, o)[0]
        if col not in cols:
            continue
        vt = base + (o - rp + va) + 8
        n = 0
        while True:
            vo = rva2off(vt - base + 8 * n)
            if vo is None:
                break
            p = struct.unpack_from("<Q", d, vo)[0]
            if not is_code(p) or (n and (vt + 8 * n - base + base) in [c for c in ()]):
                break
            n += 1
            if n > 1 and struct.unpack_from("<Q", d, vo)[0] in cols:
                break
        if n:
            name, off = cols[col]
            out.write(f"{vt:x} {n} {name} {off}\n")
            count += 1
print("vtables:", count)
