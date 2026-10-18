"""Compares nitro3d.decode_texture() with apicula's PNG output, pixel by pixel.
Usage: python -P tools/assets/validate_textures.py [<file.n> ...]"""
import glob
import os
import subprocess
import sys
import tempfile

from PIL import Image

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import nitro3d  # noqa: E402

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
APICULA = os.environ.get("APICULA", r"C:\Tools\bin\apicula.exe")


def palette_for(c, mat, tex):
    """Palette offset used with a texture: the material's palette, or the texture's name + '_pl'."""
    names = {p["name"]: p["offset"] for p in c["palettes"]}
    for m in c["models"]:
        for mt in m["materials"]:
            if mt.get("texture") == tex["name"] and mt.get("palette") in names:
                return names[mt["palette"]]
    return names.get(tex["name"] + "_pl", 0)


def main():
    files = sys.argv[1:] or sorted(glob.glob(os.path.join(ROOT, "assets", "vfs", "Graphics", "Cars", "*", "0", "lod0.n"))
                                   + glob.glob(os.path.join(ROOT, "assets", "vfs", "Graphics", "Sky", "*", "sky_day.n")))
    total = bad = 0
    per_fmt = {}
    for f in files:
        c = nitro3d.read_container(open(f, "rb").read())
        with tempfile.TemporaryDirectory() as out:
            out = os.path.join(out, "x")
            subprocess.run([APICULA, "convert", f, "-o", out, "-f", "glb"], check=True,
                           stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            for tex in c["textures"]:
                png = os.path.join(out, tex["name"] + ".png")
                if not os.path.exists(png):
                    continue
                ref = Image.open(png).convert("RGBA")
                mine = nitro3d.decode_texture(tex, c["pal_block"], palette_for(c, None, tex))
                diff = 0
                for y in range(tex["height"]):
                    for x in range(tex["width"]):
                        a, b = mine[y][x], ref.getpixel((x, y))
                        if a[3] == 0 and b[3] == 0:
                            continue
                        if max(abs(p - q) for p, q in zip(a, b)) > 8:
                            diff += 1
                total += 1
                name = nitro3d.TEX_FORMAT_NAMES[tex["format"]]
                s = per_fmt.setdefault(name, [0, 0])
                s[0] += 1
                if diff:
                    bad += 1
                    s[1] += 1
                    print("DIFF %-40s %-8s %dx%d: %d pixels" % (os.path.relpath(f, ROOT)[-40:] + ":" + tex["name"],
                                                                name, tex["width"], tex["height"], diff))
    print("textures compared %d, differing %d; per format (count, differing): %s" % (total, bad, per_fmt))


if __name__ == "__main__":
    main()
