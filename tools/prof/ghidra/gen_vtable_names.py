"""Writes V lines for ApplyNames.java: Class::vfN for every RTTI vtable slot of the given DLLs.
gen_vtable_names.py <out file> <dll> [<dll>...]  (uses all_vtables.py's locator scan)"""
import os
import struct
import subprocess
import sys
import tempfile

here = os.path.dirname(os.path.abspath(__file__))
finder = os.path.join(here, "..", "all_vtables.py")


def demangle(name):
    # .?AVCRenderMesh@@ / .?AUSFoo@Ns@@ -> CRenderMesh / Ns::SFoo
    body = name[4:].rstrip("@")
    parts = [p for p in body.split("@") if p]
    return "::".join(reversed(parts)) if parts and all(p.isidentifier() for p in parts) else None


out = open(sys.argv[1], "w")
for dll in sys.argv[2:]:
    d = open(dll, "rb").read()
    pe = struct.unpack_from("<I", d, 0x3C)[0]
    nsec = struct.unpack_from("<H", d, pe + 6)[0]
    optsize = struct.unpack_from("<H", d, pe + 20)[0]
    base = struct.unpack_from("<Q", d, pe + 24 + 24)[0]
    secs = [struct.unpack_from("<IIII", d, pe + 24 + optsize + 40 * i + 8) for i in range(nsec)]

    def read64(va):
        r = va - base
        for vs, v, rs, rp in secs:
            if v <= r < v + rs:
                return struct.unpack_from("<Q", d, r - v + rp)[0]

    tmp = tempfile.mktemp(suffix=".txt")
    subprocess.run([sys.executable, "-I", finder, dll, tmp], check=True, stdout=subprocess.DEVNULL)
    module = os.path.basename(dll)
    out.write(f"# {module}\n")
    count = 0
    # Fewer slots first, so an inherited function that is not overridden keeps its base class name.
    for vt, n, name, off in sorted((l.split() for l in open(tmp)), key=lambda f: int(f[1])):
        cls = demangle(name)
        if not cls or off != "0":  # secondary vtables of multiple inheritance keep the primary's names
            continue
        for s in range(int(n)):
            fn = read64(int(vt, 16) + 8 * s)
            out.write(f"V {module} {fn:x} {cls}::vf{s}\n")
            count += 1
    os.remove(tmp)
    print(module, "slots", count)
