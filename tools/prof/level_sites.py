r"""List every access to Cry3DEngine's global render recursion level 0x302008E0, grouped by Ghidra function."""
import bisect
import struct
from capstone import CS_ARCH_X86, CS_MODE_64, Cs
from capstone.x86 import X86_OP_MEM, X86_REG_RIP

DLL = r'D:\Games\Crysis\Bin64\Cry3DEngine.dll'
TARGET = 0x302008E0
d = open(DLL, 'rb').read()
pe = struct.unpack_from('<I', d, 0x3C)[0]
nsec = struct.unpack_from('<H', d, pe + 6)[0]
optsize = struct.unpack_from('<H', d, pe + 20)[0]
base = struct.unpack_from('<Q', d, pe + 24 + 24)[0]
rows = []
for l in open(r'D:\Games\Crysis-RE\logs\dump\funcs\Cry3DEngine.dll.funcs.txt'):
    p = l.split()
    if len(p) >= 3:
        rows.append((int(p[0], 16), int(p[1], 16), p[2]))
rows.sort()
starts = [r[0] for r in rows]


def fname(va):
    i = bisect.bisect_right(starts, va) - 1
    return '%x %s' % (rows[i][0], rows[i][2]) if i >= 0 else '?'


md = Cs(CS_ARCH_X86, CS_MODE_64)
md.detail = True
md.skipdata = True
out = {}
for i in range(nsec):
    name = d[pe + 24 + optsize + 40 * i: pe + 24 + optsize + 40 * i + 8].rstrip(b'\0')
    vs, va, rs, rp = struct.unpack_from('<IIII', d, pe + 24 + optsize + 40 * i + 8)
    if name != b'.text':
        continue
    # disassemble function by function so a misaligned decode does not hide sites
    for fs, fsz, fn in rows:
        o = fs - base - va + rp
        if not (rp <= o < rp + rs):
            continue
        for ins in md.disasm(d[o:o + max(fsz, 1)], fs):
            if ins.id == 0:
                continue
            for op in ins.operands:
                if op.type == X86_OP_MEM and op.mem.base == X86_REG_RIP and ins.address + ins.size + op.mem.disp == TARGET:
                    w = ins.mnemonic in ('mov', 'add', 'sub', 'inc', 'dec') and ins.operands[0].type == X86_OP_MEM
                    out.setdefault(fname(ins.address), []).append('%x %s%s %s' % (ins.address, 'W ' if w else '', ins.mnemonic, ins.op_str))
n = 0
for f in sorted(out):
    print(f)
    for s in out[f]:
        print('   ', s)
        n += 1
print('sites', n, 'functions', len(out))
