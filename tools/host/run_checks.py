"""Runs every host check: each producer (tm_cfgdump, tm_trackdump, tm_physics) writes its dump to build/host/checks,
then the matching checker reads it. Prints one line per check and the total error count.

Usage: python -P tools/host/run_checks.py [--bin build/host] [--out build/host/checks] [--quick] [--no-check]
                                          [--dumps <dir>] [--list]
  --quick     one track (easy/0) instead of the 26 Coast tracks
  --no-check  only write the dumps
  --dumps     run the checkers on existing dumps <dir>/<name>.txt (e.g. the PSP suite output) instead of producing
  --list      print the check list ("<name> <program> <args>") for the check suite (tm_checksuite, PSP tm_checks)
"""
import os
import re
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
TRACKS = ["practice/%d" % i for i in range(5)] + ["easy/%d" % i for i in range(5)] + \
         ["medium/%d" % i for i in range(5)] + ["hard/%d" % i for i in range(5)] + ["extreme/0"] + \
         ["bonus/%d" % i for i in range(5)]
CAMERA_TRACKS = ["easy/0", "practice/0", "medium/2", "hard/0", "extreme/0"]


def checks(tracks):
    """(dump name, program, arguments, checker, checker arguments)"""
    out = [
        ("carparams", "tm_cfgdump", ["--carparams"], "check_carparams.py", []),
        ("cameras", "tm_cfgdump", ["--cameras"], "check_cameras.py", []),
        ("engine", "tm_physics", ["--engine"], "check_engine.py", []),
        ("raycast", "tm_physics", ["--track", "easy/0", "--raycast"], "check_raycast.py", []),
        ("drop", "tm_physics", ["--drop"], "check_drop.py", []),
    ]
    for t in tracks:
        n = t.replace("/", "_")
        out.append(("track_" + n, "tm_trackdump", ["--track", t, "--pieces", "--collision", "--lighting"],
                    "check_track.py", ["--track", t]))
        out.append(("terrain_" + n, "tm_trackdump", ["--track", t, "--terrain"], "check_terrain.py", ["--track", t]))
        out.append(("triggers_" + n, "tm_trackdump", ["--track", t, "--triggers"], "check_triggers.py", []))
        out.append(("progress_" + n, "tm_physics", ["--track", t, "--progress"], "check_progress.py", []))
    for t in CAMERA_TRACKS:
        if t in tracks:
            out.append(("camera_" + t.replace("/", "_"), "tm_physics", ["--track", t, "--camera"],
                        "check_camera.py", []))
    return out


def main():
    args = sys.argv[1:]
    opt = lambda k, d: args[args.index(k) + 1] if k in args else d
    bindir = opt("--bin", os.path.join(ROOT, "build", "host"))
    outdir = opt("--out", os.path.join(ROOT, "build", "host", "checks"))
    tracks = ["easy/0"] if "--quick" in args else TRACKS
    if "--list" in args:
        for name, prog, pargs, _, _ in checks(TRACKS):
            print("%s %s %s" % (name, prog, " ".join(pargs)))
        return 0
    os.makedirs(outdir, exist_ok=True)
    total, failed = 0, []
    for name, prog, pargs, checker, cargs in checks(tracks):
        exe = os.path.join(bindir, prog + (".exe" if os.name == "nt" else ""))
        dump = os.path.join(outdir, name + ".txt")
        if "--dumps" in args:
            dump = os.path.join(opt("--dumps", ""), name + ".txt")
            if not os.path.exists(dump):
                failed.append(name)
                print("%-24s missing dump" % name)
                continue
        else:
            with open(dump, "wb") as f:
                r = subprocess.run([exe, "--root", os.path.join(ROOT, "extract", "files")] + pargs, stdout=f,
                                   cwd=ROOT)
            if r.returncode != 0:
                failed.append(name)
                print("%-24s producer exit %d" % (name, r.returncode))
                continue
        if "--no-check" in args:
            continue
        c = subprocess.run([sys.executable, "-P", os.path.join(ROOT, "tools", "host", checker), dump] + cargs,
                           capture_output=True, text=True, cwd=ROOT)
        last = (c.stdout.strip().splitlines() or ["?"])[-1]
        m = re.search(r"(\d+) errors?", last)
        n = int(m.group(1)) if m else 0
        if c.returncode != 0 and n == 0:
            n = 1
        total += n
        if c.returncode != 0:
            failed.append(name)
        print("%-24s %s" % (name, last))
    print("%d checks, %d failed, %d errors" % (len(checks(tracks)), len(failed), total))
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
