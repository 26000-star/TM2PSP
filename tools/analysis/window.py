"""Lists functions in an address window: address, mode, size, tags, callers, first strings.
Usage: python -P tools/analysis/window.py <block> <start hex> <end hex>"""
import json, os, sys
ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
blk, lo, hi = sys.argv[1], int(sys.argv[2], 16), int(sys.argv[3], 16)
tags = json.load(open(os.path.join(ROOT, "build", "analysis", "tags.json")))
for l in open(os.path.join(ROOT, "build", "analysis", "functions.jsonl"), encoding="utf-8"):
    f = json.loads(l)
    a = int(f["addr"].split(":")[-1], 16)
    if f["block"] == blk and lo <= a < hi:
        print("%08x %s %5d c%-3d %-22s %s" % (a, "T" if f["thumb"] else "A", f["size"], f["callers"],
              ",".join(tags[f["addr"]])[:22], " | ".join(s[:30] for s in f["strings"][:3])))
