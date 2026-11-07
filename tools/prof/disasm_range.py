"""Disassemble a VA range: disasm_range.py <dll> <startVA> <endVA>"""
import struct, sys
from capstone import CS_ARCH_X86, CS_MODE_64, Cs
d = open(sys.argv[1], "rb").read()
pe = struct.unpack_from("<I", d, 0x3C)[0]; nsec = struct.unpack_from("<H", d, pe + 6)[0]; optsize = struct.unpack_from("<H", d, pe + 20)[0]
base = struct.unpack_from("<Q", d, pe + 24 + 24)[0]
secs = [struct.unpack_from("<IIII", d, pe + 24 + optsize + 40 * i + 8) for i in range(nsec)]
a, b = int(sys.argv[2], 16), int(sys.argv[3], 16)
off = next(a - base - va + rp for vs, va, rs, rp in secs if va <= a - base < va + max(vs, rs))
for ins in Cs(CS_ARCH_X86, CS_MODE_64).disasm(d[off:off + (b - a)], a):
    print(f"{ins.address:#x}: {ins.mnemonic} {ins.op_str}")
