"""Applies config/code_map.txt to build/analysis/functions.jsonl and prints per-library / per-action totals.
Writes build/analysis/function_libs.json ({addr: library}). Usage: python -P tools/analysis/code_map.py"""
import json
import os
from collections import defaultdict

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))


def load_map():
    rows = []
    for line in open(os.path.join(ROOT, "config", "code_map.txt"), encoding="utf-8"):
        line = line.split("#", 1)[0].split()
        if len(line) == 5:
            blk, lo, hi, lib, action = line
            rows.append((blk, int(lo, 16), int(hi, 16), lib, action))
    return rows


def classify(rows, f):
    a = int(f["addr"].split(":")[-1], 16)
    for blk, lo, hi, lib, action in rows:
        if blk == f["block"] and lo <= a < hi:
            return lib, action
    return "unmapped", "?"


def main():
    rows = load_map()
    F = [json.loads(l) for l in open(os.path.join(ROOT, "build", "analysis", "functions.jsonl"), encoding="utf-8")]
    libs = {}
    by_lib = defaultdict(lambda: [0, 0])
    by_action = defaultdict(lambda: [0, 0])
    for f in F:
        lib, action = classify(rows, f)
        libs[f["addr"]] = lib
        for d, k in ((by_lib, (lib, action)), (by_action, action)):
            d[k][0] += 1
            d[k][1] += f["size"]
    json.dump(libs, open(os.path.join(ROOT, "build", "analysis", "function_libs.json"), "w"))
    print("%-14s %-8s %6s %9s" % ("library", "action", "funcs", "bytes"))
    for (lib, action), (n, size) in sorted(by_lib.items(), key=lambda kv: (kv[0][1], -kv[1][1])):
        print("%-14s %-8s %6d %9d" % (lib, action, n, size))
    print()
    for action, (n, size) in sorted(by_action.items()):
        print("total %-8s %6d functions %9d bytes" % (action, n, size))


if __name__ == "__main__":
    main()
