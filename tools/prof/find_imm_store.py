r"""Instructions in a PE's code with a given immediate and memory displacement, e.g. who sets ERF_HIDDEN (0x100) in
render node flags: find_imm_store.py <dll> <imm hex> <disp hex>[,<disp hex>...]  (also ~imm for and-masks)
Run with tools\venv\Scripts\python.exe (capstone). Linear sweep of executable sections, so a few hits can be noise."""
import os
import struct
import sys
from capstone import CS_ARCH_X86, CS_MODE_64, Cs

sys.path.insert(0, os.path.dirname(__file__))
from vfcompare import Pe  # noqa: E402

p = Pe(sys.argv[1])
imm = int(sys.argv[2], 16)
disps = [int(x, 16) for x in sys.argv[3].split(',')]
want = {'0x%x' % imm, '0x%x' % (~imm & 0xffffffff), '-0x%x' % (imm + 1)}
md = Cs(CS_ARCH_X86, CS_MODE_64)
md.skipdata = True
counts = {}
# executable sections from the PE header
d = p.d
pe = struct.unpack_from('<I', d, 0x3C)[0]
nsec = struct.unpack_from('<H', d, pe + 6)[0]
optsize = struct.unpack_from('<H', d, pe + 20)[0]
for i in range(nsec):
    vs, va, rs, rp = struct.unpack_from('<IIII', d, pe + 24 + optsize + 40 * i + 8)
    fl = struct.unpack_from('<I', d, pe + 24 + optsize + 40 * i + 36)[0]
    if not fl & 0x20:
        continue
    for x in md.disasm(d[rp:rp + rs], p.base + va):
        ops = x.op_str
        if not any(w in ops.split(', ')[-1] for w in want):
            continue
        for disp in disps:
            if '+ 0x%x]' % disp in ops:
                key = (disp, x.mnemonic, ops.split(', ')[-1])
                counts[key] = counts.get(key, 0) + 1
                if counts[key] <= 3:
                    print('%x  %s %s' % (x.address, x.mnemonic, ops))
for k, v in sorted(counts.items()):
    print('disp 0x%x %s %s: %d' % (k[0], k[1], k[2], v))
