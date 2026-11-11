r"""Printable strings (ASCII, 6+ chars) one game's module has and another's lacks: new log messages, profiler labels, asserts.
strdiff.py <old game dir> <new game dir> <dll> [filter regex]   e.g. "D:\Games\Crysis" "D:\Games\Crysis Warhead" Cry3DEngine.dll
Prints the strings only the new one has (and with -gone, only the old one)."""
import os
import re
import sys

args = [a for a in sys.argv[1:] if a != '-gone']
gone = '-gone' in sys.argv
old_dir, new_dir, dll = args[:3]
flt = re.compile(args[3], re.I) if len(args) > 3 else None
PAT = re.compile(rb'[\x20-\x7e]{6,}')


def strings(path):
    d = open(path, 'rb').read()
    return {m.group().decode() for m in PAT.finditer(d)}


a = strings(os.path.join(old_dir, 'Bin64', dll))
b = strings(os.path.join(new_dir, 'Bin64', dll))
diff = sorted((a - b) if gone else (b - a))
# drop noise: long runs of hex/base64-like data and mangled names
diff = [s for s in diff if not re.fullmatch(r'[A-Za-z0-9+/=]{6,}', s) and not s.startswith(('?', '.?A'))]
if flt:
    diff = [s for s in diff if flt.search(s)]
for s in diff:
    print(s)
print('--', len(diff), 'strings', 'only in old' if gone else 'only in new')
