"""Find free-list pop sites that read and store a pointer through 32-bit registers in x64 DLLs."""
import glob
import os
import re
import struct
import sys

from capstone import CS_ARCH_X86, CS_MODE_64, Cs

POP = re.compile(r"^mov\s+(e[a-z0-9]+|r\d+d), dword ptr \[(r[a-z0-9]+)\]$")
STORE = re.compile(r"^mov dword ptr \[(r[a-z0-9]+) \+ (r[a-z0-9]+)\*8 \+ (0x[0-9a-f]+)\], (e[a-z0-9]+|r\d+d)$")


def main(folder):
    md = Cs(CS_ARCH_X86, CS_MODE_64)
    md.skipdata = True
    for path in sorted(glob.glob(os.path.join(folder, "*.dll")) + glob.glob(os.path.join(folder, "*.exe"))):
        d = open(path, "rb").read()
        pe = struct.unpack_from("<I", d, 0x3C)[0]
        if struct.unpack_from("<H", d, pe + 4)[0] != 0x8664:
            continue
        nsec = struct.unpack_from("<H", d, pe + 6)[0]
        optsize = struct.unpack_from("<H", d, pe + 20)[0]
        base = struct.unpack_from("<Q", d, pe + 24 + 24)[0]
        hits = []
        for i in range(nsec):
            s = pe + 24 + optsize + 40 * i
            vs, va, rs, rp, flags = (*struct.unpack_from("<IIII", d, s + 8), struct.unpack_from("<I", d, s + 36)[0])
            if not flags & 0x20:
                continue
            prev = None
            for ins in md.disasm(d[rp:rp + rs], base + va):
                text = f"{ins.mnemonic} {ins.op_str}"
                st = STORE.match(text)
                if st and prev and POP.match(prev[1]) and POP.match(prev[1]).group(1) == st.group(4):
                    hits.append((prev[0], st.group(3), prev[1], text))
                prev = (ins.address, text)
        if hits:
            print(f"{os.path.basename(path):26} {len(hits):3} pop sites  list offsets: {sorted({h[1] for h in hits})}")
            print(f"    e.g. {hits[0][0]:#x}: {hits[0][2]} ; {hits[0][3]}")


main(sys.argv[1])
