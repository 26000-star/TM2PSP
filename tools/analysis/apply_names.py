"""Writes recovered names into dsd symbols.txt files (the project's source of truth for names).

Input files: lines "<addr> <name> ...", where addr is "XXXXXXXX" (main/itcm) or "ovNNN:XXXXXXXX".
Only dsd placeholder names (func_..., data_...) are replaced, so manual names are never overwritten.
A name already used elsewhere in the same file gets an "_<addr>" suffix.

Usage: python -P tools/analysis/apply_names.py build/analysis/lua_align.txt [more files...]
"""
import os
import re
import sys

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
PLACEHOLDER = re.compile(r"^(func|data|bss)_(ov\d+_)?[0-9a-f]{8}(_unk)?$")
LINE = re.compile(r"^(\S+)( kind:\S+.*addr:0x)([0-9a-fA-F]+)(.*)$")


def symbols_path(module):
    if module == "main":
        return os.path.join(ROOT, "config", "arm9", "symbols.txt")
    if module in ("itcm", "dtcm"):
        return os.path.join(ROOT, "config", "arm9", module, "symbols.txt")
    return os.path.join(ROOT, "config", "arm9", "overlays", module, "symbols.txt")


def main():
    wanted = {}  # module -> {addr: name}
    for path in sys.argv[1:]:
        for line in open(path):
            parts = line.split()
            if len(parts) < 2 or parts[0].startswith("#"):
                continue
            key, name = parts[0], parts[1]
            module, addr = key.split(":") if ":" in key else ("main", key)
            if module == "main" and int(addr, 16) < 0x02000000:
                module = "itcm"
            wanted.setdefault(module, {})[int(addr, 16)] = name

    for module, names in wanted.items():
        path = symbols_path(module)
        lines = open(path).read().splitlines()
        used = {LINE.match(l).group(1) for l in lines if LINE.match(l)}
        renamed = 0
        for i, l in enumerate(lines):
            m = LINE.match(l)
            if not m:
                continue
            addr = int(m.group(3), 16)
            if addr not in names or not PLACEHOLDER.match(m.group(1)):
                continue
            new = names[addr]
            if new in used:
                new = "%s_%08x" % (new, addr)
            used.discard(m.group(1))
            used.add(new)
            lines[i] = new + m.group(2) + m.group(3) + m.group(4)
            renamed += 1
        open(path, "w", newline="\n").write("\n".join(lines) + "\n")
        print("%s: %d names applied (%d requested)" % (os.path.relpath(path, ROOT), renamed, len(names)))


if __name__ == "__main__":
    main()
