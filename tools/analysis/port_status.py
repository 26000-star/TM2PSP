"""Port progress per function, keyed by dsd symbols.

The status of a function lives next to its code: a comment "@port <module>:<addr> <status>" (module = main, itcm,
dtcm, ov000..ov007; addr in hex without 0x) anywhere in src/. Statuses:
  done     decompiled and ported, same behaviour as the original
  partial  ported with documented simplifications (the comment next to the tag says what)
  stub     placeholder that does not do the original's work yet
  lib      replaced by a third-party source or a re-implementation of its API (src/lib)
Functions without a tag get a default from config/code_map.txt and the shim:
  shim     NitroSDK/NitroSystem function whose name is implemented in src/shim (declared in nitro.h)
  lib      Lua 5.0.2 (vendored source) and MSL (host libc/libstdc++)
  drop     library that the port leaves out (network, Download Play, ...)
  todo     everything else

Usage: python -P tools/analysis/port_status.py [--list] [--race-only]
Writes build/analysis/port_status.txt (one line per function) and prints a summary; with build/analysis/race_inventory
(tools/analysis/callgraph.py race) the summary also covers the functions reachable in race mode.
"""
import collections
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import callgraph  # noqa: E402

ROOT = callgraph.ROOT
TAG = re.compile(r"@port\s+(main|itcm|dtcm|ov\d{3}):([0-9a-fA-F]{8})\s+(done|partial|stub|lib)\b")
STATUSES = ["done", "partial", "stub", "lib", "shim", "drop", "todo"]


def scan_tags():
    tags = {}
    for base, _, files in os.walk(os.path.join(ROOT, "src")):
        for f in files:
            if not f.endswith((".c", ".cpp", ".h", ".hpp")):
                continue
            path = os.path.join(base, f)
            rel = os.path.relpath(path, ROOT).replace(os.sep, "/")
            for lineno, line in enumerate(open(path, encoding="utf-8", errors="replace"), 1):
                for m in TAG.finditer(line):
                    tags[(m.group(1), int(m.group(2), 16))] = (m.group(3), "%s:%d" % (rel, lineno))
    return tags


def shim_names():
    names = set()
    p = os.path.join(ROOT, "src", "shim", "include", "nitro.h")
    for line in open(p):
        m = re.match(r"^\s*(?:static inline\s+)?[\w\s\*]+?\b(\w+)\s*\(", line)
        if m and not line.lstrip().startswith(("#", "/*", "*", "typedef")):
            names.add(m.group(1))
    return names


def main():
    g = callgraph.Graph()
    rows = callgraph.load_code_map()
    tags = scan_tags()
    shim = shim_names()
    race = set()
    inv = os.path.join(ROOT, "build", "analysis", "race_inventory.txt")
    if os.path.exists(inv):
        for line in open(inv):
            p = line.split()
            if p and not line.startswith("#"):
                race.add((p[0], int(p[1], 16)))
    out = []
    total = collections.Counter()
    in_race = collections.Counter()
    by_action = collections.defaultdict(collections.Counter)
    unknown_tags = [k for k in tags if k[1] not in g.mods.get(k[0], g.mods["main"]).funcs]
    for mn, mod in g.mods.items():
        for a, (name, size, thumb) in sorted(mod.funcs.items()):
            lib, act = callgraph.classify(rows, mn, a)
            if (mn, a) in tags:
                status, where = tags[(mn, a)]
            elif act == "drop":
                status, where = "drop", ""
            elif name in shim:
                status, where = "shim", "src/shim"
            elif lib in ("lua", "msl"):
                status, where = "lib", ""
            else:
                status, where = "todo", ""
            r = (mn, a) in race
            total[status] += 1
            if r:
                in_race[status] += 1
                by_action[act][status] += 1
            if "--race-only" in sys.argv and not r:
                continue
            out.append("%-5s %08x %-7s %-7s %-12s %s%s %s" % (mn, a, status, act, lib, name, "  [race]" if r else "", where))
    path = os.path.join(ROOT, "build", "analysis", "port_status.txt")
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w") as f:
        f.write("# module addr status action library name [race] source  (tools/analysis/port_status.py)\n")
        f.write("\n".join(out) + "\n")
    print("all functions:  " + "  ".join("%s %d" % (s, total[s]) for s in STATUSES if total[s]))
    if race:
        print("race reachable: " + "  ".join("%s %d" % (s, in_race[s]) for s in STATUSES if in_race[s]))
        for act in ("port", "replace", "drop"):
            c = by_action[act]
            print("  %-7s %5d: %s" % (act, sum(c.values()), "  ".join("%s %d" % (s, c[s]) for s in STATUSES if c[s])))
    if unknown_tags:
        print("tags on addresses that are not function starts:", ["%s:%08x" % k for k in unknown_tags])
    if "--list" in sys.argv:
        print("\n".join(out))
    print("written", os.path.relpath(path, ROOT))


if __name__ == "__main__":
    main()
