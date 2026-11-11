r"""Find the render recursion level of a Cry3DEngine build: in .data an int -1 followed by the float 1.0 (as in Crysis 6156,
0x302008E0), and count the RIP-relative instructions that touch each candidate. The real one is read at ~100 sites.
find_level.py <Cry3DEngine.dll> [more candidate VAs in hex]
Run with tools\venv\Scripts\python.exe (capstone)."""
import struct
import sys
from collections import Counter
from capstone import CS_ARCH_X86, CS_MODE_64, Cs
from capstone.x86 import X86_OP_MEM, X86_REG_RIP

d = open(sys.argv[1], 'rb').read()
pe = struct.unpack_from('<I', d, 0x3C)[0]
nsec = struct.unpack_from('<H', d, pe + 6)[0]
optsize = struct.unpack_from('<H', d, pe + 20)[0]
base = struct.unpack_from('<Q', d, pe + 24 + 24)[0]
secs = {}
for i in range(nsec):
    name = d[pe + 24 + optsize + 40 * i: pe + 24 + optsize + 40 * i + 8].rstrip(b'\0').decode()
    vs, va, rs, rp = struct.unpack_from('<IIII', d, pe + 24 + optsize + 40 * i + 8)
    secs[name] = (va, vs, rs, rp)
va, vs, rs, rp = secs['.data']
raw = d[rp:rp + rs]
cands = set(int(x, 16) for x in sys.argv[2:])
pat = struct.pack('<if', -1, 1.0)
pos = raw.find(pat)
while pos >= 0:
    cands.add(base + va + pos)
    pos = raw.find(pat, pos + 1)
print('candidates:', ' '.join('%x' % c for c in sorted(cands)))
tva, tvs, trs, trp = secs['.text']
md = Cs(CS_ARCH_X86, CS_MODE_64)
md.detail = True
md.skipdata = True
refs = Counter()
writes = Counter()
for ins in md.disasm(d[trp:trp + trs], base + tva):
    if ins.id == 0:
        continue
    for op in ins.operands:
        if op.type == X86_OP_MEM and op.mem.base == X86_REG_RIP:
            a = ins.address + ins.size + op.mem.disp
            if a in cands:
                refs[a] += 1
                if ins.mnemonic in ('mov', 'add', 'sub', 'inc', 'dec') and ins.operands[0].type == X86_OP_MEM:
                    writes[a] += 1
for a in sorted(cands):
    print('%x: %d accesses, %d writes' % (a, refs[a], writes[a]))
