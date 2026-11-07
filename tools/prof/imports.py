"""List imports of a PE: imports.py <file> [dll substring]"""
import struct, sys
d = open(sys.argv[1], "rb").read(); flt = sys.argv[2].lower() if len(sys.argv) > 2 else ""
pe = struct.unpack_from("<I", d, 0x3C)[0]; nsec = struct.unpack_from("<H", d, pe + 6)[0]; optsize = struct.unpack_from("<H", d, pe + 20)[0]
opt = pe + 24; is64 = struct.unpack_from("<H", d, opt)[0] == 0x20B; imp = struct.unpack_from("<I", d, opt + (112 if is64 else 96) + 8)[0]
secs = [struct.unpack_from("<IIII", d, opt + optsize + 40 * i + 8) for i in range(nsec)]
def off(r): return next(r - va + rp for vs, va, rs, rp in secs if va <= r < va + max(vs, rs))
def cs(o): return d[o:d.index(b"\0", o)].decode()
o = off(imp)
while True:
    oft, _, _, nm, ft = struct.unpack_from("<IIIII", d, o)
    if not nm: break
    dll = cs(off(nm))
    if flt in dll.lower():
        t = off(oft or ft); names = []
        while True:
            v = struct.unpack_from("<Q" if is64 else "<I", d, t)[0]
            if not v: break
            names.append(f"#{v & 0xFFFF}" if v >> (63 if is64 else 31) else cs(off(v & 0x7FFFFFFF) + 2))
            t += 8 if is64 else 4
        print(dll, names)
    o += 20
