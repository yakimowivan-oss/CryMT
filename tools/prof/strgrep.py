r"""Printable strings of a binary matching a regex (cvar help texts, log messages): strgrep.py <file> <regex> [max=60]"""
import re
import sys

d = open(sys.argv[1], 'rb').read()
rx = re.compile(sys.argv[2], re.I)
n = 0
for m in re.finditer(rb'[\x20-\x7e\n\t]{6,}', d):
    s = m.group().decode()
    if rx.search(s):
        print(repr(s[:300]))
        n += 1
        if n >= (int(sys.argv[3]) if len(sys.argv) > 3 else 60):
            break
