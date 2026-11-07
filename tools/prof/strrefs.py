# RIP-relative references (lea/mov reg, [rip+disp32]) to a string in a PE image: which code uses it.
# Usage: python strrefs.py <dll> <string>
import struct
import sys

path, text = sys.argv[1], sys.argv[2].encode() + b'\0'
d = open(path, 'rb').read()
pe = struct.unpack_from('<I', d, 0x3C)[0]
base = struct.unpack_from('<Q', d, pe + 24 + 24)[0]
count = struct.unpack_from('<H', d, pe + 6)[0]
opt = struct.unpack_from('<H', d, pe + 20)[0]
sections = []
for i in range(count):
    o = pe + 24 + opt + i * 40
    name = d[o:o + 8].rstrip(b'\0').decode()
    vsize, va, rsize, raw = struct.unpack_from('<IIII', d, o + 8)
    sections.append((name, va, vsize, raw, rsize))


def rva_of(off):
    for name, va, vsize, raw, rsize in sections:
        if raw <= off < raw + rsize:
            return va + off - raw


targets = []
start = 0
while (at := d.find(text, start)) >= 0:
    if at == 0 or d[at - 1] == 0:
        targets.append(rva_of(at))
    start = at + 1
print('string at RVAs', [hex(base + t) for t in targets if t is not None])
for name, va, vsize, raw, rsize in sections:
    if name != '.text':
        continue
    code = d[raw:raw + rsize]
    for i in range(len(code) - 7):
        if code[i] in (0x48, 0x4C) and code[i + 1] in (0x8D, 0x8B) and (code[i + 2] & 0xC7) == 0x05:
            disp = struct.unpack_from('<i', code, i + 3)[0]
            target = va + i + 7 + disp
            if target in targets:
                print('ref at', hex(base + va + i))
