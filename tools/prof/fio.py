# Exact-replacement edits for patch scripts: F(path).rep(old, new) asserts one match, CRLF kept; .save(). append(path, text)
# adds text keeping the file's line endings. Write the script with the Write tool (no bash heredoc) and use raw strings
# (r'''...''') for text with backslashes: in a plain Python string '\b' is a backspace and '\n' a line break, which
# silently broke a path (snapshots\build) and a C string literal before. Both helpers refuse such control characters.
import io

CR = chr(13)
LF = chr(10)
TAB = chr(9)


def check_text(text, what):
    bad = sorted({hex(ord(c)) for c in text if ord(c) < 32 and c not in (CR, LF, TAB)})
    assert not bad, (what, 'control characters ' + ', '.join(bad) + ': a backslash escape in a non-raw string?')


class F:
    """A source file edited with exact replacements; CRLF files are handled as LF and written back as CRLF."""

    def __init__(self, p):
        self.p = p
        self.s = io.open(p, encoding='utf-8', newline='').read()
        self.crlf = (CR + LF) in self.s
        self.s = self.s.replace(CR + LF, LF)

    def rep(self, a, b):
        check_text(b, (self.p, b[:80]))
        assert self.s.count(a) == 1, (self.p, a[:80], self.s.count(a))
        self.s = self.s.replace(a, b)

    def save(self):
        out = self.s.replace(LF, CR + LF) if self.crlf else self.s
        io.open(self.p, 'w', encoding='utf-8', newline='').write(out)


def append(p, text):
    """Appends text to a file in its own line endings."""
    check_text(text, (p, text[:80]))
    s = io.open(p, encoding='utf-8', newline='').read()
    if (CR + LF) in s:
        text = text.replace(CR + LF, LF).replace(LF, CR + LF)
    io.open(p, 'a', encoding='utf-8', newline='').write(text)
