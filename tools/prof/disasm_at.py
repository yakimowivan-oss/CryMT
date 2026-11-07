"""Disassemble the whole function (from .pdata) containing each VA: disasm_at.py <dll> <va> [<va>...]"""
import struct
import sys

from capstone import CS_ARCH_X86, CS_MODE_64, Cs


def load(path):
    d = open(path, "rb").read()
    pe = struct.unpack_from("<I", d, 0x3C)[0]
    nsec = struct.unpack_from("<H", d, pe + 6)[0]
    optsize = struct.unpack_from("<H", d, pe + 20)[0]
    opt = pe + 24
    base = struct.unpack_from("<Q", d, opt + 24)[0]
    pdata_rva, pdata_size = struct.unpack_from("<II", d, opt + 112 + 3 * 8)
    secs = []
    for i in range(nsec):
        vs, va, rs, rp = struct.unpack_from("<IIII", d, opt + optsize + 40 * i + 8)
        secs.append((va, max(vs, rs), rp))
    return d, base, pdata_rva, pdata_size, secs


def off(secs, rva):
    for va, size, raw in secs:
        if va <= rva < va + size:
            return rva - va + raw
    raise ValueError(hex(rva))


def main():
    d, base, prva, psize, secs = load(sys.argv[1])
    funcs = [struct.unpack_from("<III", d, off(secs, prva) + i) for i in range(0, psize, 12)]
    md = Cs(CS_ARCH_X86, CS_MODE_64)
    for arg in sys.argv[2:]:
        va = int(arg, 16)
        rva = va - base
        fn = next((f for f in funcs if f[0] <= rva < f[1]), None)
        if not fn:
            print(f"no pdata entry for {va:#x}")
            continue
        print(f"\n=== {va:#x} in function {base + fn[0]:#x}..{base + fn[1]:#x} ({fn[1] - fn[0]} bytes)")
        code = d[off(secs, fn[0]):off(secs, fn[0]) + (fn[1] - fn[0])]
        for ins in md.disasm(code, base + fn[0]):
            mark = "==>" if ins.address == va else "   "
            print(f"{mark} {ins.address:#x}: {ins.mnemonic} {ins.op_str}")


main()
