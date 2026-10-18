"""Address-space view of the dsd extract: main ARM9, ITCM, DTCM and overlays, plus dsd symbols."""
import os
import re

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
EXTRACT = os.path.join(ROOT, "extract")
CONFIG = os.path.join(ROOT, "config", "arm9")

_SYM = re.compile(r"^(\S+) kind:(function|data|bss|label)(?:\(([^)]*)\))?\s+addr:0x([0-9a-fA-F]+)")


def _flat_yaml(path):
    out = {}
    for line in open(path):
        if ":" in line and not line.startswith(" "):
            k, v = line.split(":", 1)
            out[k.strip()] = v.strip()
    return out


def _overlays():
    out, cur = [], None
    for line in open(os.path.join(EXTRACT, "arm9_overlays", "overlays.yaml")):
        t = line.strip()
        if t.startswith("- "):
            cur = {}
            out.append(cur)
            t = t[2:]
        if cur is not None and ":" in t:
            k, v = t.split(":", 1)
            cur[k.strip()] = v.strip()
    return out


class Module:
    def __init__(self, name, base, data, symfile):
        self.name, self.base, self.data = name, base, data
        self.end = base + len(data)
        self.functions = {}   # addr -> (name, thumb, size)
        self.symbols = {}     # addr -> (name, kind)
        if os.path.exists(symfile):
            for line in open(symfile):
                m = _SYM.match(line)
                if not m:
                    continue
                name_, kind, attrs, addr = m.group(1), m.group(2), m.group(3) or "", int(m.group(4), 16)
                self.symbols[addr] = (name_, kind)
                if kind == "function":
                    size = re.search(r"size=0x([0-9a-f]+)", attrs)
                    self.functions[addr] = (name_, attrs.startswith("thumb"), int(size.group(1), 16) if size else 0)

    def contains(self, addr):
        return self.base <= addr < self.end

    def u32(self, addr):
        o = addr - self.base
        return int.from_bytes(self.data[o:o + 4], "little")

    def cstring(self, addr, limit=256):
        o = addr - self.base
        if not (0 <= o < len(self.data)):
            return None
        e = self.data.find(b"\0", o, o + limit)
        if e < 0:
            return None
        raw = self.data[o:e]
        if not raw or any(c < 0x20 or c > 0x7e for c in raw if c not in (9, 10, 13)):
            return None
        return raw.decode("latin1")


def load():
    """Returns {name: Module} for main, itcm, dtcm and ov000..ov007 (code + zeroed bss)."""
    mods = {}
    a9 = _flat_yaml(os.path.join(EXTRACT, "arm9", "arm9.yaml"))
    main = open(os.path.join(EXTRACT, "arm9", "arm9.bin"), "rb").read()
    base = int(a9["base_address"])
    main += bytes(int(a9["bss_end"]) - base - len(main))
    mods["main"] = Module("main", base, main, os.path.join(CONFIG, "symbols.txt"))
    for tcm in ("itcm", "dtcm"):
        y = _flat_yaml(os.path.join(EXTRACT, "arm9", tcm + ".yaml"))
        d = open(os.path.join(EXTRACT, "arm9", tcm + ".bin"), "rb").read() + bytes(int(y["bss_size"]))
        mods[tcm] = Module(tcm, int(y["base_address"]), d, os.path.join(CONFIG, tcm, "symbols.txt"))
    for ov in _overlays():
        name = "ov%03d" % int(ov["id"])
        d = open(os.path.join(EXTRACT, "arm9_overlays", ov["file_name"]), "rb").read() + bytes(int(ov["bss_size"]))
        mods[name] = Module(name, int(ov["base_address"]), d, os.path.join(CONFIG, "overlays", name, "symbols.txt"))
    return mods


def resolve(mods, home, addr):
    """Module that an absolute pointer found in module `home` refers to (own overlay first, then shared modules)."""
    if mods[home].contains(addr):
        return mods[home]
    for n in ("main", "itcm", "dtcm"):
        if mods[n].contains(addr):
            return mods[n]
    return None
