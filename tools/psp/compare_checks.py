"""Compares the check suite's dumps of two runs byte for byte (host tm_checksuite vs PSP tm_checks in PPSSPP).

Usage: python -P tools/psp/compare_checks.py <host dir> <psp dir> <checks.txt>
Prints one line per differing dump (first differing line), the exit codes and timings of both logs, and a total.
"""
import os
import sys


def read_log(path):
    out = {}
    for line in open(path):
        w = line.split()
        if len(w) >= 3 and w[0] != "done":
            out[w[0]] = (int(w[1]), int(w[2]))
    return out


def main():
    host, psp, listing = sys.argv[1:4]
    names = [line.split()[0] for line in open(listing) if line.strip()]
    hlog, plog = read_log(os.path.join(host, "suite.log")), read_log(os.path.join(psp, "suite.log"))
    diffs = 0
    for n in names:
        a, b = os.path.join(host, n + ".txt"), os.path.join(psp, n + ".txt")
        if not os.path.exists(b):
            print("%-24s missing on the PSP" % n)
            diffs += 1
            continue
        da, db = open(a, "rb").read(), open(b, "rb").read()
        if hlog.get(n, (None,))[0] != plog.get(n, (None,))[0]:
            print("%-24s exit code host %s psp %s" % (n, hlog.get(n, ("?",))[0], plog.get(n, ("?",))[0]))
            diffs += 1
        if da != db:
            la, lb = da.split(b"\n"), db.split(b"\n")
            k = next((i for i in range(min(len(la), len(lb))) if la[i] != lb[i]), min(len(la), len(lb)))
            print("%-24s differs at line %d (host %d lines, psp %d)" % (n, k + 1, len(la), len(lb)))
            print("    host: %s" % (la[k][:150] if k < len(la) else b"<eof>"))
            print("    psp:  %s" % (lb[k][:150] if k < len(lb) else b"<eof>"))
            diffs += 1
    th = sum(v[1] for v in hlog.values())
    tp = sum(v[1] for v in plog.values())
    size = sum(os.path.getsize(os.path.join(host, n + ".txt")) for n in names)
    print("%d dumps (%d bytes), %d differences; time host %d ms, psp %d ms (PPSSPP)" % (len(names), size, diffs, th,
                                                                                      tp))
    return 1 if diffs else 0


if __name__ == "__main__":
    sys.exit(main())
