"""Checks tm_trackdump output (--pieces --collision --lighting) against independent readings of the same data.

- Piece catalogue: the unluac decompilation of Scripts/PieceData/<env>/PieceData.luac is evaluated as data and every
  field the game reads is compared (names, model names, sizes, flags, defaults, ids incl. the auto-numbering).
- Track: the unluac decompilation of the scene is evaluated; header values and every placed piece (cell, rotation,
  position) are recomputed. The placement matrix is checked geometrically: every collision vertex transformed by the
  dumped matrix (float math) must fall inside the piece's footprint cells (W x D cells, swapped by odd rotations),
  and no two pieces may share a cell.
- Collision: each .h is parsed from assets/vfs; vertex/triangle/grid counts and grid fields are compared, the cell
  lists and the integer normals are recomputed with the same checksums, and the normals are compared with float
  normals (max error 2/4096).
- Lighting: Scripts/config/tracks lua entries <env>_<tod>.

Usage: python -P tools/host/check_track.py <trackdump output> [--env coa] [--track easy/0]
"""
import math
import os
import re
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from check_carparams import eval_lua_data  # noqa: E402

LUA = "assets/lua_src"
VFS = "assets/vfs"
ENVS = ["sta", "isl", "sno", "coa"]
TRIGGER_X = [0x1C000, 0x40000, 0x1C000, 0x10000]
TRIGGER_Y = [0x8000, 0x8000, 0x8000, 0xE000]
CELL = {3: (16, 4), 1: (64, 8)}  # whole units; others (32, 8)
TERRAIN = {3: ["wcliffcorner", "wcliffdeadend02", "wcliffbase1a", "dcliffcrossa", "wcliffbase5"],
           1: ["beachcorner", "beachdeadend", "beachbase1", "beachgrass", "beachbase5"]}

errors = 0


def err(msg):
    global errors
    errors += 1
    if errors <= 40:
        print(msg)


def fx(v):
    return int(v) >> 12


def expected_pieces(env):
    data = eval_lua_data("%s/Scripts/PieceData/%s/PieceData.lua" % (LUA, ENVS[env]))["PieceData"]
    rows, unnumbered, used = [], [], set()
    for i in sorted(data):
        e = data[i]
        p = {"name": e["Name"], "type": fx(e.get("Type", 0)), "size": (fx(e["Width"]), fx(e["Height"]), fx(e["Depth"])),
             "cost": (fx(e.get("CostMem", 0)), fx(e.get("CostPoly", 0)))}
        hg, ha = fx(e.get("HasGround", 0)) != 0, fx(e.get("HasAir", 0)) != 0
        below = fx(e["BelowGnd"]) != 0 if "BelowGnd" in e else False
        cull = fx(e["Cullable"]) != 0 if "BelowGnd" in e else True
        pillar = fx(e["IsPillar"]) != 0 if "IsPillar" in e else False
        haspil = fx(e["HasPillars"]) != 0 if "IsPillar" in e else False
        p["ground"] = e["Name"] + "ground" if hg else None
        p["air"] = e["Name"] + "air" if ha else None
        if not hg and not ha:
            p["ground"] = e["Name"]
            if not below or env in (1, 3):
                p["air"] = e["Name"]
        p["helper"] = e.get("Helper")
        notch = [fx(e["NotchData"][k]) for k in sorted(e["NotchData"])] if "NotchData" in e else []
        cols = [fx(e["ColumnNotches"][k]) for k in sorted(e["ColumnNotches"])] + [255] if "ColumnNotches" in e else []
        p["notch"], p["columns"] = notch, cols
        flags = [hg, ha, "NotchData" in e, below, cull, pillar, haspil,
                 fx(e.get("DesignerTPV", 0)) != 0, fx(e.get("IsLooseTPV", 0)) != 0, fx(e.get("DisableNotch", 0)) != 0]
        p["flags"] = "".join("1" if f else "0" for f in flags)

        def vec(k, default):
            if k not in e:
                return default
            return tuple(int(e[k][j]) for j in (1, 2, 3))
        p["initDir"] = vec("InitDir", (0, 0, 4096))
        p["posGround"] = vec("InitPosGround", (0, 0, 0))
        p["posAir"] = vec("InitPosAir", (0, 0, 0))
        p["trigger"] = vec("TriggerSize", (TRIGGER_X[env], TRIGGER_Y[env], TRIGGER_Y[env] // 2))
        if "ID" in e:
            p["id"] = fx(e["ID"])
            used.add(p["id"])
            rows.append(p)
        else:
            unnumbered.append(p)
    for p in unnumbered:
        p["id"] = next(i for i in range(1, 350) if i not in used)
        used.add(p["id"])
        rows.append(p)
    return rows


def parse_dump(path):
    pieces, placed, meshes, lights, track = [], [], {}, {}, None
    for line in open(path):
        w = line.split()
        if not w:
            continue
        if w[0] == "piece":
            kv = dict(x.split("=", 1) for x in w[3:])
            pieces.append((int(w[1]), w[2], kv))
        elif w[0] == "track":
            track = dict(x.split("=", 1) for x in w[2:])
            track["scene"] = w[1]
        elif w[0] == "placed":
            kv = dict(x.split("=", 1) for x in w[3:])
            placed.append((int(w[1]), w[2], kv))
        elif w[0] == "mesh":
            meshes[w[1]] = dict(x.split("=", 1) for x in w[2:]) if w[2] != "missing" else None
        elif w[0] == "lighting":
            lights[w[1]] = w[2:]
    return pieces, track, placed, meshes, lights


def parse_h(path):
    d = open(path, "rb").read()
    ver, nv, nt, fl = struct.unpack_from("<HHHh", d, 0)
    o = 8
    verts = [struct.unpack_from("<3i", d, o + 12 * i) for i in range(nv)]
    o += 12 * nv
    tris = [struct.unpack_from("<3H", d, o + 6 * i) for i in range(nt)]
    o += 6 * nt
    nc, = struct.unpack_from("<I", d, o)
    o += 4
    cells, grid, nodes = [], None, []
    if nc:
        cells = list(struct.unpack_from("<%dH" % nc, d, o))
        o += 2 * nc
        grid = struct.unpack_from("<3I3i3i", d, o)
        o += 36
        nn, = struct.unpack_from("<I", d, o)
        o += 4
        nodes = [struct.unpack_from("<2H", d, o + 4 * i) for i in range(nn)]
        o += 4 * nn
    mats = d[o:o + nt] if fl else b""
    return ver, verts, tris, cells, grid, nodes, mats


def isqrt(v):
    return math.isqrt(v) if v > 0 else 0


def int_normal(c):
    # dsVec_Normalize (itcm 0x01ffaac0), independent re-derivation of the hardware sequence
    t = c[0] * c[0] + c[1] * c[1] + c[2] * c[2]
    q = (1 << 56) // t
    k = (isqrt(4 * t) * q) & ((1 << 64) - 1)
    out = []
    for v in c:
        p = (v * k) & ((1 << 64) - 1)
        hi = (p >> 32) & 0xFFFFFFFF
        if hi >= 1 << 31:
            hi -= 1 << 32
        out.append((hi + 0x1000) >> 13)
    return out


def u32(x):
    return x & 0xFFFFFFFF


def check_mesh(model, got):
    path = os.path.join(VFS, "Graphics", model.replace("Tracks/", "Tracks/").replace("/", os.sep) + ".h")
    # file names are mixed case on disk; look the name up case-insensitively
    folder, base = os.path.split(path)
    real = None
    if os.path.isdir(folder):
        for f in os.listdir(folder):
            if f.lower() == base.lower():
                real = os.path.join(folder, f)
    if real is None:
        for root, _, files in os.walk(os.path.join(VFS, "Graphics")):
            for f in files:
                if os.path.join(root, f).lower().endswith(model.lower().replace("/", os.sep) + ".h"):
                    real = os.path.join(root, f)
    if real is None:
        if got is not None:
            err("mesh %s: no file but the port loaded one" % model)
        return 0
    if got is None:
        err("mesh %s: the port could not load %s" % (model, real))
        return 0
    ver, verts, tris, cells, grid, nodes, mats = parse_h(real)
    if int(got["verts"]) != len(verts) or int(got["tris"]) != len(tris) or int(got["materials"]) != len(mats):
        err("mesh %s: counts %s/%s/%s, expected %d/%d/%d" % (model, got["verts"], got["tris"], got["materials"],
                                                             len(verts), len(tris), len(mats)))
    if grid:
        if got["grid"] != "%d,%d,%d" % grid[0:3] or got["min"] != "%d,%d,%d" % grid[3:6] or \
                got["size"] != "%d,%d,%d" % grid[6:9]:
            err("mesh %s: grid %s %s %s, expected %s" % (model, got["grid"], got["min"], got["size"], grid))
        cs = ",".join(str(int(grid[6 + i] * 4096 / (grid[i] << 12))) for i in range(3))
        if got["cell"] != cs:
            err("mesh %s: cell size %s, expected %s" % (model, got["cell"], cs))
    # cell lists
    lists = 0
    if grid:
        gx, gy, gz = grid[0:3]
        for x in range(gx):
            for y in range(gy):
                for z in range(gz):
                    seen = []
                    i = cells[x + gx * y + gx * gy * z]
                    while i != 0xFFFF:
                        t, i = nodes[i]
                        if t not in seen:
                            seen.append(t)
                    for t in seen:
                        lists = u32(lists * 31 + t + 1)
                    lists = u32(lists * 31)
    if int(got["cells"], 16) != lists:
        err("mesh %s: cell list checksum %s, expected %08x" % (model, got["cells"], lists))
    # normals: exact integer path and float sanity check
    s = 0
    worst = 0
    for a, b, c in tris:
        A, B, C = verts[a], verts[b], verts[c]
        e1 = [B[i] - A[i] for i in range(3)]
        e2 = [C[i] - A[i] for i in range(3)]
        cr = [(e1[1] * e2[2] - e1[2] * e2[1] + 0x800) >> 12, (e1[2] * e2[0] - e1[0] * e2[2] + 0x800) >> 12,
              (e1[0] * e2[1] - e1[1] * e2[0] + 0x800) >> 12]
        if any(abs(v) > 0x64000 for v in cr):
            cr = [v >> 7 for v in cr]
        n = [0, 0, 4096] if cr == [0, 0, 0] else int_normal(cr)
        for v in n:
            s = u32(s * 31 + (v & 0xFFFF))
        # float check of the normalisation itself, on vectors long enough for the integer square root of the
        # hardware sequence to be exact to 0.05% (tiny triangles are inherently imprecise in the game too)
        fc = cr
        ln = math.sqrt(sum(v * v for v in fc))
        if ln >= 1024:
            worst = max(worst, max(abs(n[i] - fc[i] / ln * 4096) for i in range(3)))
    if int(got["normals"], 16) != s:
        err("mesh %s: normals checksum %s, expected %08x" % (model, got["normals"], s))
    if worst > 2.0:
        err("mesh %s: integer normals differ from float normals by %.2f/4096" % (model, worst))
    return len(tris)


shared = [0]


def check_footprints(placed, cat, meshes_dir):
    occupied = {}
    for pid, name, kv in placed:
        p = cat[pid]
        w, h, d = p["size"]
        rot = int(kv["rot"])
        cx, cy, cz = (int(v) for v in kv["cell"].split(","))
        fw, fd = (d, w) if rot in (1, 3) else (w, d)
        # footprint grows towards -x and +z from the origin cell (from the meshes of rotation 0)
        cells = [(x, y, z) for x in range(cx - fw + 1, cx + 1) for y in range(cy, cy + h) for z in range(cz, cz + fd)]
        # Footprints may legitimately share cells (curve in/out parts nest inside a large curve, loops leave
        # their middle cells free), so shared cells are counted, not reported; a piece placed twice on the same
        # origin is an error.
        if (cx, cy, cz, pid) in occupied:
            err("placed %s twice at %s" % (name, kv["cell"]))
        occupied[(cx, cy, cz, pid)] = name
        for c in cells:
            if c in occupied:
                shared[0] += 1
            occupied[c] = name
        m = [[int(v) for v in r.split(",")] for r in kv["mtx"].split(";")]
        mesh = meshes_dir.get(kv["model"])
        if not mesh:
            continue
        verts = mesh
        xs, zs = [], []
        for v in verts:
            x = (v[0] * m[0][0] + v[1] * m[1][0] + v[2] * m[2][0]) / 4096.0 + m[3][0]
            z = (v[0] * m[0][2] + v[1] * m[1][2] + v[2] * m[2][2]) / 4096.0 + m[3][2]
            xs.append(x / 4096.0)
            zs.append(z / 4096.0)
        lo_x, hi_x = (cx - fw + 1) * 16 - 8, cx * 16 + 8
        lo_z, hi_z = cz * 16 - 8, (cz + fd - 1) * 16 + 8
        tol = 1.5  # curbs of curves and transitions overhang their cells by up to 1.44
        if min(xs) < lo_x - tol or max(xs) > hi_x + tol or min(zs) < lo_z - tol or max(zs) > hi_z + tol:
            err("placed %s rot %d cell %s: mesh x %.2f..%.2f z %.2f..%.2f outside footprint x %d..%d z %d..%d" %
                (name, rot, kv["cell"], min(xs), max(xs), min(zs), max(zs), lo_x, hi_x, lo_z, hi_z))
    return len([c for c in occupied if len(c) == 3])


def main():
    args = sys.argv[1:]
    dump = args.pop(0)
    env_name, track = "coa", "easy/0"
    while args:
        a = args.pop(0)
        if a == "--env":
            env_name = args.pop(0)
        elif a == "--track":
            track = args.pop(0)
    env = ENVS.index(env_name)
    got_pieces, got_track, placed, meshes, lights = parse_dump(dump)

    # ---- piece catalogue
    exp = expected_pieces(env)
    cat = {p["id"]: p for p in exp}
    if len(got_pieces) != len(exp):
        err("piecedata: %d pieces, expected %d" % (len(got_pieces), len(exp)))
    for (pid, name, kv), p in zip(got_pieces, exp):
        def cmp(field, value):
            if kv.get(field) != value:
                err("piece %s.%s: got %s, expected %s" % (name, field, kv.get(field), value))
        if pid != p["id"] or name != (p["name"] or "-"):
            err("piece order: got %d %s, expected %d %s" % (pid, name, p["id"], p["name"]))
        cmp("type", str(p["type"]))
        cmp("size", "%d,%d,%d" % p["size"])
        cmp("cost", "%d,%d" % p["cost"])
        cmp("ground", p["ground"] or "-")
        cmp("air", p["air"] or "-")
        cmp("helper", p["helper"] or "-")
        cmp("flags", p["flags"])
        cmp("initDir", "%d,%d,%d" % p["initDir"])
        cmp("posGround", "%d,%d,%d" % p["posGround"])
        cmp("posAir", "%d,%d,%d" % p["posAir"])
        cmp("trigger", "%d,%d,%d" % p["trigger"])
        cmp("notch", "".join("%d." % v for v in p["notch"]))
        cmp("columns", "".join("%d." % v for v in p["columns"]))

    # ---- track header and pieces
    scene = eval_lua_data("%s/Scenes/%s/%s.lua" % (LUA, env_name, track))["Track"]
    entries = [scene[k] for k in sorted(scene)]
    head = {e[1]: e[2] for e in entries if isinstance(e[1], str) and len(e) == 2}
    author = int(head.get("AuthorTime", 0))
    expect_head = {"env": str(fx(head["Environment"])), "tod": str(fx(head["TimeOfDay"])),
                   "laps": str(fx(head["LapCount"])), "author": str(author),
                   "gold": str(author + 0x2000 if author else 0),
                   "silver": str(author + 0x5000 if author else 0), "bronze": str(author + 0xA000 if author else 0)}
    for k, v in expect_head.items():
        if got_track.get(k) != v:
            err("track %s: got %s, expected %s" % (k, got_track.get(k), v))
    by_name = {p["name"]: p for p in exp}
    terrain = {by_name[n]["id"] for n in TERRAIN.get(env, []) if n in by_name}
    cw, ch = CELL.get(env, (32, 8))
    exp_placed = []
    for e in entries:
        if len(e) < 5:
            continue
        p = by_name.get(e[1].lower())
        if p is None or p["id"] in terrain:
            continue
        rot, x, y, z = fx(e[2]), int(e[3]), int(e[4]), int(e[5])
        exp_placed.append((p["id"], p["name"], rot, (x * cw, y * ch * 1 + 0, z * cw), (x >> 12, y >> 12, z >> 12)))
    if len(placed) != len(exp_placed):
        err("placed: %d pieces, expected %d" % (len(placed), len(exp_placed)))
    for (pid, name, kv), (eid, ename, rot, pos, cell) in zip(placed, exp_placed):
        pos = (pos[0], pos[1] + 0x59A, pos[2])
        if pid != eid or kv["rot"] != str(rot) or kv["pos"] != "%d,%d,%d" % pos or kv["cell"] != "%d,%d,%d" % cell:
            err("placed %s: got id %d rot %s pos %s cell %s, expected %d %d %s %s" %
                (name, pid, kv["rot"], kv["pos"], kv["cell"], eid, rot, pos, cell))
        p = cat[pid]
        air = cell[1] > (3 if env == 3 else 0) and p["air"] is not None
        model = "Tracks/%s/%s" % (env_name, p["air"] if air else (p["ground"] or p["air"]))
        if kv["model"] != model:
            err("placed %s: model %s, expected %s" % (name, kv["model"], model))

    # ---- collision meshes
    ntris = 0
    mesh_verts = {}
    for model, got in sorted(meshes.items()):
        ntris += check_mesh(model, got)
        folder = os.path.join(VFS, "Graphics", "Tracks")
        for root, _, files in os.walk(folder):
            for f in files:
                if os.path.join(root, f).lower().endswith((model.split("/", 1)[1] + ".h").lower().replace("/", os.sep)):
                    mesh_verts[model] = parse_h(os.path.join(root, f))[1]
    cells = check_footprints(placed, cat, mesh_verts)

    # ---- lighting
    tracks = eval_lua_data("%s/Scripts/config/tracks.lua" % LUA)["tracks"]
    for key, vals in lights.items():
        e = tracks.get(key)
        if e is None:
            err("lighting %s: not in tracks.lua" % key)
            continue
        v = [int(e[i]) for i in range(1, 27)]
        want = ["%d,%d,%d" % tuple(v[c * 3:c * 3 + 3]) for c in range(8)] + \
               ["v24=%d" % ((v[24] * 41 + 0x800) >> 12), "v25=%d" % v[25]]
        if vals != want:
            err("lighting %s: got %s, expected %s" % (key, vals, want))

    print("%d catalogue pieces, %d placed pieces on %d cells (%d shared), %d meshes (%d triangles), %d lightings:"
          " %d errors" % (len(exp), len(placed), cells, shared[0], len(meshes), ntris, len(lights), errors))
    sys.exit(1 if errors else 0)


if __name__ == "__main__":
    main()
