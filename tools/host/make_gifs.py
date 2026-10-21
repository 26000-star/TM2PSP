"""Animated GIFs of the port for the README, recorded from scripted runs of the host's tm_race (deterministic: the
same frames as the PSP's, see tools/psp/race_shot.ps1).

The menus use the ROM's pictures (TM_FE_ROM_PICTURES=1, tools/fe/fe_pictures.py), never the ones in assets/fe.

Usage: python -P tools/host/make_gifs.py [--root extract/files] [--out docs/media] [--exe build/host/tm_race]
"""
import argparse
import glob
import os
import shutil
import subprocess
import sys

from PIL import Image

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..")
FPS_FRAMES = 3  # one GIF frame every three game frames: 10 per second
# (gif, track, first frame, last frame): accelerator held, countdown included
RACES = [
    ("race", [("sta/easy/1", 84, 234)]),
    ("environments", [("coa/easy/0", 100, 154), ("isl/easy/1", 100, 154), ("sno/easy/0", 130, 184),
                      ("sta/easy/0", 100, 154)]),
]
# Front end tour: keys pressed from the main menu (U D L R, O = cross, B = back), one picture each.
MENUS = ["", "O", "OO", "OOR", "OORO", "OOROO", "OOROOO", "R", "RR"]
# Track editor (one press per frame): START > Free Draw, then X held while the cursor draws a loop.
EDITOR_PAD = "......P.....D.....X.....[....." + "".join(d + "....." for d in "RRRRDDDDLLLLUUUU") + "].........."


def run(exe, args, workdir):
    r = subprocess.run([exe, "--mute"] + args, cwd=workdir, capture_output=True, text=True)
    if r.returncode != 0:
        sys.exit("tm_race failed: %s\n%s" % (" ".join(args), r.stderr))


def record(exe, root, fe, args, first, last, tmp):
    """The frames first..last (every FPS_FRAMES) of one scripted run, as pictures."""
    for f in glob.glob(os.path.join(tmp, "*.bmp")):
        os.remove(f)
    run(exe, ["--root", root, "--fe", fe, "--frames", str(last + 1), "--shot-from", str(first),
              "--shot-every", str(FPS_FRAMES), "--shot", os.path.join(tmp, "f_%05d.bmp")] + args, ROOT)
    return [Image.open(f).convert("RGB") for f in sorted(glob.glob(os.path.join(tmp, "*.bmp")))]


def band(im):
    """The 16:9 band where the front end is drawn (letterboxed in the host's 4:3 window)."""
    w, h = im.size
    bh = w * 9 // 16
    return im.crop((0, (h - bh) // 2, w, (h - bh) // 2 + bh))


def save_gif(path, frames, size, ms):
    # No dithering: it changes pixels from frame to frame and defeats the GIF's compression.
    frames = [f.resize(size, Image.LANCZOS).quantize(colors=255, method=Image.Quantize.MEDIANCUT,
                                                       dither=Image.Dither.NONE) for f in frames]
    frames[0].save(path, save_all=True, append_images=frames[1:], duration=ms, loop=0, optimize=True)
    print("%s: %d frames, %d KB" % (os.path.normpath(path), len(frames), os.path.getsize(path) // 1024))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--root", default=os.path.join(ROOT, "extract", "files"))
    ap.add_argument("--out", default=os.path.join(ROOT, "docs", "media"))
    ap.add_argument("--exe", default=os.path.join(ROOT, "build", "host", "tm_race"))
    a = ap.parse_args()
    exe = a.exe + (".exe" if os.name == "nt" and not a.exe.endswith(".exe") else "")
    root = os.path.abspath(a.root)
    tmp = os.path.join(ROOT, "build", "gifs")
    fe = os.path.join(tmp, "fe")
    shots = os.path.join(tmp, "shots")
    os.makedirs(shots, exist_ok=True)
    os.makedirs(a.out, exist_ok=True)
    env = dict(os.environ, TM_FE_ROM_PICTURES="1", TM_ROM_FILES=root)
    subprocess.run([sys.executable, "-P", os.path.join(ROOT, "tools", "fe", "convert.py"), fe], env=env, check=True,
                   stdout=subprocess.DEVNULL)

    for name, clips in RACES:
        frames = []
        for track, first, last in clips:
            frames += record(exe, root, fe, ["--track", track, "--auto"], first, last, shots)
        save_gif(os.path.join(a.out, name + ".gif"), frames, (400, 300), 1000 * FPS_FRAMES // 30)

    frames = []
    save = os.path.join(tmp, "save.txt")
    for keys in MENUS:
        if os.path.exists(save):
            os.remove(save)
        run(exe, ["--root", root, "--fe", fe, "--frames", "10", "--screen", "menu", "--keys", keys, "--save", save,
                  "--shot", os.path.join(shots, "menu.bmp")], ROOT)
        frames.append(band(Image.open(os.path.join(shots, "menu.bmp")).convert("RGB")))
    save_gif(os.path.join(a.out, "menus.gif"), frames, (480, 270), 1300)

    frames = record(exe, root, fe, ["--editor", "sta", "--pad", EDITOR_PAD], 0, len(EDITOR_PAD), shots)
    save_gif(os.path.join(a.out, "editor.gif"), frames, (400, 300), 1000 * FPS_FRAMES // 30)
    shutil.rmtree(shots)


if __name__ == "__main__":
    main()
