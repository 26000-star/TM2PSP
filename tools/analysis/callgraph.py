"""Reference graph of the ARM9 code built from the dsd relocations, and the race-mode reachability set.

Nodes are functions (from every symbols.txt) and data runs. A data run is a maximal sequence of relocated words in a
data/rodata section with gaps of at most 8 bytes, which is how C++ vtables (RTTI pointer, offset 0, methods), luaL_reg
tables and other function-pointer tables look. Code that loads the address of a run pulls in everything the run points
to, so virtual calls are followed through the constructors that install the vtables. This over-approximates (every
method of every constructed class), which is what a port inventory needs.

Usage:
  python -P tools/analysis/callgraph.py race      [--out build/analysis/race_inventory.txt]
  python -P tools/analysis/callgraph.py callees   <selector>      # direct references of one function
  python -P tools/analysis/callgraph.py callers   <selector>
  selector: name | 0xADDR | ovNNN:0xADDR
"""
import bisect
import collections
import os
import re
import sys

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..")
CFG = os.path.join(ROOT, "config", "arm9")

MODULES = ["main", "itcm", "dtcm"] + ["ov%03d" % i for i in range(8)]
# Modules present while racing. ov003 is the race overlay; ov004 (front end) shares its address range.
RACE_MODULES = {"main", "itcm", "dtcm", "ov003"}


def module_dir(m):
    if m == "main":
        return CFG
    if m in ("itcm", "dtcm"):
        return os.path.join(CFG, m)
    return os.path.join(CFG, "overlays", m)


SYM_RE = re.compile(r"^(\S+) kind:(\w+)(?:\(([^)]*)\))? addr:0x([0-9a-fA-F]+)")
SEC_RE = re.compile(r"^\s+(\S+)\s+start:0x([0-9a-f]+) end:0x([0-9a-f]+) kind:(\w+)")
REL_RE = re.compile(r"^from:0x([0-9a-f]+) kind:(\w+) to:0x([0-9a-f]+) module:(\S*)")


class Module:
    def __init__(self, name):
        self.name = name
        d = module_dir(name)
        self.sections = []
        for l in open(os.path.join(d, "delinks.txt")):
            m = SEC_RE.match(l)
            if not m:
                if l.strip() and not l.startswith(" ") and not l.startswith("\t"):
                    break  # end of the section header, file list follows
                continue
            self.sections.append((m.group(1), int(m.group(2), 16), int(m.group(3), 16), m.group(4)))
        self.funcs = {}  # addr -> (name, size, thumb)
        for l in open(os.path.join(d, "symbols.txt")):
            m = SYM_RE.match(l)
            if not m or m.group(2) != "function":
                continue
            attrs = m.group(3) or ""
            size = re.search(r"size=0x([0-9a-f]+)", attrs)
            self.funcs[int(m.group(4), 16)] = (m.group(1), int(size.group(1), 16) if size else 2, "thumb" in attrs)
        self.fstarts = sorted(self.funcs)
        self.relocs = []
        for l in open(os.path.join(d, "relocs.txt")):
            m = REL_RE.match(l)
            if m:
                self.relocs.append((int(m.group(1), 16), m.group(2), int(m.group(3), 16), m.group(4)))
        self.relocs.sort()
        self._runs()

    def section_of(self, a):
        for s in self.sections:
            if s[1] <= a < s[2]:
                return s
        return None

    def func_at(self, a):
        i = bisect.bisect_right(self.fstarts, a) - 1
        if i >= 0:
            s = self.fstarts[i]
            if a < s + max(self.funcs[s][1], 2):
                return s
        return None

    def _runs(self):
        """Groups relocations located in data sections into runs."""
        self.runs = []  # (start, end_exclusive, [targets])
        cur = None
        for frm, kind, to, mod in self.relocs:
            sec = self.section_of(frm)
            if not sec or sec[3] == "code":
                continue
            if cur and frm - cur[1] <= 8 and self.section_of(cur[0]) == sec:
                cur[1] = frm + 4
                cur[2].append((to, mod))
            else:
                cur = [frm, frm + 4, [(to, mod)]]
                self.runs.append(cur)
        self.run_starts = [r[0] for r in self.runs]

    def run_at(self, a):
        """The run that contains a, or starts within 8 bytes after it (vptr may point before the first relocation)."""
        i = bisect.bisect_right(self.run_starts, a + 8) - 1
        if i >= 0 and a < self.runs[i][1]:
            return i
        return None


def target_modules(mod_tag, src_module):
    if mod_tag in ("main", "itcm", "dtcm"):
        return [mod_tag]
    m = re.match(r"overlays?\(([\d,]+)\)", mod_tag)
    if m:
        return ["ov%03d" % int(x) for x in m.group(1).split(",")]
    return [src_module]


class Graph:
    def __init__(self):
        self.mods = {m: Module(m) for m in MODULES}
        self.edges = collections.defaultdict(set)  # node -> nodes; node = (module, 'f'|'d', addr)
        for mod in self.mods.values():
            for frm, kind, to, tag in mod.relocs:
                src = self.node_from(mod, frm)
                if src is None:
                    continue
                for tm in target_modules(tag, mod.name):
                    dst = self.node_to(tm, to)
                    if dst is not None and dst != src:
                        self.edges[src].add(dst)

    def node_from(self, mod, a):
        f = mod.func_at(a)
        if f is not None:
            return (mod.name, "f", f)
        r = mod.run_at(a)
        if r is not None:
            return (mod.name, "d", mod.runs[r][0])
        return None

    def node_to(self, mname, a):
        mod = self.mods.get(mname)
        if mod is None:
            return None
        f = a & ~1
        if f in mod.funcs:
            return (mname, "f", f)
        sec = mod.section_of(a)
        if sec and sec[3] != "code" and sec[3] != "bss":
            r = mod.run_at(a)
            if r is not None:
                return (mname, "d", mod.runs[r][0])
        return None

    def name(self, n):
        m, k, a = n
        if k == "f":
            return self.mods[m].funcs[a][0]
        return "data_%08x" % a

    def resolve(self, sel):
        m = re.match(r"(ov\d{3}|main|itcm|dtcm):0x([0-9a-fA-F]+)$", sel)
        if m:
            return (m.group(1), "f", int(m.group(2), 16))
        if sel.lower().startswith("0x"):
            a = int(sel, 16)
            for mn, mod in self.mods.items():
                if a in mod.funcs:
                    return (mn, "f", a)
        for mn, mod in self.mods.items():
            for a, (nm, _, _) in mod.funcs.items():
                if nm == sel:
                    return (mn, "f", a)
        raise SystemExit("unknown function %s" % sel)

    def roots(self, mods):
        """main(), the crt0 entry and every static-constructor table (.ctor) of the given modules."""
        out = []
        for mn in mods:
            mod = self.mods[mn]
            for s in mod.sections:
                if s[0] in (".ctor", ".init"):
                    for frm, kind, to, tag in mod.relocs:
                        if s[1] <= frm < s[2]:
                            n = self.node_from(mod, frm)
                            if n:
                                out.append(n)
                            for tm in target_modules(tag, mn):
                                d = self.node_to(tm, to)
                                if d:
                                    out.append(d)
        out.append(self.resolve("main"))
        out.append(self.resolve("Entry"))
        return out

    def reach(self, roots, allowed):
        seen = set()
        stack = list(roots)
        crossed = collections.Counter()
        while stack:
            n = stack.pop()
            if n in seen:
                continue
            seen.add(n)
            for d in self.edges.get(n, ()):
                if d[0] not in allowed:
                    crossed[d[0]] += 1
                    continue
                if d not in seen:
                    stack.append(d)
        return seen, crossed


def load_code_map():
    rows = []
    for l in open(os.path.join(ROOT, "config", "code_map.txt")):
        l = l.split("#")[0].split()
        if len(l) >= 5:
            blk = {"arm9_main": "main"}.get(l[0], l[0])
            rows.append((blk, int(l[1], 16), int(l[2], 16), l[3], l[4]))
    return rows


def classify(rows, m, a):
    for blk, lo, hi, lib, act in rows:
        if blk == m and lo <= a < hi:
            return lib, act
    return "?", "?"


def load_modules_map():
    """Heuristic subsystem per game function from build/analysis/game_modules.txt, when present."""
    out = {}
    p = os.path.join(ROOT, "build", "analysis", "game_modules.txt")
    if not os.path.exists(p):
        return out
    for l in open(p):
        m = re.match(r"(\w+)\s+([0-9a-f]{8})\.\.([0-9a-f]{8})\s+\d+ funcs\s+\d+ bytes\s+(\S+)", l)
        if m:
            blk = {"arm9_main": "main"}.get(m.group(1), m.group(1))
            out.setdefault(blk, []).append((int(m.group(2), 16), int(m.group(3), 16), m.group(4)))
    return out


def subsystem(mm, m, a):
    for lo, hi, tag in mm.get(m, ()):
        if lo <= a <= hi:
            return tag
    return "-"


def main():
    if len(sys.argv) < 2:
        raise SystemExit(__doc__)
    g = Graph()
    cmd = sys.argv[1]
    if cmd in ("callees", "callers"):
        n = g.resolve(sys.argv[2])
        if cmd == "callees":
            res = sorted(g.edges.get(n, ()))
        else:
            res = sorted(s for s, ds in g.edges.items() if n in ds)
        for m, k, a in res:
            print("%s:%08x %s" % (m, a, g.name((m, k, a))))
        return
    if cmd != "race":
        raise SystemExit(__doc__)
    out = sys.argv[sys.argv.index("--out") + 1] if "--out" in sys.argv else os.path.join(ROOT, "build", "analysis", "race_inventory.txt")
    rows = load_code_map()
    mm = load_modules_map()
    base, _ = g.reach(g.roots(["main", "itcm", "dtcm"]), {"main", "itcm", "dtcm"})
    seen, crossed = g.reach(g.roots(RACE_MODULES), RACE_MODULES)
    funcs = sorted(n for n in seen if n[1] == "f")
    stats = collections.Counter()
    subs = collections.Counter()
    lines = []
    for n in funcs:
        m, _, a = n
        lib, act = classify(rows, m, a)
        name, size, thumb = g.mods[m].funcs[a]
        stats[(lib, act)] += 1
        stats[("bytes", lib, act)] += size
        sub = subsystem(mm, m, a) if act == "port" else lib
        if act == "port":
            subs[(m == "ov003", sub)] += 1
        lines.append("%-5s %08x %-6s %-7s %-12s %-15s %5d %s%s" % (
            m, a, "thumb" if thumb else "arm", act, lib, sub, size, name, "" if n in base else "  [race]"))
    with open(out, "w") as f:
        f.write("# Race-mode reachable functions: main + itcm + dtcm + ov003, from main(), Entry and the .ctor/.init tables,\n")
        f.write("# following every relocation (calls, function pointers, vtables). Generated by tools/analysis/callgraph.py race.\n")
        f.write("# [race] = only reachable once ov003 is loaded.\n")
        f.write("# subsystem: heuristic tag from build/analysis/game_modules.txt (port) or the library name.\n")
        f.write("# module addr mode action library subsystem size name\n")
        f.write("\n".join(lines) + "\n")
    print("reachable functions: %d (%d only through ov003)" % (len(funcs), sum(1 for n in funcs if n not in base)))
    for (k, v) in sorted(((k, v) for k, v in stats.items() if k[0] != "bytes"), key=lambda kv: -kv[1]):
        print("  %-7s %-14s %5d functions %8d bytes" % (k[1], k[0], v, stats[("bytes",) + k]))
    print("game functions by heuristic subsystem (main | ov003):")
    for sub in sorted({k[1] for k in subs}, key=lambda t: -(subs[(False, t)] + subs[(True, t)])):
        print("  %-15s %5d | %5d" % (sub, subs[(False, sub)], subs[(True, sub)]))
    print("references into modules not loaded during a race:", dict(crossed))
    total = collections.Counter()
    for mn, mod in g.mods.items():
        for a in mod.funcs:
            lib, act = classify(rows, mn, a)
            if act == "port":
                total[mn] += 1
    print("port-action functions per module (all):", dict(total))
    print("written", os.path.relpath(out, ROOT))


if __name__ == "__main__":
    main()
