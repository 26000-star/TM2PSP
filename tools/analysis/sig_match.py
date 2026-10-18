"""Names library functions by byte signatures from objects built with the original compiler.

For every function symbol in the given ELF objects: its bytes, with every relocated word masked out
(call targets, literal-pool pointers). A DS function matches when its bytes are identical outside
the mask. Matches are only kept when unique (one signature for one DS address and vice versa).
Each matched signature then names its relocation targets too: a Thumb/ARM call or a literal-pool
pointer to a named symbol yields that symbol's DS address (functions only when the target is a
known function start, data otherwise).

Usage: python -P tools/analysis/sig_match.py <out.txt> <object dir> [<object dir> ...]
       Objects whose path contains "arm7" are skipped (ARM7 code is not in the ARM9 binary).
Output lines: "<ds addr> <name> <object> # <reason>"
"""
import glob
import io
import os
import struct
import sys
from collections import defaultdict

from elftools.elf.elffile import ELFFile
from elftools.elf.relocation import RelocationSection

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import romimage  # noqa: E402

R_ARM_ABS32, R_ARM_REL32, R_ARM_PC24, R_ARM_THM_CALL, R_ARM_CALL, R_ARM_JUMP24 = 2, 3, 1, 10, 28, 29


def signatures(obj_path):
    """[(name, thumb, bytes, mask, relocs[(offset, type, target name)], object)] for one ELF object."""
    elf = ELFFile(io.BytesIO(open(obj_path, "rb").read()))
    symtab = elf.get_section_by_name(".symtab")
    if not symtab:
        return []
    syms = list(symtab.iter_symbols())
    relocs = defaultdict(list)  # section index -> [(offset, type, symbol name)]
    for sec in elf.iter_sections():
        if isinstance(sec, RelocationSection):
            target = sec["sh_info"]
            for r in sec.iter_relocations():
                s = syms[r["r_info_sym"]]
                name = s.name
                if not name and s["st_shndx"] not in ("SHN_UNDEF", "SHN_ABS"):
                    name = elf.get_section(s["st_shndx"]).name + "+%x" % s["st_value"]
                relocs[target].append((r["r_offset"], r["r_info_type"], name))
    # Mapping symbols ($a/$t) give the instruction set when st_value has no Thumb bit.
    modes = defaultdict(list)
    for s in syms:
        if s.name in ("$a", "$t") and isinstance(s["st_shndx"], int):
            modes[s["st_shndx"]].append((s["st_value"], s.name == "$t"))
    out = []
    for s in syms:
        if s["st_info"]["type"] != "STT_FUNC" or not isinstance(s["st_shndx"], int) or s["st_size"] == 0:
            continue
        sec = elf.get_section(s["st_shndx"])
        if sec["sh_type"] != "SHT_PROGBITS":
            continue
        start = s["st_value"] & ~1
        thumb = bool(s["st_value"] & 1)
        if not thumb:
            prior = [t for v, t in sorted(modes[s["st_shndx"]]) if v <= start]
            thumb = prior[-1] if prior else False
        data = sec.data()[start:start + s["st_size"]]
        mask = bytearray(b"\xff" * len(data))
        rel = []
        for off, typ, name in relocs[s["st_shndx"]]:
            if start <= off < start + len(data):
                o = off - start
                for i in range(o, min(o + 4, len(data))):
                    mask[i] = 0
                rel.append((o, typ, name))
        out.append((s.name, thumb, bytes(data), bytes(mask), rel, os.path.basename(obj_path)))
    return out


def decode_target(mod, addr, typ):
    """Absolute target of a relocated field in the DS image."""
    if typ == R_ARM_THM_CALL:
        hi, lo = struct.unpack_from("<HH", mod.data, addr - mod.base)
        if (hi & 0xf800) != 0xf000 or (lo & 0xe800) != 0xe800:
            return None
        off = ((hi & 0x7ff) << 12) | ((lo & 0x7ff) << 1)
        if off & 0x400000:
            off -= 0x800000
        return addr + 4 + off
    if typ in (R_ARM_PC24, R_ARM_CALL, R_ARM_JUMP24):
        w = mod.u32(addr)
        off = (w & 0xffffff) << 2
        if off & 0x2000000:
            off -= 0x4000000
        return addr + 8 + off
    if typ == R_ARM_ABS32:
        return mod.u32(addr)
    return None


def main():
    out_path, dirs = sys.argv[1], sys.argv[2:]
    sigs = []
    for d in dirs:
        for p in glob.glob(os.path.join(d, "**", "*.o"), recursive=True):
            if "arm7" in p.lower():
                continue
            sigs.extend(signatures(p))
    print("signatures: %d functions from %d objects" % (len(sigs), len({s[5] for s in sigs})))

    mods = romimage.load()
    # Candidate DS functions: every dsd function in main and ITCM (libraries can hide anywhere).
    cands = []
    for mn in ("main", "itcm"):
        m = mods[mn]
        for addr, (name, thumb, size) in m.functions.items():
            cands.append((mn, addr, thumb))
    by_prefix = defaultdict(list)
    for mn, addr, thumb in cands:
        m = mods[mn]
        o = addr - m.base
        by_prefix[(thumb, m.data[o:o + 4])].append((mn, addr))

    hits = defaultdict(set)  # sig index -> {(module, addr)}
    for i, (name, thumb, data, mask, rel, obj) in enumerate(sigs):
        if len(data) < 4:
            continue
        if mask[:4] == b"\xff\xff\xff\xff":
            pool = by_prefix.get((thumb, data[:4]), [])
        else:
            pool = [(mn, a) for mn, a, t in cands if t == thumb]
        for mn, addr in pool:
            m = mods[mn]
            o = addr - m.base
            seg = m.data[o:o + len(data)]
            if len(seg) == len(data) and all((x ^ y) & k == 0 for x, y, k in zip(seg, data, mask)):
                hits[i].add((mn, addr))

    # Unique both ways. Identical functions (same bytes, different names) are ambiguous: skip them.
    addr_sigs = defaultdict(set)
    for i, hs in hits.items():
        for h in hs:
            addr_sigs[h].add(i)
    names = {}  # (module, addr) -> (name, obj, reason)
    for i, hs in hits.items():
        if len(hs) != 1:
            continue
        h = next(iter(hs))
        same = {sigs[j][0] for j in addr_sigs[h]}
        if len(same) == 1:
            names[h] = (sigs[i][0], sigs[i][5], "bytes:%d" % len(sigs[i][2]))
    n_bytes = len(names)

    # Relocation-derived names (callees and data referenced by matched functions).
    derived = defaultdict(set)
    for i, hs in hits.items():
        if len(hs) != 1:
            continue
        mn, addr = next(iter(hs))
        if names.get((mn, addr), (None,))[0] != sigs[i][0]:
            continue
        for off, typ, target in sigs[i][4]:
            if not target or "+" in target or target.startswith("$"):
                continue
            t = decode_target(mods[mn], addr + off, typ)
            if t is None:
                continue
            tm = romimage.resolve(mods, mn, t & ~1)
            if tm:
                derived[(tm.name, t & ~1)].add((target, typ))
    n_derived = 0
    for key, cand in derived.items():
        tnames = {c[0] for c in cand}
        if key in names or len(tnames) != 1:
            continue
        mod = mods[key[0]]
        kind = "function" if key[1] in mod.functions else "data"
        names[key] = (next(iter(tnames)), "-", "reloc-%s" % kind)
        n_derived += 1

    with open(out_path, "w") as w:
        for (mn, addr), (name, obj, why) in sorted(names.items(), key=lambda kv: kv[0][1]):
            key = "%08x" % addr if mn == "main" or mn == "itcm" else "%s:%08x" % (mn, addr)
            w.write("%s %s %s # %s\n" % (key, name, obj, why))
    multi = sum(1 for hs in hits.values() if len(hs) > 1)
    print("byte matches (unique) %d, ambiguous signatures %d, relocation-derived names %d"
          % (n_bytes, multi, n_derived))


if __name__ == "__main__":
    main()
