# Launcher translations: finds single-quoted JS string literals with Cyrillic text that are not translated yet and
# wraps them as <fn>('русский', 'English').
#   python i18n_wrap.py list <file.js>                       numbered list of untranslated literals (N<TAB>text)
#   python i18n_wrap.py apply <file.js> <fn> <translations>  translations: lines "N<TAB>English" from the list
#   python i18n_wrap.py check <file.js>                      count of untranslated literals (0 = done)
# A literal counts as translated when it is the first argument of L( or tt( (the Russian half of a pair).
import io
import re
import sys

CYR = re.compile('[Ѐ-ӿ]')


def literals(src):
    """(start, end, text) of every single-quoted literal outside comments, regexes are not expected in these files."""
    out, i, n = [], 0, len(src)
    while i < n:
        c = src[i]
        if c == '/' and src.startswith('//', i):
            i = src.find('\n', i)
            i = n if i < 0 else i
            continue
        if c == '/' and src.startswith('/*', i):
            j = src.find('*/', i + 2)
            i = n if j < 0 else j + 2
            continue
        if c == '/':
            # A regex literal where an expression may start (after ( , = : ! & | ? ; { } or return); skip it whole.
            k = i - 1
            while k >= 0 and src[k] in ' \t\r\n':
                k -= 1
            if k < 0 or src[k] in '(,=:!&|?;{}[' or src[max(0, k - 5):k + 1] == 'return':
                j, cls = i + 1, False
                while j < n and (cls or src[j] != '/') and src[j] != '\n':
                    if src[j] == '\\':
                        j += 1
                    elif src[j] == '[':
                        cls = True
                    elif src[j] == ']':
                        cls = False
                    j += 1
                i = j + 1
                continue
            i += 1
            continue
        if c in '"`':
            j = i + 1
            while j < n and src[j] != c:
                j += 2 if src[j] == '\\' else 1
            i = j + 1
            continue
        if c == "'":
            j = i + 1
            while j < n and src[j] != "'":
                j += 2 if src[j] == '\\' else 1
            out.append((i, j + 1, src[i + 1:j]))
            i = j + 1
            continue
        i += 1
    return out


def untranslated(src):
    res = []
    for s, e, t in literals(src):
        if not CYR.search(t):
            continue
        before = src[max(0, s - 4):s]
        if before.endswith('L(') or before.endswith('tt('):
            continue
        if src[e:e + 16].lstrip().startswith('/* not ui */'):  # data, not text for people (a section marker, ...)
            continue
        res.append((s, e, t))
    return res


def main():
    mode, path = sys.argv[1], sys.argv[2]
    src = io.open(path, encoding='utf-8').read()
    items = untranslated(src)
    if mode == 'list':
        for k, (_, _, t) in enumerate(items):
            print('%d\t%s' % (k, t))
    elif mode == 'check':
        print('%s: %d untranslated' % (path, len(items)))
        for _, _, t in items[:20]:
            print('  ' + t)
    elif mode == 'apply':
        fn = sys.argv[3]
        tr = {}
        for line in io.open(sys.argv[4], encoding='utf-8').read().splitlines():
            if '\t' in line:
                k, en = line.split('\t', 1)
                tr[int(k)] = en
        missing = [k for k in range(len(items)) if k not in tr]
        if missing:
            sys.exit('no translation for: %s' % missing[:20])
        for k in reversed(range(len(items))):
            s, e, t = items[k]
            en = tr[k].replace('\\', '\\\\').replace("'", "\\'") if "\\'" not in tr[k] else tr[k]
            src = src[:s] + "%s('%s', '%s')" % (fn, t, en) + src[e:]
        io.open(path, 'w', encoding='utf-8').write(src)
        print('wrapped %d literals with %s()' % (len(items), fn))


main()
