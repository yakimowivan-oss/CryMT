r"""Console variable names in one game's engine DLLs that the other lacks (strings that look like cvars), plus build stamps.
cvardiff.py <old game dir> <new game dir> [dll ...]   e.g. "D:\Games\Crysis" "D:\Games\Crysis Warhead"
Reads Bin64\<dll>; with no dll names, the usual engine modules."""
import os
import re
import struct
import sys
import time

old_dir, new_dir = sys.argv[1], sys.argv[2]
dlls = sys.argv[3:] or ['Cry3DEngine.dll', 'CryRenderD3D10.dll', 'CrySystem.dll', 'CryAnimation.dll', 'CryEntitySystem.dll',
                        'CryPhysics.dll', 'CryAction.dll', 'CryAISystem.dll', 'Crysis.exe']
PAT = re.compile(rb'(?<![A-Za-z0-9_])(?:e|r|sys|ca|es|p|ai|g|i|s|v|cl|sv|gt|log|mfx|net)_[A-Za-z][A-Za-z0-9_]{2,48}(?![A-Za-z0-9_])')


def stamp(path):
    d = open(path, 'rb').read(4096)
    pe = struct.unpack_from('<I', d, 0x3C)[0]
    return time.strftime('%Y-%m-%d', time.gmtime(struct.unpack_from('<I', d, pe + 8)[0]))


def names(path):
    d = open(path, 'rb').read()
    return {m.group().decode() for m in PAT.finditer(d)}


allold, allnew = set(), set()
for dll in dlls:
    po, pn = os.path.join(old_dir, 'Bin64', dll), os.path.join(new_dir, 'Bin64', dll)
    if not os.path.exists(po) or not os.path.exists(pn):
        print('%-22s %s' % (dll, 'missing in ' + ('old' if not os.path.exists(po) else 'new')))
        if os.path.exists(pn):
            allnew |= names(pn)
        if os.path.exists(po):
            allold |= names(po)
        continue
    a, b = names(po), names(pn)
    allold |= a
    allnew |= b
    print('%-22s old %s new %s: %d cvar-like names new, %d gone' % (dll, stamp(po), stamp(pn), len(b - a), len(a - b)))
print()
print('new in', new_dir, '(all modules):')
for n in sorted(allnew - allold):
    print('  ', n)
print()
print('gone:')
for n in sorted(allold - allnew):
    print('  ', n)
