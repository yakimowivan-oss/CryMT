r"""Vtables of classes by RTTI name, with the bodies of short slots (getters show their field offsets).
vtdump.py <dll path> <class> [<class> ...] [--max=24 bytes of a short body] [--slots=N]
Run with tools\venv\Scripts\python.exe (capstone)."""
import os
import sys
from capstone import CS_ARCH_X86, CS_MODE_64, Cs

sys.path.insert(0, os.path.dirname(__file__))
from vfcompare import Pe  # noqa: E402

args = [a for a in sys.argv[1:] if not a.startswith('--')]
opts = dict(a[2:].split('=') for a in sys.argv[1:] if a.startswith('--'))
short = int(opts.get('max', '40'))
max_slots = int(opts.get('slots', '80'))
p = Pe(args[0])
md = Cs(CS_ARCH_X86, CS_MODE_64)
for cls in args[1:]:
    vt = p.vtable(cls)
    print('== %s vtable %s' % (cls, '%x' % (p.base + vt) if vt is not None else 'not found'))
    if vt is None:
        continue
    for i in range(max_slots):
        f = p.slot(vt, i)
        if not p.is_text(f):
            break
        end = p.func_end(f)
        size = (end - f) if end else 0
        body = ''
        if 0 < size <= short:
            body = '; '.join('%s %s' % (x.mnemonic, x.op_str) for x in md.disasm(p.d[p.off(f):p.off(f) + size], p.base + f))
        elif size == 0:  # a leaf without .pdata: up to its first ret
            parts = []
            for x in md.disasm(p.d[p.off(f):p.off(f) + 64], p.base + f):
                parts.append('%s %s' % (x.mnemonic, x.op_str))
                if x.mnemonic in ('ret', 'jmp'):
                    break
            body = 'leaf: ' + '; '.join(parts)
        print('  %2d %x %4d  %s' % (i, p.base + f, size, body))
