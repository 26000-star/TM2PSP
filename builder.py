"""TM2PSP builder: turns your own TrackMania Turbo DS ROM into the PSP game folder (EBOOT.PBP + game files).

Nothing from the game is in this repository: the builder reads the game's files from the ROM you give it, checks
them, converts the front end's pictures, compiles the EBOOT from the sources (or takes one you built) and lays out
the memory stick folder:

    <out>/PSP/GAME/TM2PSP/EBOOT.PBP, files/ (the game's VFS and sound archive), fe/ (menu pictures), LISEZMOI.txt

Copy <out>/PSP to the root of the memory stick (custom firmware needed). A save.txt already there is kept.

Usage:
    python -P builder.py --rom <TrackMania Turbo (Europe).nds> [--out build/psp/package]
    python -P builder.py --files extract/files --eboot build/psp/tm_race/EBOOT.PBP     (already extracted / built)
Options:
    --toolchain auto|native|wsl|docker   how to compile the EBOOT (auto: psp-cmake on PATH, then WSL, then Docker)
    --wsl-distro NAME                    the WSL distribution with pspdev (default: WSL's default one)
    --force                              accept a ROM whose files differ from the supported dump
    --perf, --diag                       measurement files for the race (see tools/psp/package.ps1)
"""
import argparse
import hashlib
import os
import shutil
import subprocess
import sys

ROOT = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(ROOT, "tools"))
from nitrofs import NitroFS  # noqa: E402

GAME_CODE = "BT2P"  # TrackMania Turbo, Europe (En, Fr, De, Es, It)
ROM_SHA1 = "857b8f5f5ce9b91eaa04e35b99a591243672e51c"
# The files the PSP game reads (movies, Download Play, WFC and the network sound archive are not used), with the
# SHA-1 of the supported dump: a trimmed ROM has another SHA-1 but the same files.
FILES = {
    "vfs.zip": "9ef024d78c4cdb45c9f38e4ed3453cc49f22343d",
    "vfs.bin": "19eb1c2cffed44bc979599d139a44d8318d923c6",
    "vfs.dat": "abe21b3e46ed29edb08d4274a0433ee036c5e5db",
    "audio/sound_data.sdat": "61bcdfb630aae25d556ca895130538531b81dec6",
}
PSP_IMAGE = "pspdev/pspdev:latest"
DIAG = ("sno/easy/0 40 20 300 -16 33 345\nsno/easy/0 110 10 330 -16 33 336\ncoa/easy/0 60 6 80 85 17 115\n"
        "coa/easy/0 200 5 140 240 20 165\n")
LISEZMOI = r"""TM2PSP - portage PSP de TrackMania Turbo DS

Installation : copier le dossier PSP a la racine de la Memory Stick (ms0:/PSP/GAME/TM2PSP/EBOOT.PBP).
Firmware personnalise requis (ex. 6.61 LME / Infinity).

Menus : croix = valider, rond = retour. Les 104 pistes des quatre decors (Stadium, Island, Snow, Coast), le mode
plate-forme, le mode puzzle, l'editeur de pistes, la boutique, le profil et les options.
Les meilleurs temps et la progression sont gardes dans save.txt, a cote de l'EBOOT.

Course : croix = accelerer, carre = freiner / marche arriere, croix directionnelle ou stick = diriger,
triangle = dernier checkpoint, START = pause (continuer, recommencer, piste suivante, menu des pistes),
SELECT = autre camera, L = klaxon, R = temps par image (mise a jour / son / construction / attente GE, en ms).
Son : un fichier vide mute.txt a cote de l'EBOOT coupe le son.
Mesures : un fichier vide perf.txt a cote de l'EBOOT fait ecrire les temps par seconde de course dans
frametimes.txt (ecrit a la pause, au menu et en quittant). HOME pour quitter.
"""


def step(text):
    print("== " + text, flush=True)


def fail(text):
    sys.exit("error: " + text)


def sha1(data):
    return hashlib.sha1(data).hexdigest()


def extract(rom, dest, force):
    """Copies the game files the PSP needs out of the ROM into dest, after checking them."""
    step("reading the ROM " + rom)
    try:
        fs = NitroFS(rom)
    except (OSError, ValueError) as e:
        fail(str(e))
    if fs.game_code != GAME_CODE:
        msg = "game code %s, expected %s (TrackMania Turbo, Europe)" % (fs.game_code, GAME_CODE)
        if not force:
            fail(msg + "; --force tries it anyway")
        print("warning: " + msg)
    h = hashlib.sha1()
    with open(rom, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    print("ROM SHA-1 %s (%s)" % (h.hexdigest(), "the supported dump" if h.hexdigest() == ROM_SHA1 else
                                 "another dump: its files are checked one by one"))
    for path, want in FILES.items():
        if path not in fs.files:
            fail("%s is not in the ROM" % path)
        data = fs.read(path)
        if sha1(data) != want:
            if not force:
                fail("%s differs from the supported dump (SHA-1 %s); --force uses it anyway" % (path, sha1(data)))
            print("warning: %s differs from the supported dump" % path)
        out = os.path.join(dest, path)
        os.makedirs(os.path.dirname(out), exist_ok=True)
        with open(out, "wb") as f:
            f.write(data)
    print("%d files in %s" % (len(FILES), dest))


def check_files(src, force):
    for path, want in FILES.items():
        p = os.path.join(src, path)
        if not os.path.exists(p):
            fail("%s is missing" % p)
        with open(p, "rb") as f:
            if sha1(f.read()) != want:
                if not force:
                    fail("%s differs from the supported dump; --force uses it anyway" % p)
                print("warning: %s differs from the supported dump" % p)


def run(cmd, **kw):
    print("$ " + " ".join(cmd), flush=True)
    if subprocess.call(cmd, **kw) != 0:
        fail("the command failed")


def wsl_cmd(distro):
    return ["wsl"] + (["-d", distro] if distro else []) + ["--"]


def wsl_distros():
    try:
        r = subprocess.run(["wsl", "-l", "-q"], capture_output=True, timeout=60)
    except (OSError, subprocess.TimeoutExpired):
        return []
    return [d.strip() for d in r.stdout.decode("utf-16-le", "ignore").splitlines() if d.strip()]


def find_wsl_pspdev(distro):
    """The WSL distribution with pspdev: the given one, else the default one, else the first that has it."""
    if os.name != "nt" or not shutil.which("wsl"):
        return None
    for d in [distro] if distro else [None] + wsl_distros():
        if wsl_has_pspdev(d):
            return d or ""
    return None


def wsl_has_pspdev(distro):
    try:
        r = subprocess.run(wsl_cmd(distro) + ["bash", "-lc", "command -v psp-cmake"], capture_output=True,
                           timeout=60)
    except (OSError, subprocess.TimeoutExpired):
        return False
    return r.returncode == 0 and r.stdout.strip() != b""


def build_eboot(toolchain, distro):
    """Compiles tm_race for the PSP (Release) and returns the path of its EBOOT.PBP."""
    if toolchain == "auto":
        if shutil.which("psp-cmake"):
            toolchain = "native"
        elif find_wsl_pspdev(distro) is not None:
            toolchain = "wsl"
        elif shutil.which("docker"):
            toolchain = "docker"
        else:
            fail("no PSP toolchain: install pspdev (https://pspdev.github.io, psp-cmake on PATH, or in WSL on "
                 "Windows) or Docker, or give a ready EBOOT with --eboot")
    step("compiling the EBOOT (%s)" % toolchain)
    if toolchain == "native":
        b = os.path.join(ROOT, "build", "psp-builder")
        run(["psp-cmake", "-S", ROOT, "-B", b, "-DCMAKE_BUILD_TYPE=Release"])
        run(["cmake", "--build", b, "--target", "tm_race", "-j", str(os.cpu_count() or 2)])
    elif toolchain == "wsl":
        distro = find_wsl_pspdev(distro)
        if distro is None:
            fail("no WSL distribution with pspdev (psp-cmake)")
        # wsl.exe drops backslashes and mangles command lines: forward slashes, and the commands in a script file.
        root = subprocess.run(wsl_cmd(distro) + ["wslpath", "-a", ROOT.replace("\\", "/")], capture_output=True,
                              text=True)
        if root.returncode != 0:
            fail("wslpath failed: " + root.stderr.strip())
        root = root.stdout.strip()
        b = os.path.join(ROOT, "build", "psp-builder")
        os.makedirs(b, exist_ok=True)
        # The build tree lives in the distro's file system (much faster than /mnt/c); only the EBOOT comes back.
        with open(os.path.join(b, "build.sh"), "w", newline="\n") as f:
            f.write("set -e\n"
                    "B=$HOME/tm2psp-build/builder\n"
                    "psp-cmake -S '{r}' -B $B -DCMAKE_BUILD_TYPE=Release >/dev/null\n"
                    "cmake --build $B --target tm_race -j$(nproc)\n"
                    "mkdir -p '{r}/build/psp-builder/tm_race_pbp'\n"
                    "cp $B/tm_race_pbp/EBOOT.PBP '{r}/build/psp-builder/tm_race_pbp/'\n".format(r=root))
        run(wsl_cmd(distro) + ["bash", "-l", root + "/build/psp-builder/build.sh"])
    elif toolchain == "docker":
        b = os.path.join(ROOT, "build", "psp-builder")
        run(["docker", "run", "--rm", "-v", ROOT + ":/src", "-w", "/src", PSP_IMAGE, "sh", "-c",
             "psp-cmake -S . -B build/psp-builder -DCMAKE_BUILD_TYPE=Release && "
             "cmake --build build/psp-builder --target tm_race -j$(nproc)"])
    else:
        fail("unknown toolchain " + toolchain)
    eboot = os.path.join(b, "tm_race_pbp", "EBOOT.PBP")
    if not os.path.exists(eboot):
        fail("the build ended without " + eboot)
    return eboot


def convert_pictures(files, dest):
    step("converting the menu pictures")
    env = dict(os.environ, TM_ROM_FILES=os.path.abspath(files))
    run([sys.executable, "-P", os.path.join(ROOT, "tools", "fe", "convert.py"), dest], env=env)


def package(out, eboot, files, fe, perf, diag):
    step("laying out " + out)
    g = os.path.join(out, "PSP", "GAME", "TM2PSP")
    os.makedirs(g, exist_ok=True)
    shutil.copyfile(eboot, os.path.join(g, "EBOOT.PBP"))
    for sub in ("files", "fe"):
        if os.path.isdir(os.path.join(g, sub)):
            shutil.rmtree(os.path.join(g, sub))
    for path in FILES:
        os.makedirs(os.path.dirname(os.path.join(g, "files", path)), exist_ok=True)
        shutil.copyfile(os.path.join(files, path), os.path.join(g, "files", path))
    shutil.copytree(fe, os.path.join(g, "fe"))
    # No track.txt: the program starts on its menus. save.txt (best times, progression) is written by the program
    # and is not touched, so copying a new package over an old one keeps it.
    for name in ["track.txt"] + [n for n in os.listdir(g) if n.startswith("diag_") and n.endswith(".bmp")]:
        if os.path.exists(os.path.join(g, name)):
            os.remove(os.path.join(g, name))
    for name, on, text in (("perf.txt", perf, ""), ("diag.txt", diag, DIAG)):
        p = os.path.join(g, name)
        if on:
            with open(p, "w", newline="\n") as f:
                f.write(text)
        elif os.path.exists(p):
            os.remove(p)
    with open(os.path.join(g, "LISEZMOI.txt"), "w", encoding="ascii", newline="\r\n") as f:
        f.write(LISEZMOI)
    size = sum(os.path.getsize(os.path.join(d, n)) for d, _, ns in os.walk(g) for n in ns)
    print("package: %s (%.1f MB)" % (g, size / 1048576))


def main():
    ap = argparse.ArgumentParser(description="Builds the TM2PSP memory stick folder from your own ROM.")
    src = ap.add_mutually_exclusive_group(required=True)
    src.add_argument("--rom", help="your dump of TrackMania Turbo DS (Europe, BT2P)")
    src.add_argument("--files", help="a directory already holding the ROM's files (e.g. extract/files)")
    ap.add_argument("--out", default=os.path.join(ROOT, "build", "psp", "package"))
    ap.add_argument("--eboot", help="a ready EBOOT.PBP instead of compiling one")
    ap.add_argument("--toolchain", default="auto", choices=["auto", "native", "wsl", "docker"])
    ap.add_argument("--wsl-distro")
    ap.add_argument("--force", action="store_true")
    ap.add_argument("--perf", action="store_true")
    ap.add_argument("--diag", action="store_true")
    a = ap.parse_args()

    work = os.path.join(ROOT, "build", "builder")
    if a.rom:
        files = os.path.join(work, "files")
        extract(a.rom, files, a.force)
    else:
        files = a.files
        check_files(files, a.force)
    fe = os.path.join(work, "fe")
    if os.path.isdir(fe):
        shutil.rmtree(fe)
    convert_pictures(files, fe)
    eboot = a.eboot or build_eboot(a.toolchain, a.wsl_distro)
    if not os.path.exists(eboot):
        fail(eboot + " does not exist")
    package(a.out, eboot, files, fe, a.perf, a.diag)
    print("done: copy %s to the root of the memory stick" % os.path.join(a.out, "PSP"))


if __name__ == "__main__":
    main()
