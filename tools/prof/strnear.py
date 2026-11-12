r"""Printable text around each occurrence of a word in a binary (help strings of console commands):
strnear.py <file> <word> [width=200]"""
import re
import sys

d = open(sys.argv[1], 'rb').read()
word = sys.argv[2].encode()
width = int(sys.argv[3]) if len(sys.argv) > 3 else 200
for m in re.finditer(re.escape(word), d):
    a, b = max(0, m.start() - width), min(len(d), m.end() + width)
    chunk = d[a:b]
    text = ''.join(chr(c) if 32 <= c < 127 else '|' for c in chunk)
    text = re.sub(r'\|{2,}', ' || ', text)
    print('%x: %s' % (m.start(), text))
