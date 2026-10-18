"""Cross-checks nitro3d.py against apicula on standard BMD0 files: triangle counts, position bounds and
material/texture names must agree.

Usage: python -P tools/assets/validate_apicula.py [<file.n> ...]   (default: a sample of cars/sky/misc)
"""
import glob
import json
import os
import struct
import subprocess
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import nitro3d  # noqa: E402

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
APICULA = os.environ.get("APICULA", r"C:\Tools\bin\apicula.exe")


def ours(path):
    c = nitro3d.read_container(open(path, "rb").read())
    m = c["models"][0]
    tris, lo, hi = 0, [1e9] * 3, [-1e9] * 3
    # Positions are in model space before the node matrix and up_scale (apicula applies both).
    scale = m["up_scale"] if any(o[0] == "scale_up" for o in m["ops"]) else 1.0
    for pc in m["pieces"]:
        for pr in pc["prims"]:
            n = len(pr["verts"])
            tris += {"triangles": n // 3, "quads": n // 4 * 2}.get(pr["type"], max(0, n - 2))
            for v in pr["verts"]:
                for k in range(3):
                    lo[k] = min(lo[k], v[0][k] * scale)
                    hi[k] = max(hi[k], v[0][k] * scale)
    return tris, lo, hi, sorted(mt["texture"] or "" for mt in m["materials"])


def theirs(path):
    with tempfile.TemporaryDirectory() as out:
        out = os.path.join(out, "x")  # apicula refuses an existing output directory
        subprocess.run([APICULA, "convert", path, "-o", out, "-f", "glb"], check=True,
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        glb = glob.glob(os.path.join(out, "*.glb"))[0]
        data = open(glb, "rb").read()
        jlen = struct.unpack_from("<I", data, 12)[0]
        gltf = json.loads(data[20:20 + jlen])
        tris, lo, hi = 0, [1e9] * 3, [-1e9] * 3
        for mesh in gltf["meshes"]:
            for p in mesh["primitives"]:
                idx = gltf["accessors"][p["indices"]]["count"]
                tris += idx // 3
                acc = gltf["accessors"][p["attributes"]["POSITION"]]
                for k in range(3):
                    lo[k] = min(lo[k], acc["min"][k])
                    hi[k] = max(hi[k], acc["max"][k])
        names = sorted(m.get("name", "") for m in gltf.get("materials", []))
        return tris, lo, hi, names


def main():
    files = sys.argv[1:] or sorted(
        glob.glob(os.path.join(ROOT, "assets", "vfs", "Graphics", "Cars", "*", "0", "lod*.n"))
        + glob.glob(os.path.join(ROOT, "assets", "vfs", "Graphics", "Sky", "*", "*.n"))[:6]
        + glob.glob(os.path.join(ROOT, "assets", "vfs", "Graphics", "Misc", "*.n"))[:6])
    ok = 0
    for f in files:
        a, b = ours(f), theirs(f)
        same_tris = a[0] == b[0]
        same_bounds = all(abs(x - y) < 1e-3 for x, y in zip(a[1] + a[2], b[1] + b[2]))
        ok += same_tris and same_bounds
        flag = "OK " if same_tris and same_bounds else "BAD"
        print("%s %-55s tris %5d/%5d  bounds %s" % (flag, os.path.relpath(f, ROOT)[-55:], a[0], b[0],
              "same" if same_bounds else "ours %s..%s theirs %s..%s" % (
                  [round(x, 3) for x in a[1]], [round(x, 3) for x in a[2]],
                  [round(x, 3) for x in b[1]], [round(x, 3) for x in b[2]])))
    print("%d/%d files agree" % (ok, len(files)))


if __name__ == "__main__":
    main()
