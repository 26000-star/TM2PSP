"""Annotates a Ghidra decompile.ps1 output: each DAT_<module>__ADDR / DAT_ADDR literal gets its value and what it
points to (string, function, symbol) as a trailing comment, once per line.

Usage: python -P tools/analysis/annotate_decomp.py <module> <in.c> <out.c>     module: main, ov000..ov007
"""
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import romimage  # noqa: E402
from peek import describe  # noqa: E402

DAT = re.compile(r"DAT_(?:(ov\d\d\d)__)?([0-9a-f]{8})")


def main():
    home, src, dst = sys.argv[1:4]
    mods = romimage.load()
    cache = {}

    def note(mod, a):
        key = (mod, a)
        if key not in cache:
            m = romimage.resolve(mods, mod, a)
            if not m:
                cache[key] = None
            else:
                v = m.u32(a)
                d = describe(mods, mod, v)
                cache[key] = "%s=%x%s" % (DAT.pattern and "%08x" % a, v, (" " + d) if d else "")
        return cache[key]

    out = []
    with open(src, encoding="utf-8", errors="replace") as f:
        for line in f:
            line = line.rstrip("\n")
            notes = []
            for mm in DAT.finditer(line):
                n = note(mm.group(1) or home, int(mm.group(2), 16))
                if n and n not in notes:
                    notes.append(n)
            out.append(line + ("  /* " + "; ".join(notes) + " */" if notes else ""))
    with open(dst, "w", encoding="utf-8", newline="\n") as f:
        f.write("\n".join(out) + "\n")


if __name__ == "__main__":
    main()
