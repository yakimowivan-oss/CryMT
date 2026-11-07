"""GPU ms per pass label (crymt_gpu.txt) averaged over frame windows, joined to frames through crymt_detail.txt.
gpuwin.py <gpu file> <detail file> <window a-b> <window c-d> [lap=1500] [top=25]"""
import sys

opts = {a.split("=")[0]: int(a.split("=")[1]) for a in sys.argv[3:] if "=" in a}
wins = [a for a in sys.argv[3:] if "=" not in a]
lap, top = opts.get("lap", 1500), opts.get("top", 25)
lines = open(sys.argv[1]).read().splitlines()
names = lines[0].split()[1:]
gpu = {}
for line in lines[1:]:
    p = line.split()
    gpu[int(p[0])] = [float(x) for x in p[1:]]
seq_of = {}
for line in open(sys.argv[2]).read().splitlines()[1:]:
    p = line.split()
    seq_of[int(p[0])] = int(p[6])
means = []
for w in wins:
    a, b = map(int, w.split("-"))
    rows = [gpu[seq_of[f]] for f in seq_of if a <= f % lap <= b and f >= lap and seq_of[f] in gpu]
    n = len(rows)
    means.append([sum(r[k] for r in rows if k < len(r)) / max(n, 1) for k in range(len(names))] + [n])
print("frames per window:", [m[-1] for m in means])
print(f"{'label':24}" + "".join(f"{w:>12}" for w in wins) + "       delta")
print(f"{'total':24}" + "".join(f"{sum(m[:-1]):12.3f}" for m in means) + f"{sum(means[-1][:-1]) - sum(means[0][:-1]):12.3f}")
order = sorted(range(len(names)), key=lambda k: -max(m[k] for m in means))
for k in order[:top]:
    print(f"{names[k]:24}" + "".join(f"{m[k]:12.3f}" for m in means) + f"{means[-1][k] - means[0][k]:12.3f}")
