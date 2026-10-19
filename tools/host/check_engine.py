"""Checks tm_physics --engine: gear tables, torque curves and car body boxes.

- Gears and torque are recomputed from the unluac decompilation of Scripts/config/vehicles.luac with the formulas
  of ov003 0x0212ea78 / 0x0212e5c8 (FX_Div rounding the 32.32 quotient, FX_MUL rounding, TopSpeed mph -> m/s with
  x1831/4096), including the original's "1 - t" (integer 1) below a gear's start rpm.
- Boxes come from the BMD0 files read by tools/assets/nitro3d.py (phase 2 parser): size = (boxW, boxH, boxD) x
  posScale, centre = position + size / 2 (z: - size / 2 when the box starts at z >= 1/4096).

Usage: python -P tools/host/check_engine.py <tm_physics --engine output>
"""
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from check_carparams import eval_lua_data  # noqa: E402

VFS = "assets/vfs"


def mul(a, b):
    return (a * b + 0x800) >> 12


def div(a, b):
    if b == 0:
        return 0
    q = abs(a << 32) // abs(b)
    q = q if (a < 0) == (b < 0) else -q
    return (q + (1 << 19)) >> 20


def engine(c):
    rmin, rmax = fx(c["RPMMin"]) << 12, fx(c["RPMMax"]) << 12
    up = rmax - 0x64000
    n = fx(c["NumGears"])
    tops = [mul(int(c["TopSpeedRev"]), 1831)] + [mul(int(c["TopSpeed%d" % i]), 1831) for i in range(1, 7)]
    times = [int(c["TimeRev"])] + [int(c["Time%d" % i]) for i in range(1, 7)]
    gears = [None] * n

    def build(i, rtop, prev):
        top, time = tops[i], times[i]
        ratio = div(top, rtop)
        start = div(prev, ratio) if prev > 0 and ratio else 0
        f = div(rtop - start, rtop)
        torque = div(mul(mul(top, ratio), f), time) if time else 0
        down = start - 0x1F4000 if i > 1 else 0
        gears[i] = [ratio, torque, start, down]
    prev = 0
    for i in range(1, n - 1):
        build(i, up, prev)
        prev = tops[i]
    build(n - 1, rmax, prev)
    ratio = div(tops[0], rmax)
    gears[0] = [ratio, div(mul(mul(tops[0], ratio), div(rmax, rmax)), times[0]) if times[0] else 0, 0, 0]
    return rmin, rmax, up, gears


def torque(e, gear, r):
    rmin, rmax, up, gears = e
    if gear == 0:
        return 0
    g = gears[gear] if gear > 0 else gears[0]
    tq = g[1]
    if r < g[2]:
        t = div(r, g[2]) if g[2] else 0
        a = 1 - t
        tq = mul(tq, 0x1000 - mul(mul(a, a), a))
    elif r >= up:
        t = min(div(r - up, rmax - up), 0x1000)
        tq += mul(-tq, t)
    return tq


def fx(v):
    return int(v) >> 12


def find(path):
    want = path.lower().replace("/", os.sep)
    for root, _, files in os.walk(os.path.join(VFS, "Graphics", "Cars")):
        for f in files:
            p = os.path.join(root, f)
            if p.lower().endswith(want):
                return p
    return None


def box(car, skin):
    p = find("Cars/%s/%d/lod1.n" % (car, skin))
    if not p:
        return None
    d = open(p, "rb").read()
    mdl = struct.unpack_from("<I", d, 0x10)[0]
    ofs = struct.unpack_from("<H", d, mdl + 8 + 6)[0]
    m = mdl + struct.unpack_from("<I", d, mdl + 8 + ofs + 4)[0]
    sys.path.insert(0, "tools/assets")
    import nitro3d
    info = nitro3d.parse_model(d, m, "m")
    s = struct.unpack_from("<i", d, m + 0x1c)[0]  # posScale ("up" in nitro3d)
    assert abs(info.get("up", s / 4096) - s / 4096) < 1e-9 if "up" in info else True
    bb = [round(v * 4096) for v in info["bbox"]]
    size = [mul(bb[3], s), mul(bb[4], s), mul(bb[5], s)]
    z = mul(bb[2], s)
    centre = [mul(bb[0], s) + int(size[0] / 2), mul(bb[1], s) + int(size[1] / 2),
              z + int(size[2] / 2) if z < 1 else z - int(size[2] / 2)]
    return size, centre


def main():
    cars = eval_lua_data("assets/lua_src/Scripts/config/vehicles.lua")["carParams"]
    errors = checked = 0

    def err(m):
        nonlocal errors
        errors += 1
        if errors <= 20:
            print(m)
    eng = {}
    for line in open(sys.argv[1]):
        w = line.split()
        if not w:
            continue
        car = w[1]
        if car not in eng:
            eng[car] = engine(cars[car])
        e = eng[car]
        if w[0] == "engine":
            kv = dict(x.split("=") for x in w[2:])
            want = "%d,%d,%d" % e[:3]
            if kv["rpm"] != want:
                err("%s rpm %s, expected %s" % (car, kv["rpm"], want))
            checked += 1
        elif w[0] == "gear":
            i = int(w[2])
            kv = dict(x.split("=") for x in w[3:])
            g = e[3][i]
            got = [int(kv["ratio"]), int(kv["torque"]), int(kv["start"]), int(kv["down"])]
            if got != g:
                err("%s gear %d: %s, expected %s" % (car, i, got, g))
            checked += 1
        elif w[0] == "torque":
            gear = int(w[2])
            got = [int(v) for v in w[3:]]
            want = [torque(e, gear, r) for r in range(0, e[1] + 0x100000 + 1, 0x80000)]
            if got != want:
                err("%s torque gear %d differs" % (car, gear))
            checked += 1
        elif w[0] == "box":
            b = box(car, int(w[2]))
            got = ([int(v) for v in w[3].split(",")], [int(v) for v in w[4].split(",")])
            if b is None or list(b[0]) != got[0] or list(b[1]) != got[1]:
                err("%s skin %s box %s, expected %s" % (car, w[2], got, b))
            checked += 1
    print("%d cars, %d lines checked, %d errors" % (len(eng), checked, errors))
    sys.exit(1 if errors else 0)


if __name__ == "__main__":
    main()
