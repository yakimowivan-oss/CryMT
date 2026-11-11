r"""Where a module keeps an int/float console variable: the instructions around each reference to the cvar's name string
(the registration passes the field's address next to the name, e.g. lea r8, [rbx+0x2fc]).
cvarfield.py <dll> <cvar name> [window=6]
Run with tools\venv\Scripts\python.exe (capstone)."""
import struct
import sys
from capstone import CS_ARCH_X86, CS_MODE_64, Cs
from capstone.x86 import X86_OP_MEM, X86_REG_RIP

d = open(sys.argv[1], 'rb').read()
name = sys.argv[2].encode()
win = int(sys.argv[3]) if len(sys.argv) > 3 else 6
pe = struct.unpack_from('<I', d, 0x3C)[0]
nsec = struct.unpack_from('<H', d, pe + 6)[0]
optsize = struct.unpack_from('<H', d, pe + 20)[0]
base = struct.unpack_from('<Q', d, pe + 24 + 24)[0]
secs = []
for i in range(nsec):
    nm = d[pe + 24 + optsize + 40 * i: pe + 24 + optsize + 40 * i + 8].rstrip(b'\0')
    vs, va, rs, rp = struct.unpack_from('<IIII', d, pe + 24 + optsize + 40 * i + 8)
    secs.append((nm, va, vs, rs, rp))
s = d.find(b'\0' + name + b'\0') + 1
target = next(base + va + (s - rp) for nm, va, vs, rs, rp in secs if rp <= s < rp + rs)
md = Cs(CS_ARCH_X86, CS_MODE_64)
md.detail = True
md.skipdata = True
nm, va, vs, rs, rp = next(x for x in secs if x[0] == b'.text')
window = []
after = 0
for ins in md.disasm(d[rp:rp + rs], base + va):
    if ins.id == 0:
        continue
    window.append(ins)
    window = window[-win:]
    if after:
        print('   %x: %s %s' % (ins.address, ins.mnemonic, ins.op_str))
        after -= 1
        continue
    for op in ins.operands:
        if op.type == X86_OP_MEM and op.mem.base == X86_REG_RIP and ins.address + ins.size + op.mem.disp == target:
            print('--- reference at %x' % ins.address)
            for w in window:
                print('   %x: %s %s' % (w.address, w.mnemonic, w.op_str))
            after = win
            break
