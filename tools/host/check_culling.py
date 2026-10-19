"""Checks that the piece culling (RaceSession::Cull) never drops a visible piece: for each Coast track and several
frame counts of the auto drive, tm_race renders the frame with and without culling (--no-cull) and the two images
must be identical pixel for pixel. Also reports how many pieces are drawn.

Usage: python -P tools/host/check_culling.py [--bin build/host] [--frames 60,200,450] [--quick]
"""
import os
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from run_checks import TRACKS  # noqa: E402


def main():
    args = sys.argv[1:]
    opt = lambda k, d: args[args.index(k) + 1] if k in args else d
    bindir = opt("--bin", os.path.join(ROOT, "build", "host"))
    frames = [int(f) for f in opt("--frames", "60,200,450").split(",")]
    tracks = ["easy/0"] if "--quick" in args else TRACKS
    exe = os.path.join(bindir, "tm_race" + (".exe" if os.name == "nt" else ""))
    tmp = os.path.join(ROOT, "build", "host", "culling")
    os.makedirs(tmp, exist_ok=True)
    errors, drawn, total = 0, 0, 0
    for t in tracks:
        for n in frames:
            imgs = []
            for extra in ([], ["--no-cull"]):
                out = os.path.join(tmp, "%s_%d%s.bmp" % (t.replace("/", "_"), n, "_all" if extra else ""))
                r = subprocess.run([exe, "--root", os.path.join(ROOT, "extract", "files"), "--track", t, "--frames",
                                    str(n), "--auto", "--shot", out] + extra, capture_output=True, text=True)
                if not extra:
                    for line in r.stdout.splitlines():
                        if line.startswith("pieces drawn"):
                            w = line.split()
                            drawn += int(w[2])
                            total += int(w[4])
                imgs.append(open(out, "rb").read())
            same = imgs[0] == imgs[1]
            errors += not same
            if not same:
                print("%s frame %d: images differ" % (t, n))
    print("%d tracks x %d frames: %d differences; pieces drawn %d of %d (%.0f%%)" %
          (len(tracks), len(frames), errors, drawn, total, 100.0 * drawn / max(total, 1)))
    return 1 if errors else 0


if __name__ == "__main__":
    sys.exit(main())
