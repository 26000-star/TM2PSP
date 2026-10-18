"""Statistics over every Nitro 3D file in assets/vfs: models, primitives, texture formats, special features.
Usage: python -P tools/assets/bmd_stats.py"""
import glob
import os
import sys
from collections import Counter

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import nitro3d  # noqa: E402

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))


def main():
    files = [p for p in glob.glob(os.path.join(ROOT, "assets", "vfs", "**", "*"), recursive=True)
             if p.lower().endswith((".n", ".na"))]
    magic, sections, fmts, prims, ops, gpu_other, errors = (Counter() for _ in range(7))
    tex_bytes = Counter()
    n_models = n_verts = 0
    biggest = []
    skinned = texmtx = multi_mtx = 0
    for p in files:
        data = open(p, "rb").read()
        try:
            c = nitro3d.read_container(data)
        except Exception as e:  # noqa: BLE001 - report and continue
            errors[type(e).__name__ + ": " + str(e)[:60]] += 1
            continue
        magic[c["magic"]] += 1
        sections.update(c["sections"])
        for t in c["textures"]:
            fmts[nitro3d.TEX_FORMAT_NAMES[t["format"]]] += 1
            tex_bytes[nitro3d.TEX_FORMAT_NAMES[t["format"]]] += len(t["data"]) + len(t["data2"])
        for m in c["models"]:
            n_models += 1
            nv = 0
            for pc in m["pieces"]:
                for pr in pc["prims"]:
                    prims[pr["type"]] += 1
                    nv += len(pr["verts"])
                    if any(v[4] is not None for v in pr["verts"]):
                        multi_mtx += 1
                gpu_other.update(pc["other"])
            n_verts += nv
            biggest.append((nv, os.path.relpath(p, ROOT)))
            ops.update(o[0] for o in m["ops"])
            skinned += any(o[0] == "skin" for o in m["ops"])
            texmtx += any(mt["tex_matrix"] for mt in m["materials"])
    print("files %d, magic %s, sections %s" % (len(files), dict(magic), dict(sections)))
    print("models %d, vertices %d" % (n_models, n_verts))
    print("primitives", dict(prims))
    print("render ops", dict(ops))
    print("extra GPU commands in display lists", {("%#x" % k if isinstance(k, int) else k): v for k, v in gpu_other.items()})
    print("skinned models %d, models with texture matrices %d, prims using matrix restore %d" % (skinned, texmtx, multi_mtx))
    print("texture formats", dict(fmts))
    print("texture bytes", dict(tex_bytes))
    print("largest models", sorted(biggest, reverse=True)[:6])
    print("errors", dict(errors))


if __name__ == "__main__":
    main()
