"""Finds {const char *name, function *fn} tables (Lua luaL_reg and similar) in every module.

Output: build/analysis/reg_tables.txt, one table per block: address, then "func_addr name".
"""
import os
import re
import sys

sys.path.insert(0, os.path.dirname(__file__))
import romimage

IDENT = re.compile(r"^[A-Za-z_][A-Za-z0-9_.:]{0,63}$")


def entry(mods, home, addr):
    m = romimage.resolve(mods, home, addr)
    if not m:
        return None
    sp = m.u32(addr)
    fp = m.u32(addr + 4)
    sm = romimage.resolve(mods, home, sp)
    fm = romimage.resolve(mods, home, fp & ~1)
    if not sm or not fm:
        return None
    s = sm.cstring(sp, 64)
    if not s or not IDENT.match(s):
        return None
    f = fm.functions.get(fp & ~1)
    if not f or f[1] != bool(fp & 1):
        return None
    return s, fm.name, fp & ~1


def main():
    mods = romimage.load()
    out_dir = os.path.join(romimage.ROOT, "build", "analysis")
    os.makedirs(out_dir, exist_ok=True)
    tables = []
    for name, m in mods.items():
        a = m.base
        while a + 8 <= m.end:
            rows = []
            b = a
            while True:
                e = entry(mods, name, b)
                if not e:
                    break
                rows.append(e)
                b += 8
            if len(rows) >= 2:
                terminated = m.contains(b + 4) and m.u32(b) == 0
                tables.append((name, a, rows, terminated))
                a = b
            else:
                a += 4
    with open(os.path.join(out_dir, "reg_tables.txt"), "w") as w:
        for name, a, rows, term in tables:
            w.write("== %s %08x  %d entries%s\n" % (name, a, len(rows), "" if term else "  (no NULL terminator)"))
            for s, fmod, fa in rows:
                w.write("  %s:%08x %s\n" % (fmod, fa, s))
    total = sum(len(t[2]) for t in tables)
    print("%d tables, %d entries" % (len(tables), total))
    for name, a, rows, term in tables:
        print("  %-6s %08x %3d  %s" % (name, a, len(rows), ", ".join(r[0] for r in rows[:8])))


if __name__ == "__main__":
    main()
