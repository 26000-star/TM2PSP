"""Checks `tm_cfgdump --cameras` against the unluac decompilation of Scripts/config/cameras and the ROM.

  - every environment's Chase1/Chase2 fields as read by main 0x0208fa18 (RollMode as an integer, missing keys 0);
  - the spring damping lookups (main 0x020abc68) recomputed from the (stiffness, damping) table read from the ROM
    (0x020c1d08, count 0x020c1d04): linear inside, first segment extended below, middle-to-last segment above;
    within 1 + |dy| / 4096 (t is an fx32 quotient).
Usage: python -P tools/host/check_cameras.py <dump>
"""
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(HERE, "..", "analysis"))
from check_carparams import eval_lua_data  # noqa: E402
import romimage  # noqa: E402

FIELDS = ["Height", "Distance", "TiltTightness", "LookAtHeight", "FOV", "DirTightness", "DistInc", "DistDec",
          "RollMode", "DistTightness"]


def main():
    cams = eval_lua_data("assets/lua_src/Scripts/config/cameras.lua")["cameras"]
    m = romimage.load()["main"]
    n = m.u32(0x020C1D04)
    table = [(m.u32(0x020C1D08 + 8 * i), m.u32(0x020C1D0C + 8 * i)) for i in range(n)]

    def damping(x):
        if x < table[0][0]:
            (x0, y0), (x1, y1) = table[0], table[1]
            return y0 + (y1 - y0) * (x - x0) / (x1 - x0), 1 + abs(y1 - y0) / 4096
        for i in range(n - 1):
            if x < table[i + 1][0]:
                (x0, y0), (x1, y1) = table[i], table[i + 1]
                return y0 + (y1 - y0) * (x - x0) / (x1 - x0), 1 + abs(y1 - y0) / 4096
        (xm, ym), (xl, yl) = table[n // 2], table[-1]
        return yl + (yl - ym) * (x - xl) / (xl - xm), 1 + abs(yl - ym) / 4096

    errors, seen, nd = [], set(), 0
    for line in open(sys.argv[1]):
        w = line.split()
        if w and w[0] == "camera":
            env, kind = w[1], int(w[2])
            seen.add((env, kind))
            got = dict(x.split("=") for x in w[3:])
            e = cams[env]
            for f in FIELDS:
                v = int(e.get("Chase%d%s" % (kind, f), 0))
                if f == "RollMode":
                    v >>= 12
                if int(got[f]) != v:
                    errors.append("%s Chase%d%s: %s, expected %d" % (env, kind, f, got[f], v))
            exp, tol = damping(int(got["DistTightness"]))
            if abs(int(got["damping"]) - exp) > tol:
                errors.append("%s %d damping %s" % (env, kind, got["damping"]))
        elif w and w[0] == "damping":
            nd += 1
            x, y = int(w[1]), int(w[2])
            exp, tol = damping(x)
            if abs(y - exp) > tol:
                errors.append("damping(%d) = %d, expected %.1f" % (x, y, exp))
    if seen != {(e, k) for e in cams for k in (1, 2)}:
        errors.append("cameras dumped: %s" % sorted(seen))
    for e in errors[:40]:
        print("ERROR", e)
    print("%d cameras, %d damping lookups, %d errors" % (len(seen), nd, len(errors)))
    return 1 if errors else 0


if __name__ == "__main__":
    sys.exit(main())
