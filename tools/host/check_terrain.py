"""Checks `tm_trackdump --terrain` (dsTrack::BuildTerrain, ov003 0x02100998) for Coast tracks.

Independent pieces: the scene and the piece catalogue come from the unluac decompilations (check_track.py), the
neighbourhood patterns, quarter-turn tables and terrain piece names are read from the ROM's ov003 data
(0x02134e8c, 0x02134e68, 0x02135bcc), and the selection is re-implemented here from the disassembly.
Usage: python -P tools/host/check_terrain.py <dump> [--track easy/0]
"""
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(HERE, "..", "analysis"))
import check_track  # noqa: E402
import romimage  # noqa: E402

COA = 3
GROUND_LEVEL, GRID, CELL = 3, (22, 20, 22), 16


def rom_tables():
    mods = romimage.load()
    m = [x for x in (mods.values() if isinstance(mods, dict) else mods) if x.name == "ov003"][0]
    b = lambda a, n: bytes(m.data[a - m.base:a - m.base + n])
    pats = [list(b(0x02134E8C + 9 * i, 9)) for i in range(15)]
    turns = [list(b(0x02134E68 + 9 * i, 9)) for i in range(4)]
    names = [m.cstring(m.u32(0x02135BCC + COA * 0x18 + 4 * i)) for i in range(6)]
    return pats, turns, names


def main():
    args = sys.argv[1:]
    dump = args.pop(0)
    track = args.pop(1) if len(args) > 1 and args[0] == "--track" else "easy/0"
    pats, turns, names = rom_tables()
    cat = {p["id"]: p for p in check_track.expected_pieces(COA)}
    by_name = {p["name"]: p for p in cat.values()}
    ids = [by_name[n]["id"] for n in names]
    base_rot = [0, 0, 3, 0, 0, 0]
    terrain_ids = set(ids)

    placed, got = [], []
    for line in open(dump):
        w = line.split()
        if w and w[0] == "placed":
            kv = dict(x.split("=", 1) for x in w[3:])
            placed.append((int(w[1]), [int(v) for v in kv["cell"].split(",")], int(kv["rot"])))
        elif w and w[0] == "terrainpiece":
            kv = dict(x.split("=", 1) for x in w[3:])
            got.append((int(w[1]), int(kv["rot"]), tuple(int(v) for v in kv["cell"].split(",")), kv["pos"]))

    def solid_cells():
        cells = set()
        for pid, (cx, cy, cz), rot in placed:
            p = cat[pid]
            if pid in terrain_ids:
                continue
            w, h, d = p["size"]
            if rot & 1:
                w, d = d, w
            below = p["flags"][3] == "1"
            if below or not p["ground"]:
                continue
            for x in range(cx - w + 1, cx + 1):
                for z in range(cz, cz + d):
                    if cy <= GROUND_LEVEL < cy + h:
                        cells.add((x, z))
        return cells

    def occupied():
        cells = set()
        for pid, (cx, cy, cz), rot in placed:
            w, h, d = cat[pid]["size"]
            if rot & 1:
                w, d = d, w
            for x in range(cx - w + 1, cx + 1):
                for z in range(cz, cz + d):
                    if cy <= GROUND_LEVEL < cy + h:
                        cells.add((x, z))
        return cells

    solid, occ = solid_cells(), occupied()
    exp = []
    for x in range(GRID[0]):
        for z in range(GRID[2]):
            if (x, z) in occ:
                continue
            near = [(i, k) for i in range(max(x - 1, 0), min(x + 1, GRID[0] - 1) + 1)
                    for k in range(max(z - 1, 0), min(z + 1, GRID[2] - 1) + 1)]
            if not any(c in solid for c in near):
                continue
            mask = [1 if (x + dx, z + dz) in solid else 0 for dz in (-1, 0, 1) for dx in (-1, 0, 1)]
            hit = None
            for pat in pats:
                for r in range(4):
                    if all(pat[i] == 255 or i == 4 or mask[turns[r][i]] == pat[i] for i in range(9)):
                        hit = (ids[pat[4]], (base_rot[pat[4]] + r) & 3)
                        break
                if hit:
                    break
            if hit:
                pos = "%d,%d,%d" % (x * CELL << 12, 1434, z * CELL << 12)
                exp.append((hit[0], hit[1], (x, 0, z), pos))
    errors = 0
    if sorted(exp) != sorted(got):
        for e in sorted(set(exp) - set(got)):
            print("ERROR missing", e)
            errors += 1
        for g in sorted(set(got) - set(exp)):
            print("ERROR extra", g)
            errors += 1
    print("%s: %d terrain pieces, %d errors" % (track, len(got), errors))
    return 1 if errors else 0


if __name__ == "__main__":
    sys.exit(main())
