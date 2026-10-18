"""Groups the ported game code into probable source modules and labels their subsystem.

Each game function gets evidence labels from the named library functions it reaches (directly, or
through up to two unnamed game callees) and from the strings it uses. CodeWarrior emits each source
file as one contiguous block, so consecutive functions are merged into runs; a run breaks where the
dominant label changes. Output: build/analysis/game_modules.txt (one run per line) and a summary.

Usage: python -P tools/analysis/game_modules.py [--min-run N]
"""
import argparse
import json
import os
import re
import sys
from collections import Counter, defaultdict

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import romimage  # noqa: E402

ROOT = romimage.ROOT

# Library name prefix -> subsystem.
CALL_LABELS = [
    (r"^(G3_|G3X_|G3B_|G3C_|G3i_|GXi?_LoadTex|GX_LoadTex|GX_BeginLoadTex|GX_EndLoadTex|GX_LoadClrImage|GX_.*Clear.*3D|texmtxCalc)", "render3d"),
    (r"^(G2_|G2S_|GXS?_LoadBG|GXS?_LoadOBJ|GXS?_.*BG|GXS?_.*OBJ|GX_SetBank|GX_SetGraphicsMode|GXS_SetGraphicsMode|NNS_G2d|NNS_Gfd)", "render2d"),
    (r"^(GX_|GXS_|GXx_)", "display"),
    (r"^(SND|SNDi|NNS_Snd)", "audio"),
    (r"^(FS_|FSi_|CARD|CARDi)", "file_save"),
    (r"^(TP_|PAD_)", "input"),
    (r"^(WM|WMi|MB_|WFS|WBT|CTRDG|VIB)", "wireless_rumble"),
    (r"^(MTX_|VEC_|FX_|FXi_|MATH|CP_)", "math"),
    (r"^(lua_|luaL_|luaopen)", "lua_data"),
    (r"^(unz|inflate)", "archive"),
    (r"^(OS_|OSi_|MI_|MIi_|PXI|RTC|PM_|DC_|IC_|NNS_Fnd)", "system"),
]
STRING_LABELS = [
    (r"(?i)^graphics[/\\]tracks|texturebatch|material\.bin|PieceData|TrackPiece|pillar|CostMem|CostPoly", "track"),
    (r"(?i)graphics[/\\]cars|carParams|lwheel|rwheel|shadow\.n|caraudio|SKID_|CAR_(IDLE|LOW|MED)", "car"),
    (r"(?i)cameras|Cam(Pos|Target|FOV)|Chase\d", "camera"),
    (r"(?i)ghosts/|Ghost", "ghost_replay"),
    (r"(?i)^hud/|HUD_|Lap|Checkpoint|RaceResults|Medal", "hud_race"),
    (r"(?i)FrontEnd|^FE/|Button|Title$|Msg$|Shop|Statistics|Welcome", "frontend"),
    (r"(?i)^Editor/|editor_|Folder|BlockShop", "editor"),
    (r"(?i)Save|CustomData|Slot", "save_ui"),
    (r"(?i)Net|Wifi|HotSeat|Multi|Download", "multiplayer"),
    (r"(?i)^scenes/|\.dcsc|Scene|Environment|TimeOfDay", "scene"),
    (r"(?i)particles/|spray|smoke", "fx_particles"),
    (r"(?i)^strings/|tokens\.dat|\.str$|font", "text"),
]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--min-run", type=int, default=3)
    a = ap.parse_args()

    F = {}
    for line in open(os.path.join(ROOT, "build", "analysis", "functions.jsonl"), encoding="utf-8"):
        f = json.loads(line)
        F[f["addr"]] = f
    libs = json.load(open(os.path.join(ROOT, "build", "analysis", "function_libs.json")))
    placeholder = re.compile(r"^(FUN_|func_|thunk_)")

    def direct_labels(f):
        out = Counter()
        for c in f["callees"]:
            g = F.get(c)
            if not g or libs.get(c) == "game" and placeholder.match(g["name"]):
                continue
            for rx, lab in CALL_LABELS:
                if re.match(rx, g["name"]):
                    out[lab] += 1
                    break
        for s in f["strings"]:
            for rx, lab in STRING_LABELS:
                if re.search(rx, s):
                    out[lab] += 2
                    break
        return out

    own = {k: direct_labels(f) for k, f in F.items() if libs.get(k) == "game"}
    # Two levels of unnamed game callees contribute at lower weight.
    labels = {}
    for k in own:
        tot = Counter({l: 3 * n for l, n in own[k].items()})
        frontier = {c for c in F[k]["callees"] if c in own}
        for depth in (1, 2):
            nxt = set()
            for c in frontier:
                for l, n in own[c].items():
                    tot[l] += n if depth == 1 else 0.5 * n
                nxt |= {x for x in F[c]["callees"] if x in own}
            frontier = nxt - {k}
        labels[k] = tot

    by_block = defaultdict(list)
    for k in own:
        by_block[F[k]["block"]].append(k)
    rows = []
    for blk, ks in by_block.items():
        ks.sort(key=lambda k: int(k.split(":")[-1], 16))
        # Smooth: label of a function = dominant label over a window of 5 neighbours.
        dom = []
        for i in range(len(ks)):
            win = Counter()
            for j in range(max(0, i - 2), min(len(ks), i + 3)):
                win.update(labels[ks[j]])
            dom.append(win.most_common(1)[0][0] if win else "unknown")
        start = 0
        for i in range(1, len(ks) + 1):
            if i == len(ks) or dom[i] != dom[start]:
                run = ks[start:i]
                strings = []
                for k in run:
                    for s in F[k]["strings"]:
                        if s not in strings and not re.fullmatch(r"Error: %s|%s|%d", s):
                            strings.append(s)
                rows.append((blk, run, dom[start], strings))
                start = i

    # Merge short runs into their predecessor so the map stays readable.
    merged = []
    for r in rows:
        if merged and (len(r[1]) < a.min_run or r[2] == merged[-1][2]) and merged[-1][0] == r[0]:
            p = merged[-1]
            merged[-1] = (p[0], p[1] + r[1], p[2] if len(p[1]) >= len(r[1]) else r[2], p[3] + r[3])
        else:
            merged.append(r)

    total = Counter()
    with open(os.path.join(ROOT, "build", "analysis", "game_modules.txt"), "w", encoding="utf-8") as w:
        for blk, run, lab, strings in merged:
            lo = run[0].split(":")[-1]
            hi = run[-1].split(":")[-1]
            size = sum(F[k]["size"] for k in run)
            total[lab] += size
            w.write("%-9s %s..%s %4d funcs %7d bytes  %-16s %s\n" % (
                blk, lo, hi, len(run), size, lab, " | ".join(s[:32] for s in strings[:6])))
    print("runs: %d" % len(merged))
    for lab, size in total.most_common():
        print("  %-16s %7d bytes" % (lab, size))


if __name__ == "__main__":
    main()
