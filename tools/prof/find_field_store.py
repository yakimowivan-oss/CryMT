r"""Stores to a field offset in a PE's code (mov/or/and/... with a memory destination [reg + disp]), e.g. who writes a
light's camera index at +0x200: find_field_store.py <dll> <disp hex> [--loads] [--max N]
Linear sweep of executable sections (a few hits can be noise); prints the address and the instruction.
Run with tools\venv\Scripts\python.exe (capstone)."""
import os
import struct
import sys

from capstone import CS_ARCH_X86, CS_MODE_64, Cs

sys.path.insert(0, os.path.dirname(__file__))
from vfcompare import Pe  # noqa: E402

p = Pe(sys.argv[1])
disp = int(sys.argv[2], 16)
loads = '--loads' in sys.argv
limit = int(sys.argv[sys.argv.index('--max') + 1]) if '--max' in sys.argv else 200
md = Cs(CS_ARCH_X86, CS_MODE_64)
md.skipdata = True
d = p.d
pe = struct.unpack_from('<I', d, 0x3C)[0]
nsec = struct.unpack_from('<H', d, pe + 6)[0]
optsize = struct.unpack_from('<H', d, pe + 20)[0]
needle = '+ 0x%x]' % disp
shown = 0
for i in range(nsec):
    vs, va, rs, rp = struct.unpack_from('<IIII', d, pe + 24 + optsize + 40 * i + 8)
    fl = struct.unpack_from('<I', d, pe + 24 + optsize + 40 * i + 36)[0]
    if not fl & 0x20:
        continue
    for x in md.disasm(d[rp:rp + rs], p.base + va):
        ops = x.op_str
        if needle not in ops:
            continue
        first = ops.split(', ')[0]
        is_store = needle in first and x.mnemonic not in ('cmp', 'test', 'lea')
        if is_store or (loads and not is_store):
            print('%x  %s %s' % (x.address, x.mnemonic, ops))
            shown += 1
            if shown >= limit:
                sys.exit(0)
