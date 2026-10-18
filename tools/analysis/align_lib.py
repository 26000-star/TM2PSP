"""Names an embedded library's functions by aligning source order with address order.

CodeWarrior keeps the definition order of each source file and links the library's objects in a
fixed order (alphabetical for Lua 5.0.2). Unreferenced functions are dead-stripped and some statics
are inlined, so the two sequences only partly line up. The script therefore:
  1. takes high-confidence anchors: luaL_reg table names, and string literals unique to one
     function on both sides (an anchor must also respect the global order);
  2. fills each gap between consecutive anchors one-to-one when both sides have the same number of
     functions;
  3. reports a consistency score: the fraction of matched DS call edges whose callee name is also a
     callee in the source call graph (gcc objects, see match_lib.py).

Usage:
  python -P tools/analysis/align_lib.py <src dir> <obj dir> <block> <start> <end> <out.txt>
         --order f1.c,f2.c,... [--tables build/analysis/reg_tables.txt]
"""
import argparse
import json
import os
import re
from collections import defaultdict

import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import match_lib  # noqa: E402

ROOT = match_lib.ROOT


def source_sequence(src_dir, order, reverse=False):
    """[(name, file, strings)] in link order: files as given, functions by definition order
    (reversed within each file when the compiler emits them backwards, as CodeWarrior does)."""
    seq = []
    for fname in order:
        path = None
        for base, _, files in os.walk(src_dir):
            if fname in files:
                path = os.path.join(base, fname)
        if not path:
            raise SystemExit("missing source file " + fname)
        text = open(path, encoding="latin1").read()
        funcs = match_lib.source_functions(os.path.dirname(path))
        file_seq = []
        for m in match_lib.FUNC_DEF.finditer(text):
            n = m.group(1)
            if n in funcs and funcs[n]["file"] == fname and n not in ("if", "while", "for", "switch"):
                file_seq.append((n, fname, funcs[n]["strings"]))
        seq.extend(reversed(file_seq) if reverse else file_seq)
    # A name can be defined twice under #if; keep the first.
    seen, out = set(), []
    for item in seq:
        if item[0] not in seen:
            seen.add(item[0])
            out.append(item)
    return out


def longest_increasing(pairs):
    """Largest subset of (src_index, ds_index) pairs increasing in both coordinates."""
    pairs = sorted(pairs)
    best = []  # best[i] = (length, prev) ending at pairs[i]
    for i, (s, d) in enumerate(pairs):
        cand = (1, -1)
        for j in range(i):
            if pairs[j][0] < s and pairs[j][1] < d and best[j][0] + 1 > cand[0]:
                cand = (best[j][0] + 1, j)
        best.append(cand)
    if not best:
        return []
    i = max(range(len(best)), key=lambda k: best[k][0])
    out = []
    while i >= 0:
        out.append(pairs[i])
        i = best[i][1]
    return out[::-1]


def main():
    ap = argparse.ArgumentParser()
    for x in ("src", "obj", "block", "start", "end", "out"):
        ap.add_argument(x)
    ap.add_argument("--order", required=True)
    ap.add_argument("--tables")
    ap.add_argument("--reverse", action="store_true", help="functions are emitted in reverse order per file")
    a = ap.parse_args()
    lo, hi = int(a.start, 16), int(a.end, 16)
    order = [x.strip() for x in a.order.split(",") if x.strip()]

    src = source_sequence(a.src, order, a.reverse)
    sidx = {n: i for i, (n, _, _) in enumerate(src)}
    D = []
    for line in open(os.path.join(ROOT, "build", "analysis", "functions.jsonl"), encoding="utf-8"):
        f = json.loads(line)
        ad = int(f["addr"].split(":")[-1], 16)
        if f["block"] == a.block and lo <= ad < hi:
            D.append(f)
    D.sort(key=lambda f: int(f["addr"].split(":")[-1], 16))
    didx = {f["addr"]: i for i, f in enumerate(D)}

    # 1. Anchors.
    cand = {}
    if a.tables:
        names = match_lib.reg_tables(a.src)
        for line in open(a.tables):
            m = re.match(r"\s+\S+:([0-9a-f]{8}) (\S+)$", line)
            if m and m.group(1) in didx and m.group(2) in names:
                cs = [n for n in names[m.group(2)] if n in sidx]
                if len(cs) == 1:
                    cand[(sidx[cs[0]], didx[m.group(1)])] = "table:" + m.group(2)
    s_by_str, d_by_str = defaultdict(set), defaultdict(set)
    for i, (_, _, strings) in enumerate(src):
        for s in strings:
            if len(s) >= 4:
                s_by_str[s].add(i)
    for i, f in enumerate(D):
        for s in f["strings"]:
            d_by_str[s].add(i)
    for s, si in s_by_str.items():
        if len(si) == 1 and len(d_by_str.get(s, ())) == 1:
            cand.setdefault((next(iter(si)), next(iter(d_by_str[s]))), "string:" + s[:40])
    # Several anchors can disagree on one function; keep only pairs consistent with global order.
    by_s, by_d = defaultdict(set), defaultdict(set)
    for s, d in cand:
        by_s[s].add(d)
        by_d[d].add(s)
    unique = [p for p in cand if len(by_s[p[0]]) == 1 and len(by_d[p[1]]) == 1]
    anchors = longest_increasing(unique)
    match = {s: (d, cand[(s, d)]) for s, d in anchors}

    scall = match_lib.source_calls(a.obj, {n for n, _, _ in src})
    s_callees = {i: {sidx[c] for c in scall.get(n, ()) if c in sidx} for i, (n, _, _) in enumerate(src)}
    s_callers = defaultdict(set)
    for i, cs in s_callees.items():
        for c in cs:
            s_callers[c].add(i)
    d_callees = {i: {didx[c] for c in f["callees"] if c in didx} for i, f in enumerate(D)}
    d_callers = defaultdict(set)
    for i, cs in d_callees.items():
        for c in cs:
            d_callers[c].add(i)

    def bounds_of(match):
        return [(-1, -1)] + sorted((s, d) for s, (d, _) in match.items()) + [(len(src), len(D))]

    def gap_fill(match):
        n = 0
        b = bounds_of(match)
        for (s0, d0), (s1, d1) in zip(b, b[1:]):
            ns, nd = s1 - s0 - 1, d1 - d0 - 1
            if ns == nd and ns > 0:
                for k in range(ns):
                    match[s0 + 1 + k] = (d0 + 1 + k, "gap %s..%s" % (
                        src[s0][0] if s0 >= 0 else "start", src[s1][0] if s1 < len(src) else "end"))
                    n += 1
        return n

    def gap_ids(match):
        """Gap number of every unmatched source and DS index."""
        b = bounds_of(match)
        sg, dg = {}, {}
        for g, ((s0, d0), (s1, d1)) in enumerate(zip(b, b[1:])):
            for s in range(s0 + 1, s1):
                sg[s] = g
            for d in range(d0 + 1, d1):
                dg[d] = g
        return sg, dg

    # 2. Gap filling, then call-graph propagation constrained to gaps, until nothing changes.
    filled = gap_fill(match)
    propagated = 0
    while True:
        sg, dg = gap_ids(match)
        new = {}
        for s, (d, _) in list(match.items()):
            for sgraph, dgraph, kind in ((s_callees, d_callees, "callee"), (s_callers, d_callers, "caller")):
                su, du = defaultdict(set), defaultdict(set)
                for x in sgraph.get(s, ()):
                    if x in sg:
                        su[sg[x]].add(x)
                for x in dgraph.get(d, ()):
                    if x in dg:
                        du[dg[x]].add(x)
                for g in su:
                    if len(su[g]) == 1 and len(du.get(g, ())) == 1:
                        x, y = next(iter(su[g])), next(iter(du[g]))
                        new.setdefault(x, set()).add((y, "%s of %s" % (kind, src[s][0])))
        added = 0
        taken = {d for d, _ in match.values()}
        cands = [(x, y, r) for x, ys in new.items() if len({y for y, _ in ys}) == 1 for y, r in [next(iter(ys))]]
        keep = set(longest_increasing([(x, y) for x, y, _ in cands if y not in taken]))
        for x, y, r in cands:
            if (x, y) in keep and x not in match:
                match[x] = (y, r)
                added += 1
        propagated += added
        filled += gap_fill(match)
        if not added:
            break

    # 3. Consistency of matched call edges.
    name_of = {D[d]["addr"]: src[s][0] for s, (d, _) in match.items()}
    ok = bad = 0
    for s, (d, _) in match.items():
        for c in D[d]["callees"]:
            if c in name_of:
                if name_of[c] in scall.get(src[s][0], ()):
                    ok += 1
                else:
                    bad += 1

    with open(a.out, "w") as w:
        for s, (d, why) in sorted(match.items(), key=lambda kv: kv[1][0]):
            w.write("%s %s %s # %s\n" % (D[d]["addr"], src[s][0], src[s][1], why))
    print("source %d, DS %d, anchors %d, gap-filled %d, propagated %d, total %d" % (
        len(src), len(D), len(anchors), filled, propagated, len(match)))
    print("matched call edges consistent %d, inconsistent %d" % (ok, bad))
    # Gap report: where the two sequences disagree in length.
    b = bounds_of(match)
    for (s0, d0), (s1, d1) in zip(b, b[1:]):
        ns, nd = s1 - s0 - 1, d1 - d0 - 1
        if ns != nd:
            print("  gap after %-22s src %3d ds %3d  (%s)" % (src[s0][0] if s0 >= 0 else "start", ns, nd,
                  D[d0 + 1]["addr"] if 0 <= d0 + 1 < len(D) else "-"))


if __name__ == "__main__":
    main()
