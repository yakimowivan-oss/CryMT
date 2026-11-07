"""Top-down call tree from a .folded profile: tree.py <file> <min%> [<max depth>]"""
import sys
from collections import defaultdict


def main(path, min_pct, max_depth):
    children = defaultdict(lambda: defaultdict(int))
    total = 0
    for line in open(path):
        stack, n = line.rstrip().rsplit(" ", 1)
        n = int(n)
        total += n
        node = ""
        for fr in stack.split(";"):
            child = node + ";" + fr
            children[node][child] += n
            node = child

    def walk(node, depth):
        for child, n in sorted(children[node].items(), key=lambda kv: -kv[1]):
            pct = 100 * n / total
            if pct < min_pct or depth > max_depth:
                continue
            print(f"{pct:6.2f}% {'  ' * depth}{child.rsplit(';', 1)[1]}")
            walk(child, depth + 1)

    walk("", 0)


main(sys.argv[1], float(sys.argv[2]), int(sys.argv[3]) if len(sys.argv) > 3 else 60)
