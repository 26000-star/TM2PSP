"""Checks that nothing from the game is about to be published: run by the pre-push hook (.githooks/pre-push).

Every file of the given commits (HEAD by default, all commits not yet on a remote with --unpushed, or the
revisions given) is checked against what must stay local:
- paths: ROMs, extracted or converted game data (extract/, assets/, build/, *.sdat, vfs.*, *.fet, *.nds...),
- binary files: only the ones listed in ALLOWED_BINARIES may be committed (none today: pictures of the game,
  screenshots included, stay out; the front end's pictures come from the user's ROM at build time),
- content: a DS ROM header, or a file starting with a Nitro signature (BMD0, SDAT, NCGR...) or a zip header.

Usage: python -P tools/check_public.py [--unpushed | <rev> ...]
"""
import fnmatch
import subprocess
import sys

FORBIDDEN = ["*.nds", "*.srl", "*.sdat", "*.mob", "*.zip", "*.fet", "*.pbp", "*.PBP", "*.luac", "*.dcsc", "vfs.*",
             "*/vfs.*", "extract/*", "assets/*", "build/*", "ghidra/*", "*.bmd", "*.nsbmd", "*.ncgr", "*.nclr",
             "*.nscr", "*.ncer", "*.nanr", "*.sseq", "*.sbnk", "*.swar", "*.strm"]
ALLOWED_BINARIES = set()
# Nitro file magics, as stored (most are byte-reversed tags) and a zip local header.
MAGICS = [b"BMD0", b"BTX0", b"SDAT", b"RGCN", b"RLCN", b"RCSN", b"RECN", b"RNAN", b"SSEQ", b"SBNK", b"SWAR",
          b"PK\x03\x04"]


def git(*args):
    return subprocess.run(["git"] + list(args), capture_output=True, check=True).stdout


def blobs(revs):
    """(path, blob id) of every file in the trees of the given commits."""
    seen = {}
    for rev in revs:
        for line in git("ls-tree", "-r", "-z", rev).split(b"\0"):
            if line:
                meta, path = line.split(b"\t", 1)
                seen[(path.decode("utf-8", "replace"), meta.split()[2].decode())] = rev
    return seen


def problems(path, data):
    low = path.lower()
    for pat in FORBIDDEN:
        if fnmatch.fnmatch(low, pat.lower()):
            yield "game data path (%s)" % pat
            break
    if b"\0" in data[:8000] and path not in ALLOWED_BINARIES:
        yield "binary file"
    for m in MAGICS:
        if data.startswith(m):
            yield "starts with the signature %r" % m
            break
    if len(data) >= 0x160 and data[0x15C:0x15E] == b"\x56\xCF":  # the DS header's Nintendo logo CRC
        yield "DS ROM header"


def main():
    args = sys.argv[1:]
    if args == ["--unpushed"]:
        revs = git("rev-list", "HEAD", "--not", "--remotes").decode().split()
    else:
        revs = args or ["HEAD"]
    if not revs:
        print("check_public: nothing to check")
        return 0
    bad = 0
    files = blobs(revs)
    for (path, blob), rev in sorted(files.items()):
        data = git("cat-file", "blob", blob)
        for why in problems(path, data):
            print("check_public: %s (%s): %s" % (path, rev[:10], why))
            bad += 1
    print("check_public: %d files in %d commit(s), %d problem(s)" % (len(files), len(revs), bad))
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
