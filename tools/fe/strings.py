"""Dumps the game's localised strings (strings/main: tokens.dat + <file>/<lang>.str, as dsStringTable reads them).

Usage: python -P tools/fe/strings.py [--lang gb] [--file frontend|common|backend] [--grep text]
Prints "key<TAB>text" per string, in the order of the names block (texts in Latin-1, shown as Unicode).
"""
import os
import struct
import sys
import zipfile

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..")
FILES = ["common", "frontend", "backend"]


def load(lang="gb", vfs=None):
    """{file: [(key, text)]} for the three files of strings/main."""
    z = vfs or zipfile.ZipFile(os.path.join(ROOT, "extract", "files", "vfs.zip"))
    names = {n.lower(): n for n in z.namelist()}
    d = z.read(names["strings/main/tokens.dat"])
    counts = [struct.unpack_from("<H", d, 6 * k)[0] for k in range(3)]
    p = 18 + 512
    keys = []  # (file, key) in block order
    while p < len(d):
        f = d[p]
        e = d.index(0, p + 1)
        keys.append((f, d[p + 1:e].decode("latin-1")))
        p = e + 1
    out = {}
    for fi, fname in enumerate(FILES):
        s = z.read(names["strings/main/%s/%s.str" % (fname, lang)])
        mine = [k for f, k in keys if f == fi]
        q, texts = 0, []
        for n in range(min(counts[fi], len(mine))):
            ln = struct.unpack_from("<H", s, q)[0]
            texts.append((mine[n], s[q + 2:q + 2 + ln].decode("latin-1")))
            q += 2 + ln
        out[fname] = texts
    return out


if __name__ == "__main__":
    a = sys.argv[1:]
    opt = lambda k, d: a[a.index(k) + 1] if k in a else d
    lang, which, grep = opt("--lang", "gb"), opt("--file", None), opt("--grep", None)
    for fname, texts in load(lang).items():
        if which and fname != which:
            continue
        for k, t in texts:
            if grep and grep.lower() not in (k + t).lower():
                continue
            sys.stdout.buffer.write(("%s\t%s\t%s\n" % (fname, k, t.replace("\n", "\\n"))).encode("utf-8"))
