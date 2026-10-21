"""Checks `tm_trackdump --triggers` (race triggers, start point and gate probes of one track, any environment).

Independent of the C++ code: the piece catalogue comes from the unluac decompilation (check_track.expected_pieces);
the placed pieces and their matrices come from the dump's "placed" lines, which check_track.py validates.
  - one "finish" trigger per finish/multilap piece, then one "cp<n>" per checkpoint, in piece order; the start piece
    of a lap track takes the next checkpoint number;
  - trigger matrix = piece matrix with row 0 x TriggerSize.x, row 1 x TriggerSize.y, translation + InitPos turned
    by the piece rows (truncating) + TriggerSize.z on y; gate centre, unit normal (row 2) and radius
    (half the diagonal of rows 0 and 1), within 2/4096;
  - start point = piece position + InitPos and InitDir, turned with the SDK sine table; Coast moves a 2-cell wide
    piece turned 2 or 3 quarters 16 along x, Island 32 along x for that, and 32 along z for a 2-cell deep piece turned
    0 or 3 quarters (ov003 0x02114a6c);
  - probes: crossing the plane one unit each side of local (u, v) hits iff |u| and |v| < 0.55 and the point is
    within the radius of the centre, both ways, and never twice in a row.
Usage: python -P tools/host/check_triggers.py <dump>
"""
import math
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import check_track  # noqa: E402

def sincos(rot):
    a = rot * 6434
    idx = (0xFFFF * (a % 25736) // 25736) >> 4
    return round(math.sin(idx * 2 * math.pi / 4096) * 4096), round(math.cos(idx * 2 * math.pi / 4096) * 4096)


def mul33(v, rows):
    return [(v[0] * rows[0][j] + v[1] * rows[1][j] + v[2] * rows[2][j]) >> 12 for j in range(3)]


def rnd(a, b):
    return (a * b + 0x800) >> 12


def main():
    lines = open(sys.argv[1]).read().splitlines()
    head = next(dict(x.split("=", 1) for x in ln.split()[2:]) for ln in lines if ln.startswith("track "))
    env = int(head["env"])
    cat = {p["id"]: p for p in check_track.expected_pieces(env)}
    placed, trig, probes, race, laps = [], [], {}, None, None
    for line in lines:
        w = line.split()
        if not w:
            continue
        if w[0] == "track":
            laps = int(dict(x.split("=", 1) for x in w[2:])["laps"])
        elif w[0] == "placed":
            kv = dict(x.split("=", 1) for x in w[3:])
            kv["id"] = int(w[1])
            kv["mtx"] = [[int(c) for c in r.split(",")] for r in kv["mtx"].split(";")]
            placed.append(kv)
        elif w[0] == "race":
            race = dict(x.split("=", 1) for x in w[1:])
        elif w[0] == "trigger":
            kv = dict(x.split("=", 1) for x in w[3:])
            kv["k"], kv["name"] = int(w[1]), w[2]
            trig.append(kv)
        elif w[0] == "probe":
            kv = dict(x.split("=", 1) for x in w[2:])
            probes.setdefault(int(w[1]), []).append({k: int(v) for k, v in kv.items()})
    errors = []

    def close(a, b, tol, what):
        if any(abs(x - y) > tol for x, y in zip(a, b)):
            errors.append("%s: got %s, expected %s" % (what, a, b))

    exp = []
    for i, p in enumerate(placed):
        if int(p["type"]) in (2, 4):
            exp.append(("finish", i, -1))
    n = 0
    for i, p in enumerate(placed):
        if int(p["type"]) == 3:
            exp.append(("cp%d" % n, i, n))
            n += 1
    start = next((i for i, p in enumerate(placed) if int(p["type"]) in (1, 4)), -1)
    if start >= 0 and int(placed[start]["type"]) == 4:
        n += 1
    got = [(t["name"], int(t["piece"]), int(t["order"])) for t in trig]
    if got != exp:
        errors.append("trigger list %s, expected %s" % (got, exp))
    if race is None or int(race["checkpoints"]) != n or int(race["laps"]) != laps or int(race["start"]) != start:
        errors.append("race line %s, expected checkpoints=%d laps=%d start=%d" % (race, n, laps, start))

    for t in trig:
        p = placed[int(t["piece"])]
        info = cat[p["id"]]
        m = p["mtx"]
        ts = info["trigger"]
        off = mul33(info["posAir"] if p["air"] == "1" else info["posGround"], m)
        e = [[rnd(c, ts[0]) for c in m[0]], [rnd(c, ts[1]) for c in m[1]], list(m[2]),
             [m[3][0] + off[0], m[3][1] + off[1] + ts[2], m[3][2] + off[2]]]
        gm = [[int(c) for c in r.split(",")] for r in t["mtx"].split(";")]
        tol = 2
        if env == 0 and p["id"] == 0x10:
            # Stadium's horizontal ring: the three rows turned a quarter about row 0 (ov003 0x021114e4); the
            # direction is taken as the one closer to the dump, with a looser tolerance for the fx32 rotation.
            l0 = math.sqrt(sum(c * c for c in e[0])) or 1.0
            a = [c / l0 for c in e[0]] if any(e[0]) else [0.0, 0.0, 1.0]

            s, c = sincos(1)  # angle 6432 = one quarter turn: the SDK table's sin 4096, cos 6
            s, c = s / 4096.0, c / 4096.0

            def turn(v, sn):
                dot = sum(a[i] * v[i] for i in range(3))
                cr = [a[1] * v[2] - a[2] * v[1], a[2] * v[0] - a[0] * v[2], a[0] * v[1] - a[1] * v[0]]
                return [round(v[i] * c + cr[i] * s * sn + a[i] * dot * (1 - c)) for i in range(3)]
            cands = [[turn(e[r], sn) for r in range(3)] for sn in (1, -1)]
            best = min(cands, key=lambda c: sum(abs(c[r][j] - gm[r][j]) for r in range(3) for j in range(3)))
            e = best + [e[3]]
            tol = 8
        for r in range(4):
            close(gm[r], e[r], tol, "%s row %d" % (t["name"], r))
        close([int(c) for c in t["centre"].split(",")], e[3], 2, t["name"] + " centre")
        l2 = math.sqrt(sum(c * c for c in e[2]))
        close([int(c) for c in t["normal"].split(",")], [round(c * 4096 / l2) for c in e[2]], tol,
              t["name"] + " normal")
        r0 = math.sqrt(sum(c * c for c in e[0]))
        r1 = math.sqrt(sum(c * c for c in e[1]))
        close([int(t["radius"])], [round(0.5 * math.hypot(r0, r1))], 2, t["name"] + " radius")
        pr = probes.get(t["k"], [])
        if len(pr) != 49:
            errors.append("%s: %d probes" % (t["name"], len(pr)))
        for q in pr:
            dist = math.sqrt((q["u"] * r0 / 4096) ** 2 + (q["v"] * r1 / 4096) ** 2 + 4096 ** 2)
            if abs(dist - int(t["radius"])) < 4:
                continue  # on the sphere: rounding decides
            inside = abs(q["u"]) < 2253 and abs(q["v"]) < 2253 and dist <= int(t["radius"])
            if q["fwd"] != inside or q["back"] != inside or q["again"]:
                errors.append("%s probe u=%d v=%d: %s, inside=%s" % (t["name"], q["u"], q["v"], q, inside))

    if start >= 0:
        p = placed[start]
        info = cat[p["id"]]
        rot = int(p["rot"])
        s, c = sincos(rot)
        rows = [[c, 0, -s], [0, 4096, 0], [s, 0, c]]
        o = list(info["posAir"] if p["air"] == "1" else info["posGround"])
        if env == 3 and info["size"][0] == 2 and rot in (2, 3):
            o[0] += 0x10000
        elif env == 1:
            if info["size"][0] == 2 and rot in (2, 3):
                o[0] += 0x20000
            if info["size"][2] == 2 and rot in (0, 3):
                o[2] += 0x20000
        o = mul33(o, rows)
        pos = [int(v) for v in p["pos"].split(",")]
        close([int(v) for v in race["startpos"].split(",")], [pos[j] + o[j] for j in range(3)], 1, "start pos")
        close([int(v) for v in race["startdir"].split(",")], mul33(info["initDir"], rows), 1, "start dir")

    for e in errors[:40]:
        print("ERROR", e)
    print("%d triggers, %d probes, %d errors" % (len(trig), sum(len(v) for v in probes.values()), len(errors)))
    return 1 if errors else 0


if __name__ == "__main__":
    sys.exit(main())
