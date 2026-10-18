"""Prints the 32-bit words at the given addresses (Ghidra DAT_xxxxxxxx literals), with what they point to.

Usage: python -P tools/analysis/peek.py [module] ADDR [ADDR ...]     module: main (default), itcm, dtcm, ov000..ov007
Each line: address, word, signed value, and the string / symbol the word points to when there is one.
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import romimage  # noqa: E402


def describe(mods, home, v):
    m = romimage.resolve(mods, home, v & ~1)
    if not m:
        return ""
    s = m.cstring(v)
    if s:
        return '"%s"' % s.replace("\n", "\\n")
    f = m.functions.get(v & ~1)
    if f:
        return "&%s" % f[0]
    sym = m.symbols.get(v)
    if sym:
        return "&%s" % sym[0]
    return "-> %s:%08x" % (m.name, v)


def main():
    args = sys.argv[1:]
    mods = romimage.load()
    home = "main"
    if args and args[0] in mods:
        home = args.pop(0)
    for a in args:
        addr = int(a.split("_")[-1], 16)
        m = romimage.resolve(mods, home, addr)
        if not m:
            print("%08x ?" % addr)
            continue
        v = m.u32(addr)
        sv = v - (1 << 32) if v & 0x80000000 else v
        print("%08x %08x %11d %s" % (addr, v, sv, describe(mods, home, v)))


if __name__ == "__main__":
    main()
