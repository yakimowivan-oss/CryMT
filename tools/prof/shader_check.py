"""Compiles an embedded HLSL shader of CryMT offline with fxc (the back end compiles them only at run time).
  python tools/prof/shader_check.py kGiShader trace filter motion ...   (entry points; cs_5_0 unless -t gives a target)
  python tools/prof/shader_check.py all                                 (every shader in src/crymt/shaders with its entries)
The shaders live in src/crymt/shaders/*.hlsl as C++ raw string literals (R"CRYMT( ... )CRYMT"), included by
D3D12Backend.cpp / BendSss.h; a constant still written inline in D3D12Backend.cpp is read from there.
Prints each entry's result and fxc's errors; exit code 1 when one fails."""
import os, re, subprocess, sys, tempfile

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), '..', '..'))
SRC = os.path.join(ROOT, 'src', 'crymt')
FXC = r'C:\Program Files (x86)\Windows Kits\10\bin\10.0.26100.0\x64\fxc.exe'

# Every shader, its entry points and target, as the back end compiles them.
ALL = {
    'kGiShader': (['trace', 'filter', 'motion', 'upsample', 'camera', 'exposure', 'sanitize', 'sanitizeOut', 'gtao', 'gtaoBlur',
                   'gtaoApply', 'pvolume', 'envmap', 'probes'], 'cs_5_0'),
    'kVoxShader': (['update', 'view', 'downsample', 'light', 'roof', 'inject'], 'cs_5_0'),
    'kVoxGeomShader': (['voxelize'], 'ps_5_0'),
    'kVfogShader': (['vfog'], 'cs_5_0'),
    'kHizShader': (['hizInit', 'hizDown'], 'cs_5_0'),
    'kCubeShader': (['cubeDecode', 'cubeDown', 'cubePrefilter'], 'cs_5_0'),
    'kEnv2DShader': (['env2d'], 'cs_5_0'),
    'kDepthAlignShader': (['AlignDepth'], 'cs_5_0'),
    'kBendSssShader': (['main'], 'cs_5_0'),
}

def extract(name):
    for f in ('D3D12Backend.cpp', 'BendSss.h'):
        src = open(os.path.join(SRC, f), encoding='utf-8').read()
        m = re.search(r'const char\* const ' + re.escape(name) + r' =\s*#include "([^"]+)"', src)
        if m:
            text = open(os.path.join(SRC, m.group(1)), encoding='utf-8').read()
            return text[text.index('R"CRYMT(') + len('R"CRYMT('):text.rindex(')CRYMT"')] + '\n'
        m = re.search(r'const char\* const ' + re.escape(name) + r' = R"\((.*?)\n\)";', src, re.S)
        if m:
            return m.group(1) + '\n'
    sys.exit('no shader ' + name)

def check(name, entries, target):
    path = os.path.join(tempfile.gettempdir(), 'crymt_' + name + '.hlsl')
    open(path, 'w', encoding='utf-8').write(extract(name))
    bad = 0
    for e in entries:
        r = subprocess.run([FXC, '/nologo', '/T', target, '/E', e, '/O3', '/Fo', os.devnull, path], capture_output=True, text=True)
        errors = [l for l in (r.stdout + r.stderr).splitlines() if 'error' in l]
        print(('ok   ' if r.returncode == 0 else 'FAIL ') + name + ' ' + e)
        for l in errors[:8]:
            print('     ' + l.replace(path, name))
        bad += r.returncode != 0
    return bad

def main():
    args = sys.argv[1:]
    target = 'cs_5_0'
    if '-t' in args:
        i = args.index('-t')
        target = args[i + 1]
        del args[i:i + 2]
    if args and args[0] == 'all':
        bad = sum(check(n, e, t) for n, (e, t) in ALL.items())
    else:
        bad = check(args[0], args[1:], target)
    sys.exit(1 if bad else 0)

main()
