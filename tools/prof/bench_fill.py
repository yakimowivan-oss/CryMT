# Puts the results of preset_bench.ps1 (logs\preset_bench.jsonl) into the launcher's options.js (BENCH.fps) and prints
# the table and the GPU cost of DLAA and GI from the GPU test (frame time differences).
#   python tools\prof\bench_fill.py [results.jsonl]
import io
import json
import os
import re
import sys

root = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
src = sys.argv[1] if len(sys.argv) > 1 else os.path.join(root, 'logs', 'preset_bench.jsonl')
fps = {}
for line in io.open(src, encoding='utf-8-sig'):
    line = line.strip()
    if not line:
        continue
    r = json.loads(line)
    if r.get('ok') and r.get('avg'):
        fps.setdefault(r['preset'], {})[r['test']] = [round(r['avg'], 1), round(r['min'], 1)]

presets = ['vanilla', 'mt', 'dx12', 'dlaa', 'full']
tests = ['cpu2', 'gpu', 'village', 'harbor']
print('%-8s' % '' + ''.join('%16s' % t for t in tests))
for p in presets:
    row = fps.get(p, {})
    print('%-8s' % p + ''.join('%16s' % ('%.1f / %.1f' % tuple(row[t]) if t in row else '-') for t in tests))


def ms(a, b, test='gpu'):
    if test in fps.get(a, {}) and test in fps.get(b, {}):
        return 1000.0 / fps[b][test][0] - 1000.0 / fps[a][test][0]
    return None


for name, a, b in [('DLAA', 'dx12', 'dlaa'), ('GI (all of it)', 'dlaa', 'full')]:
    for test in tests:
        v = ms(a, b, test)
        if v is not None:
            print('%-15s %-8s %+.2f ms' % (name, test, v))

path = os.path.join(root, 'src', 'launcher', 'ui', 'options.js')
s = io.open(path, encoding='utf-8').read()
data = json.dumps(fps, separators=(', ', ': '))
s2, n = re.subn(r'fps: \{.*\}, // BENCH-DATA', 'fps: ' + data.replace('\\', '\\\\') + ', // BENCH-DATA', s)
assert n == 1, n
io.open(path, 'w', encoding='utf-8').write(s2)
print('options.js: BENCH.fps written (%d presets)' % len(fps))
