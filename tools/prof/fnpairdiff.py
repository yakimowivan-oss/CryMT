r"""Diff one function between two builds (Crysis vs Warhead / Wars). The function is found in each build as the .pdata
function that references a string (lea of its address), e.g. a profiler label; its instructions are normalized (addresses,
large immediates and displacements to placeholders) and diffed with difflib.
fnpairdiff.py <dll name> <string> <other game dir> [context=3]
Run with tools\venv\Scripts\python.exe (capstone)."""
import difflib
import re
import struct
import sys
from capstone import CS_ARCH_X86, CS_MODE_64, Cs
from capstone.x86 import X86_OP_MEM, X86_REG_RIP

dll, text, other = sys.argv[1], sys.argv[2].encode(), sys.argv[3]
ctx = int(sys.argv[4]) if len(sys.argv) > 4 else 3
md = Cs(CS_ARCH_X86, CS_MODE_64)
md.detail = True
md.skipdata = True


def load(path):
    d = open(path, 'rb').read()
    pe = struct.unpack_from('<I', d, 0x3C)[0]
    nsec = struct.unpack_from('<H', d, pe + 6)[0]
    optsize = struct.unpack_from('<H', d, pe + 20)[0]
    opt = pe + 24
    base = struct.unpack_from('<Q', d, opt + 24)[0]
    pr, ps = struct.unpack_from('<II', d, opt + 112 + 3 * 8)
    secs = [struct.unpack_from('<IIII', d, opt + optsize + 40 * i + 8) for i in range(nsec)]
    names = [d[opt + optsize + 40 * i: opt + optsize + 40 * i + 8].rstrip(b'\0') for i in range(nsec)]
    return d, base, pr, ps, secs, names


def off(secs, r):
    return next(r - va + rp for vs, va, rs, rp in secs if va <= r < va + max(vs, rs))


def rva(secs, o):
    return next(o - rp + va for vs, va, rs, rp in secs if rp <= o < rp + rs)


def function_with_string(path):
    d, base, pr, ps, secs, names = load(path)
    s = d.find(b'\0' + text + b'\0') + 1
    if s <= 0:
        return None
    target = base + rva(secs, s)
    i = names.index(b'.text')
    vs, va, rs, rp = secs[i]
    hits = []
    for ins in md.disasm(d[rp:rp + rs], base + va):
        if ins.id == 0 or ins.mnemonic != 'lea':
            continue
        op = ins.operands[1]
        if op.type == X86_OP_MEM and op.mem.base == X86_REG_RIP and ins.address + ins.size + op.mem.disp == target:
            hits.append(ins.address - base)
    funcs = []
    o = off(secs, pr)
    for k in range(0, ps, 12):
        b, e, _ = struct.unpack_from('<III', d, o + k)
        if any(b <= h < e for h in hits):
            funcs.append((b, e))
    funcs.sort()
    lines = []
    for b, e in funcs:
        lines.append('== chunk %x' % (base + b))
        for ins in md.disasm(d[off(secs, b):off(secs, e)], base + b):
            t = '%s %s' % (ins.mnemonic, ins.op_str)
            t = re.sub(r'0x[0-9a-f]{5,}', 'ADDR', t)  # code/data addresses and rip displacements
            lines.append(t)
    return lines


a = function_with_string(r'D:\Games\Crysis\Bin64\%s' % dll)
b = function_with_string(r'%s\Bin64\%s' % (other, dll))
if a is None or b is None:
    sys.exit('string not found in one of the builds')
print('Crysis: %d instructions, other: %d' % (len(a), len(b)))
for line in difflib.unified_diff(a, b, 'crysis', 'other', n=ctx, lineterm=''):
    print(line)
