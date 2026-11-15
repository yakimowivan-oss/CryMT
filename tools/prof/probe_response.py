"""Compares GI v2 probe variants from tools\\prof\\probe_response.ps1's captures (logs\\spot\\<tag>_probe_response.txt):
noise = the mean change of the GI light between the three captures with nothing changing (relative to its level), and
the response = the GI light after the lamp went on at each capture, as a share of the last.
  python tools/prof/probe_response.py [tag]"""
import sys
import numpy as np

tag = sys.argv[1] if len(sys.argv) > 1 else 'pr'
LUM = np.array([0.2126, 0.7152, 0.0722], np.float32)

def gi(path):
    b = open(path, 'rb').read()
    w, h = np.frombuffer(b[:8], np.uint32)
    return np.frombuffer(b[8:], np.float16).reshape(h, w, 4).astype(np.float32)

runs = {}
for line in open(rf'D:\Games\Crysis-RE\logs\spot\{tag}_probe_response.txt', encoding='utf-8'):
    name, files = line.strip().split('=', 1)
    runs[name] = [gi(f) for f in files.split(',') if f]
for name, caps in runs.items():
    L = [np.nan_to_num(c[..., :3] @ LUM) for c in caps]
    # The lamp's first capture: where the GI light rose by a third over the first (captures can be lost).
    k = next((i for i in range(1, len(L)) if L[i].mean() > 1.33 * L[0].mean()), len(L))
    if k < 2 or len(L) - k < 2:
        print(name, len(caps), 'GI captures, lamp at', k, ': too few')
        continue
    level = float(np.mean([l.mean() for l in L[:k]]))
    noise = float(np.mean([np.abs(L[i + 1] - L[i]).mean() for i in range(k - 1)]))
    lit = L[-1] - L[k - 1] > np.quantile(L[-1] - L[k - 1], 0.8)  # where the lamp's GI ends up (its brightest 20%)
    gain = [float((L[i] - L[k - 1])[lit].mean()) for i in range(k, len(L))]
    print(f'{name:10s} still ({k} captures): level {level:.4f}, change between captures {noise:.5f} '
          f'({noise / max(level, 1e-6) * 100:.2f}%) | lamp on, share of the last: ' + ' '.join(f'{g / gain[-1] * 100:5.1f}%' for g in gain))
