# Горизонтальные полосы: спектр построчного среднего высокочастотной составляющей, пики относительно медианы.
import sys, numpy as np
from PIL import Image
for path in sys.argv[1:]:
    L = np.asarray(Image.open(path).convert('L')).astype(np.float32)
    h, w = L.shape
    hp = L - sum(np.roll(L, k, 0) for k in (-2, -1, 0, 1, 2)) / 5
    out = []
    for x0 in range(0, w, w // 3):
        r = hp[h // 10:h - h // 10, x0:x0 + w // 3].mean(axis=1)
        f = np.abs(np.fft.rfft(r)); n = len(r); med = np.median(f[5:])
        k = np.argsort(f[5:])[-2:][::-1] + 5
        out.append(' '.join(f'{n / i:.1f}px:{f[i] / med:.1f}' for i in k))
    print(path.split('/')[-1], w, 'x', h, '|', ' | '.join(out))
