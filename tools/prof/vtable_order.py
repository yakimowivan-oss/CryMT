r"""Virtual function slots of an interface in the Crysis Mod SDK headers (MSVC order): the base's slots first, then the
struct's own virtual functions in declaration order, overloads of one name grouped where the first one stands and in
reverse declaration order (MSVC). Prints slot, name and the declaration line.
  python tools\prof\vtable_order.py <header dir> <Struct> [name filter]
"""
import os
import re
import sys


def find_struct(folder, name):
    pat = re.compile(r'^\s*(struct|class)\s+%s\b\s*(:\s*(public\s+)?(\w+))?' % re.escape(name))
    for root, _, files in os.walk(folder):
        for f in files:
            if not f.endswith('.h'):
                continue
            path = os.path.join(root, f)
            try:
                lines = open(path, encoding='latin-1').read().split('\n')
            except OSError:
                continue
            for i, line in enumerate(lines):
                m = pat.match(line)
                if m and not line.rstrip().endswith(';'):
                    base = m.group(4)
                    # "struct IActor:" with "public IGameObjectExtension" on the next line
                    if not base and line.rstrip().endswith(':') and i + 1 < len(lines):
                        n = re.match(r'^\s*(public\s+)?(\w+)', lines[i + 1])
                        base = n.group(2) if n else None
                    return path, i, lines, base
    return None, None, None, None


def body(lines, start):
    depth, out, began = 0, [], False
    for line in lines[start:]:
        code = line.split('//')[0]
        if '{' in code:
            began = True
        if began:
            out.append(line)
        depth += code.count('{') - code.count('}')
        if began and depth == 0:
            break
    return out


def virtuals(folder, name):
    path, start, lines, base = find_struct(folder, name)
    if path is None:
        raise SystemExit('struct %s not found' % name)
    slots = virtuals(folder, base) if base else []
    own = []
    text = '\n'.join(body(lines, start))
    text = re.sub(r'/\*.*?\*/', '', '\n'.join(l.split('//')[0] for l in text.split('\n')), flags=re.S)
    text = '\n'.join(l for l in text.split('\n') if not l.strip().startswith('#'))
    # Declarations at the struct's own level only (depth 1): nested structs and inline bodies are skipped.
    depth, cur = 0, ''
    for ch in text:
        if ch in ';{}':
            s = ' '.join(cur.split())
            if depth == 1 and s.startswith('virtual '):
                m = re.search(r'(~?\w+)\s*\(', s)
                if m:
                    own.append((m.group(1), s))
            cur = ''
            if ch == '{':
                depth += 1
            elif ch == '}':
                depth -= 1
        else:
            cur += ch
    # MSVC: overloads grouped at the first one's place, reversed
    order, seen = [], set()
    for n, s in own:
        if n in seen:
            continue
        group = [x for x in own if x[0] == n]
        order.extend(reversed(group) if len(group) > 1 else group)
        seen.add(n)
    # A base's virtual declared again overrides its slot (I3DEngine re-declares IProcess's Init, Update, RenderWorld,
    # ShutDown: counted as new, GetTimeOfDay came out 165 where the game has 161).
    names_in_base = {n for n, _ in slots}
    for n, s in order:
        if n not in names_in_base:
            slots.append((n, s))
    return slots


def preprocessor(folder, name):
    """The #if / #ifdef / #else / #endif lines inside the struct and its bases (they can change the count)."""
    out = []
    while name:
        path, start, lines, base = find_struct(folder, name)
        if path is None:
            break
        for line in body(lines, start):
            if line.strip().startswith('#'):
                out.append('%s: %s' % (name, line.strip()))
        name = base
    return out


def main():
    folder, name = sys.argv[1], sys.argv[2]
    flt = sys.argv[3] if len(sys.argv) > 3 else None
    for i, (n, s) in enumerate(virtuals(folder, name)):
        if not flt or flt.lower() in n.lower():
            print('%3d  %-28s %s' % (i, n, s[:110]))
    for p in preprocessor(folder, name):
        print('     preprocessor: ' + p)


if __name__ == '__main__':
    main()
