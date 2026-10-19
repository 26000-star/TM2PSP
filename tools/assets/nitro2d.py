"""Reader for Nintendo DS NitroSystem 2D files (NCLR palettes, NCGR tiles, NSCR screens, NCER cells).

Layouts from GBATEK and the well-known g2d file format notes (files are 'RLCN', 'RGCN', 'NCSC'/'RCSN',
'RECN' containers with reversed block tags TTLP / RAHC / NRCS / KBEC). Renders to RGBA rows so the
images can be checked as PNG; the PSP port reads the raw files through its 2D engine shim.

Usage (inspection): python -P tools/assets/nitro2d.py <dir with .NCGR/.NCLR/.nsc/.NCER> <out dir>
"""
import glob
import os
import struct
import sys


def blocks(data):
    """{tag: payload} for a Nitro 2D container (tags stored reversed, e.g. b'RAHC' for CHAR)."""
    hdr_size, n = struct.unpack_from("<HH", data, 0x0C)
    out, o = {}, hdr_size
    # Runtime files store tags reversed (b'RAHC'); NITRO-CHARACTER editor files (NCSC) store them as is.
    rev = data[:4] in (b"RGCN", b"RLCN", b"RCSN", b"RECN", b"RNAN")
    for _ in range(n):
        raw = data[o:o + 4]
        tag, size = (raw[::-1] if rev else raw).decode("latin1"), struct.unpack_from("<I", data, o + 4)[0]
        out[tag] = data[o + 8:o + size]
        o += size
    return out


def bgr555(v, a=255):
    return ((v & 31) * 255 // 31, ((v >> 5) & 31) * 255 // 31, ((v >> 10) & 31) * 255 // 31, a)


def read_nclr(data):
    b = blocks(data)["PLTT"]
    bit_depth, _, size, off = struct.unpack_from("<IIII", b, 0)
    raw = b[off:off + size]  # offsets count from the block payload (after the 8-byte block header)
    colors = [struct.unpack_from("<H", raw, i)[0] for i in range(0, len(raw) - 1, 2)]
    return {"bpp": 4 if bit_depth == 3 else 8, "colors": colors}


def read_ncgr(data):
    b = blocks(data)["CHAR"]
    h_tiles, w_tiles, bit_depth, mapping, tiled_flag, size, off = struct.unpack_from("<HHIIIII", b, 0)
    bpp = 4 if bit_depth == 3 else 8
    raw = b[off:off + size]
    return {"bpp": bpp, "w_tiles": w_tiles, "h_tiles": h_tiles, "data": raw,
            "linear": bool(tiled_flag & 1), "mapping": mapping}


def read_nscr(data):
    b = blocks(data)["SCRN"]
    if data[:4] == b"NCSC":
        # Editor screen: u32 width and height in tiles, two u32 settings, then u16 map entries.
        wt, ht = struct.unpack_from("<II", b, 0)
        entries = [struct.unpack_from("<H", b, 16 + 2 * i)[0] for i in range(wt * ht)]
        return {"width": wt * 8, "height": ht * 8, "entries": entries}
    w, h, _, size = struct.unpack_from("<HHII", b, 0)
    entries = [struct.unpack_from("<H", b, 12 + i)[0] for i in range(0, size, 2)]
    return {"width": w, "height": h, "entries": entries}


def read_ncer(data):
    b = blocks(data)["CEBK"]
    n_cells, attr_type, cell_off, mapping, vram_off = struct.unpack_from("<HHIII", b, 0)
    cells = []
    base = 8 + cell_off - 8 if cell_off else 0x18
    entry_size = 16 if attr_type == 1 else 8
    oam_base = base + n_cells * entry_size
    for i in range(n_cells):
        n_oam, _, oam_off = struct.unpack_from("<HHI", b, base + i * entry_size)
        oams = []
        for k in range(n_oam):
            a0, a1, a2 = struct.unpack_from("<HHH", b, oam_base + oam_off + 6 * k)
            oams.append((a0, a1, a2))
        cells.append(oams)
    # VRAM transfer table (offsets from the block payload): u32 max size, u32 table offset, then per cell
    # {u32 source offset, u32 size} in the NCGR. Such cells are DMA'd to their own VRAM slot, so their OAM tile
    # numbers count from that cell's source data.
    srcs = [0] * n_cells
    if vram_off:
        tab = vram_off + struct.unpack_from("<I", b, vram_off + 4)[0]
        srcs = [struct.unpack_from("<I", b, tab + 8 * i)[0] for i in range(n_cells)]
    # mapping: OBJ 1D boundary 32 << mapping bytes (0..3 = 32K..256K), the unit of the OAM tile number.
    return {"cells": cells, "srcs": srcs, "boundary": 32 << mapping if mapping < 4 else 32}


def tile_pixel(ncgr, tile, x, y, base=0):
    bpp = ncgr["bpp"]
    tbytes = 8 * bpp
    o = base + tile * tbytes + (y * 8 + x) * bpp // 8
    if o >= len(ncgr["data"]):
        return 0
    v = ncgr["data"][o]
    return (v >> (4 * (x & 1))) & 15 if bpp == 4 else v


def render_screen(ncgr, nclr, nscr):
    w, h = nscr["width"], nscr["height"]
    px = [[(0, 0, 0, 0)] * w for _ in range(h)]
    cols = w // 8
    for i, e in enumerate(nscr["entries"]):
        tx, ty = (i % cols) * 8, (i // cols) * 8
        if ty >= h:
            break
        tile, hf, vf, pal = e & 0x3FF, e >> 10 & 1, e >> 11 & 1, e >> 12
        for y in range(8):
            for x in range(8):
                c = tile_pixel(ncgr, tile, 7 - x if hf else x, 7 - y if vf else y)
                if c:
                    idx = pal * 16 + c if ncgr["bpp"] == 4 else c
                    px[ty + y][tx + x] = bgr555(nclr["colors"][idx] if idx < len(nclr["colors"]) else 0)
    return px


OBJ_SIZES = {(0, 0): (8, 8), (0, 1): (16, 16), (0, 2): (32, 32), (0, 3): (64, 64),
             (1, 0): (16, 8), (1, 1): (32, 8), (1, 2): (32, 16), (1, 3): (64, 32),
             (2, 0): (8, 16), (2, 1): (8, 32), (2, 2): (16, 32), (2, 3): (32, 64)}


def render_cell(ncgr, nclr, oams, boundary=32, src=0):
    """Draws one cell (list of OAM entries) into a canvas sized to its bounding box."""
    parts = []
    for a0, a1, a2 in oams:
        y = a0 & 0xFF
        y = y - 256 if y >= 128 else y
        x = a1 & 0x1FF
        x = x - 512 if x >= 256 else x
        w, h = OBJ_SIZES.get((a0 >> 14, a1 >> 14), (8, 8))
        parts.append((x, y, w, h, a2 & 0x3FF, a2 >> 12, a1 >> 12 & 1, a1 >> 13 & 1))
    if not parts:
        return []
    x0, y0 = min(p[0] for p in parts), min(p[1] for p in parts)
    x1, y1 = max(p[0] + p[2] for p in parts), max(p[1] + p[3] for p in parts)
    px = [[(0, 0, 0, 0)] * (x1 - x0) for _ in range(y1 - y0)]
    for x, y, w, h, tile, pal, hf, vf in parts:
        first = src + tile * boundary  # 1D mapping: the OAM tile number counts boundary-sized bytes, any bpp
        for ty in range(h // 8):
            for tx in range(w // 8):
                t = ty * (w // 8) + tx
                for yy in range(8):
                    for xx in range(8):
                        c = tile_pixel(ncgr, t, xx, yy, first)
                        if not c:
                            continue
                        dx = (w - 1 - (tx * 8 + xx)) if hf else tx * 8 + xx
                        dy = (h - 1 - (ty * 8 + yy)) if vf else ty * 8 + yy
                        idx = pal * 16 + c if ncgr["bpp"] == 4 else c
                        px[y - y0 + dy][x - x0 + dx] = bgr555(nclr["colors"][idx] if idx < len(nclr["colors"]) else 0)
    return px


def save_png(px, path):
    from PIL import Image
    if not px or not px[0]:
        return
    img = Image.new("RGBA", (len(px[0]), len(px)))
    img.putdata([p for row in px for p in row])
    img.save(path)


def main():
    src, out = sys.argv[1], sys.argv[2]
    os.makedirs(out, exist_ok=True)
    files = {os.path.splitext(os.path.basename(p))[0].lower(): p for p in glob.glob(os.path.join(src, "*"))}
    byext = {}
    for p in glob.glob(os.path.join(src, "*")):
        byext.setdefault(os.path.splitext(p)[1].lower(), []).append(p)
    n = 0
    for scr in byext.get(".nsc", []) + byext.get(".nscr", []):
        base = os.path.splitext(scr)[0]
        cg = next((base + e for e in (".NCGR", ".ncgr") if os.path.exists(base + e)), None)
        cl = next((base + e for e in (".NCLR", ".nclr") if os.path.exists(base + e)), None)
        if cg and cl:
            save_png(render_screen(read_ncgr(open(cg, "rb").read()), read_nclr(open(cl, "rb").read()),
                                   read_nscr(open(scr, "rb").read())),
                     os.path.join(out, os.path.basename(base) + ".png"))
            n += 1
    for cer in byext.get(".ncer", []):
        base = os.path.splitext(cer)[0]
        cg = base + ".NCGR"
        cl = next((p for p in [base + ".NCLR"] + glob.glob(os.path.join(src, "*.NCLR")) if os.path.exists(p)), None)
        if not (os.path.exists(cg) and cl):
            continue
        cells = read_ncer(open(cer, "rb").read())
        ncgr, nclr = read_ncgr(open(cg, "rb").read()), read_nclr(open(cl, "rb").read())
        for i, oams in enumerate(cells["cells"]):
            save_png(render_cell(ncgr, nclr, oams, cells["boundary"], cells["srcs"][i]),
                     os.path.join(out, "%s_%02d.png" % (os.path.basename(base), i)))
            n += 1
    print("rendered %d screens to %s (%d files scanned)" % (n, out, len(files)))


if __name__ == "__main__":
    main()
