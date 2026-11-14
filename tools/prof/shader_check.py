"""Compiles an embedded HLSL shader of D3D12Backend.cpp offline with fxc (the back end compiles them only at run time).
  python tools/prof/shader_check.py kGiShader trace filter motion ...   (entry points; cs_5_0 unless -t gives a target)
Prints each entry's result and fxc's errors; exit code 1 when one fails."""
import os, re, subprocess, sys, tempfile

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), '..', '..'))
FXC = r'C:\Program Files (x86)\Windows Kits\10\bin\10.0.26100.0\x64\fxc.exe'

def extract(name):
    src = open(os.path.join(ROOT, 'src', 'crymt', 'D3D12Backend.cpp'), encoding='utf-8').read()
    m = re.search(r'const char\* const ' + re.escape(name) + r' = R"\((.*?)\n\)";', src, re.S)
    if not m:
        sys.exit('no raw string ' + name)
    return m.group(1) + '\n'

def main():
    args = sys.argv[1:]
    target = 'cs_5_0'
    if '-t' in args:
        i = args.index('-t')
        target = args[i + 1]
        del args[i:i + 2]
    name, entries = args[0], args[1:]
    path = os.path.join(tempfile.gettempdir(), 'crymt_' + name + '.hlsl')
    open(path, 'w', encoding='utf-8').write(extract(name))
    bad = 0
    for e in entries:
        r = subprocess.run([FXC, '/nologo', '/T', target, '/E', e, '/O3', '/Fo', os.devnull, path],
                           capture_output=True, text=True)
        errors = [l for l in (r.stdout + r.stderr).splitlines() if 'error' in l]
        print(('ok   ' if r.returncode == 0 else 'FAIL ') + e)
        for l in errors[:8]:
            print('     ' + l.replace(path, name))
        bad += r.returncode != 0
    sys.exit(1 if bad else 0)

main()
