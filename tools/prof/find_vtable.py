"""Find vtables of an MSVC class by RTTI name in an x64 PE: find_vtable.py <dll> <decorated name>"""
import struct, sys
d = open(sys.argv[1], "rb").read(); name = sys.argv[2].encode()
pe = struct.unpack_from("<I", d, 0x3C)[0]; nsec = struct.unpack_from("<H", d, pe + 6)[0]; optsize = struct.unpack_from("<H", d, pe + 20)[0]
base = struct.unpack_from("<Q", d, pe + 24 + 24)[0]
secs = [(struct.unpack_from("<IIII", d, pe + 24 + optsize + 40 * i + 8), struct.unpack_from("<I", d, pe + 24 + optsize + 40 * i + 36)[0]) for i in range(nsec)]
def rva2off(r): return next(r - va + rp for (vs, va, rs, rp), fl in secs if va <= r < va + max(vs, rs))
def off2rva(o): return next(o - rp + va for (vs, va, rs, rp), fl in secs if rp <= o < rp + rs)
text = [(va, va + vs) for (vs, va, rs, rp), fl in secs if fl & 0x20]
td = d.find(b"\0" + name + b"\0") + 1
td_rva = off2rva(td - 16)
print("type descriptor rva", hex(td_rva))
# complete object locators: signature 1, then offset, cdOffset, pTypeDescriptor (rva)
for (vs, va, rs, rp), fl in secs:
    for o in range(rp, rp + rs - 24, 4):
        sig, off, cd, ptd = struct.unpack_from("<IIII", d, o)
        if sig in (0, 1) and ptd == td_rva:  # VS2005 x64 builds (Crysis 6156) write signature 0
            col_va = base + off2rva(o)
            for (vs2, va2, rs2, rp2), fl2 in secs:
                for o2 in range(rp2, rp2 + rs2 - 8, 8):
                    if struct.unpack_from("<Q", d, o2)[0] == col_va:
                        vt = o2 + 8; n = 0; funcs = []
                        while True:
                            f = struct.unpack_from("<Q", d, vt + 8 * n)[0] - base
                            if not any(a <= f < b for a, b in text): break
                            funcs.append(f); n += 1
                        print(f"COL offset={off} vtable va={base + off2rva(vt):#x} slots={n}")
                        print(" ".join(f"{i}:{x:X}" for i, x in enumerate(funcs)))
