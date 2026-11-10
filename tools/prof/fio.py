# Exact-replacement edits for patch scripts: F(path).rep(old, new) asserts one match, CRLF kept; .save(). Use from a
# Python script written with the Write tool (bash heredocs mangle backslashes).
import io

CR = chr(13)
LF = chr(10)


class F:
    """A source file edited with exact replacements; CRLF files are handled as LF and written back as CRLF."""

    def __init__(self, p):
        self.p = p
        self.s = io.open(p, encoding='utf-8', newline='').read()
        self.crlf = (CR + LF) in self.s
        self.s = self.s.replace(CR + LF, LF)

    def rep(self, a, b):
        assert self.s.count(a) == 1, (self.p, a[:80], self.s.count(a))
        self.s = self.s.replace(a, b)

    def save(self):
        out = self.s.replace(LF, CR + LF) if self.crlf else self.s
        io.open(self.p, 'w', encoding='utf-8', newline='').write(out)
