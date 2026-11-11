r"""Slot by slot diff of a class's virtual functions between Crysis and another build (Warhead, Wars): for each slot the
mnemonic fingerprint; for changed slots the structural difference (hunks whose instructions differ beyond registers,
offsets and addresses), so real logic changes stand out from recompilation noise.
vtdiff.py <dll name> <class> <other game dir> [--slots=80] [--show=6 changed slots to print]
Run with tools\venv\Scripts\python.exe (capstone)."""
import difflib
import os
import re
import sys
from capstone import CS_ARCH_X86, CS_MODE_64, Cs

sys.path.insert(0, os.path.dirname(__file__))
from vfcompare import Pe  # noqa: E402

args = [a for a in sys.argv[1:] if not a.startswith('--')]
opts = dict(a[2:].split('=') for a in sys.argv[1:] if a.startswith('--'))
dll, cls, other = args[:3]
max_slots = int(opts.get('slots', '80'))
show = int(opts.get('show', '6'))
md = Cs(CS_ARCH_X86, CS_MODE_64)


def body(p, f):
    end = p.func_end(f)
    size = (end - f) if end else 0
    code = p.d[p.off(f):p.off(f) + (size or 256)]
    out = []
    for x in md.disasm(code, p.base + f):
        out.append('%s %s' % (x.mnemonic, x.op_str))
        if not size and x.mnemonic in ('ret', 'jmp'):
            break
    return out, size


def norm(line):
    line = re.sub(r'0x[0-9a-f]+', 'N', line)
    line = re.sub(r'\b(r[a-z0-9]+|e[a-z]{2}|[a-d]l|[re]?[sd]il?|[re]?[sb]pl?|xmm\d+)\b', 'R', line)
    return line


a = Pe(r'D:\Games\Crysis\Bin64\%s' % dll)
b = Pe(os.path.join(other, 'Bin64', dll))
va, vb = a.vtable(cls), b.vtable(cls)
print('%s: Crysis vtable %s, other %s' % (cls, va and '%x' % (a.base + va), vb and '%x' % (b.base + vb)))
if va is None or vb is None:
    sys.exit(0)
changed = 0
for i in range(max_slots):
    fa, fb = a.slot(va, i), b.slot(vb, i)
    if not a.is_text(fa) or not b.is_text(fb):
        break
    ia, sa = body(a, fa)
    ib, sb = body(b, fb)
    same = [norm(x) for x in ia] == [norm(x) for x in ib]
    print('  slot %2d %x %5d | %x %5d  %s' % (i, a.base + fa, sa, b.base + fb, sb, 'same' if same else 'CHANGED'))
    if same or changed >= show:
        continue
    changed += 1
    na, nb = [norm(x) for x in ia], [norm(x) for x in ib]
    sm = difflib.SequenceMatcher(None, na, nb, autojunk=False)
    for tag, i1, i2, j1, j2 in sm.get_opcodes():
        if tag == 'equal':
            continue
        print('      %s crysis[%d:%d] other[%d:%d]' % (tag, i1, i2, j1, j2))
        for x in ia[i1:min(i2, i1 + 12)]:
            print('        - ' + x)
        for x in ib[j1:min(j2, j1 + 12)]:
            print('        + ' + x)
