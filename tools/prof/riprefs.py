r"""Instructions of a module that reference given absolute addresses RIP-relatively (globals, cvar fields), with the
function each lies in (logs\dump\funcs tables when present) and a few instructions around.
riprefs.py <dll> <VA hex> [<VA hex> ...] [ctx=4]
Run with tools\venv\Scripts\python.exe (capstone)."""
import glob
import os
import struct
import sys
import bisect
from capstone import CS_ARCH_X86, CS_MODE_64, Cs
from capstone.x86 import X86_REG_RIP

dll = sys.argv[1]
args = [a for a in sys.argv[2:] if '=' not in a]
opts = dict(a.split('=', 1) for a in sys.argv[2:] if '=' in a)
ctx = int(opts.get('ctx', 4))
targets = set(int(x, 16) for x in args)
d = open(dll, 'rb').read()
pe = struct.unpack_from('<I', d, 0x3C)[0]
nsec = struct.unpack_from('<H', d, pe + 6)[0]
opt = pe + 24
base = struct.unpack_from('<Q', d, opt + 24)[0]
sec = opt + struct.unpack_from('<H', d, pe + 20)[0]
md = Cs(CS_ARCH_X86, CS_MODE_64)
md.detail = True
md.skipdata = True
mod = os.path.splitext(os.path.basename(dll))[0]
funcs = []
for path in glob.glob(r'D:\Games\Crysis-RE\logs\dump\funcs\%s*.funcs.txt' % mod):
    for l in open(path):
        p = l.split()
        if len(p) >= 3:
            funcs.append((int(p[0], 16), int(p[1], 16), p[2]))
funcs.sort()
starts = [f[0] for f in funcs]
for i in range(nsec):
    s = sec + i * 40
    name = d[s:s + 8].rstrip(b'\0')
    vsize, va, rsize, raw = struct.unpack_from('<IIII', d, s + 8)
    if name != b'.text':
        continue
    code = d[raw:raw + rsize]
    insns = list(md.disasm(code, base + va))
    for k, ins in enumerate(insns):
        if ins.id == 0:  # data skipped (SKIPDATA)
            continue
        for op in ins.operands:
            if op.type == 3 and op.mem.base == X86_REG_RIP and (ins.address + ins.size + op.mem.disp) in targets:
                j = bisect.bisect_right(starts, ins.address) - 1
                fn = funcs[j][2] if j >= 0 and funcs[j][0] <= ins.address < funcs[j][0] + funcs[j][1] else '?'
                print('--- %x in %s (target %x)' % (ins.address, fn, ins.address + ins.size + op.mem.disp))
                for q in insns[max(0, k - ctx):k + ctx + 1]:
                    print('   %x: %s %s' % (q.address, q.mnemonic, q.op_str))
