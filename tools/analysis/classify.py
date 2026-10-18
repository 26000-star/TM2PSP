"""Tags every function with the evidence it carries (hardware registers, library strings) and prints
the main ARM9 / overlay address space as runs, to locate library boundaries in link order.

Input: build/analysis/functions.jsonl (tools/ghidra/export.ps1).
Output: build/analysis/tags.json and a run map on stdout.
"""
import json
import os
import re
import sys

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))

# DS ARM9 I/O map -> subsystem tag (GBATEK).
IO = [
    (0x04000000, 0x04000060, "gx2d_a"), (0x04000060, 0x04000064, "g3x"), (0x04000064, 0x0400006c, "gx_capture"),
    (0x0400006c, 0x04000070, "gx2d_a"), (0x040000b0, 0x040000e0, "dma"), (0x040000e0, 0x040000f0, "dma"),
    (0x04000100, 0x04000110, "timer"), (0x04000130, 0x04000138, "pad"), (0x04000180, 0x04000190, "pxi"),
    (0x040001a0, 0x040001c0, "card"), (0x04000204, 0x04000208, "mi"), (0x04000208, 0x04000220, "irq"),
    (0x04000240, 0x0400024a, "vram"), (0x04000280, 0x040002c0, "cp_math"), (0x04000300, 0x04000304, "os"),
    (0x04000304, 0x04000308, "gx_power"), (0x04000320, 0x04000400, "g3x"), (0x04000400, 0x04000440, "g3_fifo"),
    (0x04000440, 0x04000600, "g3_cmd"), (0x04000600, 0x040006a4, "g3x"), (0x04001000, 0x04001070, "gx2d_b"),
    (0x04100000, 0x04100004, "pxi"), (0x04100010, 0x04100014, "card"),
]

# Strings that only a given library uses.
LIBS = [
    ("lua", re.compile(r"attempt to |stack overflow|Lua 5\.0|`%s'|bad argument|C stack overflow|"
                       r"function or expression too complex|unexpected symbol|chunk has too many|"
                       r"__index|__newindex|__gc|__mode|__eq|__call|__concat|__unm|_LOADED|_G\b|"
                       r"bad code in|unknown number format|too old: read|too new: read")),
    ("unzip", re.compile(r"unzip 0\.15|Gilles Vollant|inflate |incorrect header check|invalid block type|"
                         r"incorrect data check|invalid distance")),
    ("dwc", re.compile(r"DWC|nintendowifi|gamespy|GameSpy|HTTP/1\.|Content-|multipart|application/|"
                       r"\.gs\.|\bgp[A-Z]|\bgt2|NDWC|wayport|/web/client")),
    ("wfc_util", re.compile(r"\.bmg\.l$|\.nce\.l$|\.ncg\.l$|\.ncl\.l$|\.nsc\.l$|\.nan\.l$")),
    ("nitro_sdk", re.compile(r"\[SDK\+|NITRO|OS_|FS_|CARD_|MI_|GX_|SND_|WM_|CTRDG|ROM_")),
    ("game_data", re.compile(r"(?i)graphics[/\\]|ghosts/|scenes/|scripts/|tracks/|\.n$|\.h$|\.luac?$|"
                             r"\.dcsc?$|strings/|tokens\.dat|vfs\.|SEQARC_|carParams|PieceData|Track\b")),
]


def io_tag(addr):
    for lo, hi, tag in IO:
        if lo <= addr < hi:
            return tag
    return "io_other"


def main():
    F = [json.loads(l) for l in open(os.path.join(ROOT, "build", "analysis", "functions.jsonl"), encoding="utf-8")]
    for f in F:
        tags = set()
        for h in f["hw"]:
            a = int(h, 16)
            if 0x04000000 <= a < 0x05000000:
                tags.add(io_tag(a))
            elif 0x06000000 <= a < 0x07000000:
                tags.add("vram_mem")
            elif 0x05000000 <= a < 0x05000800:
                tags.add("palette_mem")
            elif 0x07000000 <= a < 0x07000800:
                tags.add("oam_mem")
            elif a >= 0xffff0000:
                tags.add("bios")
        for s in f["strings"]:
            for lib, rx in LIBS:
                if rx.search(s):
                    tags.add(lib)
        f["tags"] = sorted(tags)
    json.dump({f["addr"]: f["tags"] for f in F}, open(os.path.join(ROOT, "build", "analysis", "tags.json"), "w"))

    blocks = sys.argv[1:] or ["arm9_main"]
    for blk in blocks:
        fs = [f for f in F if f["block"] == blk]
        fs.sort(key=lambda f: int(f["addr"].split(":")[-1], 16))
        print("== %s: %d functions" % (blk, len(fs)))
        # Run-length view: consecutive functions grouped in windows of 32, listing dominant tags.
        for i in range(0, len(fs), 32):
            win = fs[i:i + 32]
            cnt = {}
            for f in win:
                for t in f["tags"]:
                    cnt[t] = cnt.get(t, 0) + 1
            thumb = sum(1 for f in win if f["thumb"])
            print("  %s..%s  thumb %2d/%2d  %s" % (
                win[0]["addr"].split(":")[-1], win[-1]["addr"].split(":")[-1], thumb, len(win),
                " ".join("%s:%d" % kv for kv in sorted(cnt.items(), key=lambda kv: -kv[1]))))


if __name__ == "__main__":
    main()
