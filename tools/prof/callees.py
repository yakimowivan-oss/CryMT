"""Time spent under a function, split by direct callee, ignoring recursion: callees.py <folded> <module!+0xRVA>[,more] [min%]"""
import sys
from collections import Counter


def main(path, targets, min_pct):
    targets = set(targets.split(","))
    total = 0
    under = 0
    callee = Counter()
    for line in open(path):
        stack, n = line.rstrip().rsplit(" ", 1)
        n = int(n)
        total += n
        frames = stack.split(";")
        idx = [i for i, f in enumerate(frames) if f in targets]
        if not idx:
            continue
        under += n
        last = idx[-1]
        callee[frames[last + 1] if last + 1 < len(frames) else "<self>"] += n
    print(f"under target: {100 * under / total:.2f}% of samples")
    for name, n in callee.most_common():
        pct = 100 * n / total
        if pct >= min_pct:
            print(f"{pct:6.2f}%  {name}")


main(sys.argv[1], sys.argv[2], float(sys.argv[3]) if len(sys.argv) > 3 else 0.2)
