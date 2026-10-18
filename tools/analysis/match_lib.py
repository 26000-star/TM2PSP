"""Names the functions of an embedded open-source library by matching call graphs.

Source side: objects compiled from the library source with host gcc
(-O0 -fno-inline -ffunction-sections, so every call keeps a relocation), plus the string literals
of each function, parsed from the .c files.
DS side: build/analysis/functions.jsonl, restricted to the library's address range.

Anchors: luaL_reg-style tables (Lua name -> C function), and string literals that occur in exactly
one function on each side. Propagation: for every matched pair, an unmatched callee (or caller) set
of size one on both sides is a match. This repeats until nothing changes. Only unique matches are
accepted; the compiler may inline or reorder code, so ambiguity is never resolved by guessing.

Usage:
  python -P tools/analysis/match_lib.py <src dir> <obj dir> <block> <start hex> <end hex> <out.txt>
           [--tables reg_tables.txt]
"""
import argparse
import glob
import json
import os
import re
import subprocess
from collections import defaultdict

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))

FUNC_DEF = re.compile(r"^[A-Za-z_][^;{}=#]*?\b([A-Za-z_]\w*)\s*\(([^;{}]*)\)\s*\{", re.M)
STRING = re.compile(r'"((?:[^"\\\n]|\\.)*)"')


def c_unescape(s):
    out, i = [], 0
    simple = {"n": "\n", "t": "\t", "r": "\r", "\\": "\\", '"': '"', "'": "'", "a": "\a", "b": "\b",
              "f": "\f", "v": "\v", "0": "\0"}
    while i < len(s):
        c = s[i]
        if c == "\\" and i + 1 < len(s):
            n = s[i + 1]
            if n in "01234567" and i + 2 < len(s) and s[i + 2] in "01234567":
                j = i + 1
                while j < len(s) and j < i + 4 and s[j] in "01234567":
                    j += 1
                out.append(chr(int(s[i + 1:j], 8)))
                i = j
                continue
            out.append(simple.get(n, n))
            i += 2
        else:
            out.append(c)
            i += 1
    return "".join(out)


def source_functions(src_dir):
    """{name: {"strings": set, "file": str}} by scanning C function bodies (adjacent literals concatenated)."""
    funcs = {}
    for path in glob.glob(os.path.join(src_dir, "**", "*.c"), recursive=True):
        text = open(path, encoding="latin1").read()
        for m in FUNC_DEF.finditer(text):
            name = m.group(1)
            if name in ("if", "while", "for", "switch", "return", "sizeof"):
                continue
            depth, i = 0, m.end() - 1
            while i < len(text):
                if text[i] == "{":
                    depth += 1
                elif text[i] == "}":
                    depth -= 1
                    if depth == 0:
                        break
                i += 1
            body = text[m.end():i]
            body = re.sub(r"/\*.*?\*/", "", body, flags=re.S)
            # Adjacent literals ("a" "b") are one string in C.
            body = re.sub(r'"\s*\n\s*"', "", body)
            body = re.sub(r'"\s+"', "", body)
            strings = {c_unescape(s) for s in STRING.findall(body)}
            funcs[name] = {"strings": strings, "file": os.path.basename(path)}
    return funcs


def source_calls(obj_dir, known):
    """{caller: set(callee)} from relocations in objdump -dr output (calls to library functions only)."""
    calls = defaultdict(set)
    for obj in glob.glob(os.path.join(obj_dir, "*.o")):
        out = subprocess.run(["objdump", "-dr", obj], capture_output=True, text=True).stdout
        cur = None
        for line in out.splitlines():
            m = re.match(r"^[0-9a-f]+ <(?:\.text\$)?([A-Za-z_]\w*)>:", line)
            if m:
                cur = m.group(1)
                continue
            # Same-object calls go through the COFF section symbol ".text$<function>".
            m = re.search(r"(?:REL32|PLT32|PC32)\s+(?:\.text\$)?([A-Za-z_]\w*)", line)
            if cur and m and m.group(1) in known and m.group(1) != cur:
                calls[cur].add(m.group(1))
    return calls


def reg_tables(src_dir):
    """Lua-visible name -> C function, from { "name", cfunc } table rows in the source."""
    out = {}
    for path in glob.glob(os.path.join(src_dir, "**", "*.c"), recursive=True):
        for name, fn in re.findall(r'\{\s*"([A-Za-z_]\w*)"\s*,\s*([A-Za-z_]\w*)\s*\}', open(path, encoding="latin1").read()):
            out.setdefault(name, set()).add(fn)
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("src")
    ap.add_argument("obj")
    ap.add_argument("block")
    ap.add_argument("start")
    ap.add_argument("end")
    ap.add_argument("out")
    ap.add_argument("--tables")
    ap.add_argument("--exclude", default="", help="comma-separated source files not linked in the ROM")
    ap.add_argument("--profiles", action="store_true", help="tie-break equal-size candidate sets by profile")
    a = ap.parse_args()
    lo, hi = int(a.start, 16), int(a.end, 16)

    excluded = {x.strip() for x in a.exclude.split(",") if x.strip()}
    sfun = {n: f for n, f in source_functions(a.src).items() if f["file"] not in excluded}
    scall = source_calls(a.obj, set(sfun))
    scallers = defaultdict(set)
    for s, cs in scall.items():
        for c in cs:
            scallers[c].add(s)

    D = {}
    for line in open(os.path.join(ROOT, "build", "analysis", "functions.jsonl"), encoding="utf-8"):
        f = json.loads(line)
        ad = int(f["addr"].split(":")[-1], 16)
        if f["block"] == a.block and lo <= ad < hi:
            D[f["addr"]] = f
    dcall = {k: {c for c in f["callees"] if c in D} for k, f in D.items()}
    dcallers = defaultdict(set)
    for k, cs in dcall.items():
        for c in cs:
            dcallers[c].add(k)

    match, why = {}, {}  # source name -> ds addr

    def add(s, d, reason):
        if s in match or d in match.values():
            return False
        match[s], why[s] = d, reason
        return True

    # Anchor 1: registration tables found in the ROM.
    if a.tables:
        names = reg_tables(a.src)
        for line in open(a.tables):
            m = re.match(r"\s+\S+:([0-9a-f]{8}) (\S+)$", line)
            if m and m.group(1) in D and m.group(2) in names and len(names[m.group(2)]) == 1:
                add(next(iter(names[m.group(2)])), m.group(1), "table:" + m.group(2))

    # Anchor 2: string literals unique to one function on both sides.
    s_by_str = defaultdict(set)
    for n, f in sfun.items():
        for s in f["strings"]:
            if len(s) >= 4:
                s_by_str[s].add(n)
    d_by_str = defaultdict(set)
    for k, f in D.items():
        for s in f["strings"]:
            d_by_str[s].add(k)
    for s, ns in s_by_str.items():
        if len(ns) == 1 and len(d_by_str.get(s, ())) == 1:
            add(next(iter(ns)), next(iter(d_by_str[s])), "string:" + s[:40])

    def profile(graph_callees, graph_callers, x, known, inv_known):
        """Structural signature: callee/caller counts plus which already-matched functions it calls."""
        cs = graph_callees.get(x, ())
        return (len(cs), len(graph_callers.get(x, ())),
                frozenset(known.get(c, inv_known.get(c)) for c in cs if c in known or c in inv_known))

    # Propagation over callees and callers.
    changed = True
    while changed:
        changed = False
        inv = {d: s for s, d in match.items()}
        for s, d in list(match.items()):
            for sg, dg, kind in ((scall, dcall, "callee"), (scallers, dcallers, "caller")):
                su = {x for x in sg.get(s, ()) if x not in match}
                du = {x for x in dg.get(d, ()) if x not in inv}
                if not su or not du:
                    continue
                if len(su) == 1 and len(du) == 1:
                    pairs = [(next(iter(su)), next(iter(du)), "%s of %s" % (kind, s))]
                elif len(su) == len(du) and a.profiles:
                    # Tie-break by profile; matched callees are compared by their source names.
                    sp = defaultdict(list)
                    for x in su:
                        sp[profile(scall, scallers, x, {k: k for k in match}, {})].append(x)
                    dp = defaultdict(list)
                    for x in du:
                        dp[profile(dcall, dcallers, x, {}, inv)].append(x)
                    pairs = [(sp[p][0], dp[p][0], "%s of %s (profile)" % (kind, s))
                             for p in sp if len(sp[p]) == 1 and len(dp.get(p, ())) == 1]
                else:
                    pairs = []
                for sx, dx, reason in pairs:
                    if add(sx, dx, reason):
                        inv[dx] = sx
                        changed = True

    with open(a.out, "w") as w:
        for s, d in sorted(match.items(), key=lambda kv: kv[1]):
            w.write("%s %s %s # %s\n" % (d, s, sfun.get(s, {}).get("file", "custom"), why[s]))
    used = {s for s in sfun if s in scall or s in scallers}
    print("source functions %d (in call graph %d), DS functions %d, matched %d"
          % (len(sfun), len(used), len(D), len(match)))
    kinds = defaultdict(int)
    for r in why.values():
        kinds[r.split(":")[0].split(" ")[0]] += 1
    print("by reason:", dict(kinds))


if __name__ == "__main__":
    main()
