"""Find direct call/jmp sites and vtable slots that target given functions: callsites.py <dll> <hexVA>..."""
import struct, sys
from capstone import CS_ARCH_X86, CS_MODE_64, Cs
d = open(sys.argv[1], "rb").read()
targets = {int(a, 16) for a in sys.argv[2:]}
pe = struct.unpack_from("<I", d, 0x3C)[0]; nsec = struct.unpack_from("<H", d, pe + 6)[0]; optsize = struct.unpack_from("<H", d, pe + 20)[0]
opt = pe + 24; base = struct.unpack_from("<Q", d, opt + 24)[0]
pr, ps = struct.unpack_from("<II", d, opt + 112 + 3 * 8)
secs = [(struct.unpack_from("<IIII", d, opt + optsize + 40 * i + 8), struct.unpack_from("<I", d, opt + optsize + 40 * i + 36)[0]) for i in range(nsec)]
def off(r): return next(r - va + rp for (vs, va, rs, rp), fl in secs if va <= r < va + max(vs, rs))
md = Cs(CS_ARCH_X86, CS_MODE_64)
found = {t: [] for t in targets}
for i in range(0, ps, 12):
    b, e, _ = struct.unpack_from("<III", d, off(pr) + i)
    for ins in md.disasm(d[off(b):off(b) + (e - b)], base + b):
        if ins.mnemonic in ("call", "jmp") and ins.op_str.startswith("0x"):
            t = int(ins.op_str, 16)
            if t in targets and ins.size == 5:
                found[t].append((ins.mnemonic, ins.address))
for (vs, va, rs, rp), fl in secs:
    if fl & 0x20: continue
    for o in range(rp, rp + rs - 8, 8):
        v = struct.unpack_from("<Q", d, o)[0]
        if v in targets:
            found[v].append(("slot", base + o - rp + va))
for t, sites in found.items():
    print(f"{t:#x}: " + ", ".join(f"{k} {a:#x}" for k, a in sorted(set(sites), key=lambda x: x[1])))
