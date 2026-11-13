import struct, sys, os, subprocess
# shaderdump.py <d3d12_psos.bin> <hash hex>...: writes each shader's bytecode (the pipeline cache's kRecordShader) to
# <out dir>/<hash>.dxbc and disassembles it with fxc /dumpbin.
path = sys.argv[1]
out = sys.argv[2]
want = {int(h, 16) for h in sys.argv[3:]}
data = open(path, 'rb').read()
at = 0
found = {}
while at + 8 <= len(data):
    tag, n = struct.unpack_from('<II', data, at)
    p = at + 8
    if p + n > len(data):
        break
    if tag == 1 and n >= 8:
        h = struct.unpack_from('<Q', data, p)[0]
        if h in want and h not in found:
            found[h] = data[p + 8:p + n]
    at = p + n
os.makedirs(out, exist_ok=True)
fxc = r'C:\Program Files (x86)\Windows Kits\10\bin\10.0.26100.0\x64\fxc.exe'
for h, code in found.items():
    f = os.path.join(out, f'{h:016x}.dxbc')
    open(f, 'wb').write(code)
    r = subprocess.run([fxc, '/nologo', '/dumpbin', f], capture_output=True, text=True)
    txt = r.stdout + r.stderr
    open(f + '.txt', 'w').write(txt)
    print(f'== {h:016x}: {len(code)} bytes')
print('missing:', [f'{h:016x}' for h in want if h not in found])
