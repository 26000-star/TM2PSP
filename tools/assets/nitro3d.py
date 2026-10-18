"""Reader for Nintendo DS NitroSystem 3D containers (BMD0 models, TEX0 textures) as used by the game.

Layouts follow apicula (github.com/scurest/apicula, src/nitro, src/nds) and GBATEK. Everything is
returned as plain Python data so converters can turn it into PSP-native formats:
  - Model: render ops (matrix stack / material / draw), materials, objects (node transforms),
    pieces (meshes) with their GPU display lists decoded into primitives.
  - Texture/Palette: raw DS data plus params; decode_texture() returns RGBA8888 rows.
"""
import struct

FX = 1 / 4096.0

# DS geometry command parameter counts (GBATEK "DS 3D Geometry Commands").
GPU_PARAMS = {
    0x00: 0, 0x10: 1, 0x11: 0, 0x12: 1, 0x13: 1, 0x14: 1, 0x15: 0, 0x16: 16, 0x17: 12, 0x18: 16,
    0x19: 12, 0x1A: 9, 0x1B: 3, 0x1C: 3, 0x20: 1, 0x21: 1, 0x22: 1, 0x23: 2, 0x24: 1, 0x25: 1,
    0x26: 1, 0x27: 1, 0x28: 1, 0x29: 1, 0x2A: 1, 0x2B: 1, 0x30: 1, 0x31: 1, 0x32: 1, 0x33: 1,
    0x34: 32, 0x40: 1, 0x41: 0, 0x50: 1, 0x60: 1, 0x70: 3, 0x71: 2, 0x72: 1,
}
PRIM_NAMES = ("triangles", "quads", "triangle_strip", "quad_strip")
# bits per texel for formats 0..7: none, A3I5, 4-color, 16-color, 256-color, 4x4 compressed, A5I3, direct
TEX_BPP = (0, 8, 2, 4, 8, 2, 8, 16)
TEX_FORMAT_NAMES = ("none", "a3i5", "pal4", "pal16", "pal256", "tex4x4", "a5i3", "direct")
RENDER_PARAMS = {0x00: 0, 0x01: 0, 0x02: 2, 0x03: 1, 0x04: 1, 0x05: 1, 0x06: 3, 0x07: 1, 0x08: 1,
                 0x0B: 0, 0x0C: 2, 0x0D: 2, 0x24: 1, 0x26: 4, 0x2B: 0, 0x40: 0, 0x44: 1, 0x46: 4,
                 0x47: 2, 0x66: 5, 0x80: 0}


def s16(v):
    return v - 0x10000 if v & 0x8000 else v


def s32(v):
    return v - 0x100000000 if v & 0x80000000 else v


def sbits(v, n):
    return v - (1 << n) if v & (1 << (n - 1)) else v


class Cur:
    def __init__(self, data, pos=0):
        self.data, self.pos = data, pos

    def u8(self):
        self.pos += 1
        return self.data[self.pos - 1]

    def u16(self):
        self.pos += 2
        return struct.unpack_from("<H", self.data, self.pos - 2)[0]

    def u32(self):
        self.pos += 4
        return struct.unpack_from("<I", self.data, self.pos - 4)[0]


def info_block(data, off, datum):
    """Nitro dictionary: [(datum tuple, name)]; datum is a struct format such as '<I'."""
    c = Cur(data, off)
    dummy, count, _ = c.u8(), c.u8(), c.u16()
    if dummy != 0:
        raise ValueError("bad info block at %#x" % off)
    c.pos += 8 + 4 * count  # unknown subheader + per-entry unknown words
    size, _ = c.u16(), c.u16()
    if size != struct.calcsize(datum):
        raise ValueError("info block datum size %d != %d at %#x" % (size, struct.calcsize(datum), off))
    items = []
    for _ in range(count):
        items.append(struct.unpack_from(datum, data, c.pos))
        c.pos += size
    names = []
    for _ in range(count):
        names.append(data[c.pos:c.pos + 16].split(b"\0")[0].decode("latin1"))
        c.pos += 16
    return list(zip(items, names))


def decode_display_list(cmds):
    """Decodes a DS GPU display list into primitives.

    Returns (prims, other): prims is a list of dicts {type, verts:[(pos, uv, color, normal, mtx)]}
    with float positions/uvs; `other` counts commands that carry state beyond geometry.
    """
    prims, other = [], {}
    pos = [0.0, 0.0, 0.0]
    uv, color, normal, mtx = None, None, None, None
    cur = None
    i = 0
    n = len(cmds)
    while i + 4 <= n:
        ops = cmds[i:i + 4]
        i += 4
        for op in ops:
            if op not in GPU_PARAMS:
                raise ValueError("unknown GPU opcode %#x" % op)
            k = GPU_PARAMS[op]
            p = struct.unpack_from("<%dI" % k, cmds, i) if k else ()
            i += 4 * k
            if op == 0x00:
                continue
            if op == 0x14:
                mtx = p[0] & 31
            elif op == 0x40:
                cur = {"type": PRIM_NAMES[p[0] & 3], "verts": []}
                prims.append(cur)
            elif op == 0x41:
                cur = None
            elif op == 0x20:
                v = p[0]
                color = ((v & 31) / 31, ((v >> 5) & 31) / 31, ((v >> 10) & 31) / 31)
            elif op == 0x21:
                v = p[0]
                normal = (sbits(v & 1023, 10) / 512, sbits((v >> 10) & 1023, 10) / 512,
                          sbits((v >> 20) & 1023, 10) / 512)
            elif op == 0x22:
                v = p[0]
                uv = (s16(v & 0xFFFF) / 16.0, s16(v >> 16) / 16.0)  # texels (1.11.4)
            elif op in (0x23, 0x24, 0x25, 0x26, 0x27, 0x28):
                if op == 0x23:
                    pos = [s16(p[0] & 0xFFFF) * FX, s16(p[0] >> 16) * FX, s16(p[1] & 0xFFFF) * FX]
                elif op == 0x24:
                    v = p[0]
                    pos = [sbits(v & 1023, 10) / 64, sbits((v >> 10) & 1023, 10) / 64,
                           sbits((v >> 20) & 1023, 10) / 64]
                elif op == 0x25:
                    pos = [s16(p[0] & 0xFFFF) * FX, s16(p[0] >> 16) * FX, pos[2]]
                elif op == 0x26:
                    pos = [s16(p[0] & 0xFFFF) * FX, pos[1], s16(p[0] >> 16) * FX]
                elif op == 0x27:
                    pos = [pos[0], s16(p[0] & 0xFFFF) * FX, s16(p[0] >> 16) * FX]
                else:
                    v = p[0]  # VTX_DIFF: 1.0.9 deltas in units of 1/8, i.e. 1/4096 per step
                    pos = [pos[0] + sbits(v & 1023, 10) / 4096, pos[1] + sbits((v >> 10) & 1023, 10) / 4096,
                           pos[2] + sbits((v >> 20) & 1023, 10) / 4096]
                if cur is None:
                    other["vertex_outside_begin"] = other.get("vertex_outside_begin", 0) + 1
                else:
                    cur["verts"].append((tuple(pos), uv, color, normal, mtx))
            else:
                other[op] = other.get(op, 0) + 1
    return prims, other


def parse_render_ops(data, off):
    ops = []
    c = Cur(data, off)
    while True:
        op = c.u8()
        if op == 0x09:
            store, count = c.u8(), c.u8()
            terms = [(c.u8(), c.u8(), c.u8() / 256.0) for _ in range(count)]
            ops.append(("skin", store, terms))
            continue
        if op not in RENDER_PARAMS:
            raise ValueError("unknown render command %#x" % op)
        p = [c.u8() for _ in range(RENDER_PARAMS[op])]
        if op == 0x01:
            return ops
        if op == 0x03:
            ops.append(("load", p[0]))
        elif op in (0x04, 0x24, 0x44):
            ops.append(("material", p[0]))
        elif op == 0x05:
            ops.append(("draw", p[0]))
        elif op in (0x06, 0x26, 0x46, 0x66):
            store = p[3] if op in (0x26, 0x66) else None
            load = p[3] if op == 0x46 else (p[4] if op == 0x66 else None)
            ops.append(("node", p[0], p[1], load, store))
        elif op == 0x0B:
            ops.append(("scale_up",))
        elif op == 0x2B:
            ops.append(("scale_down",))
        elif op not in (0x00, 0x02):
            ops.append(("op%02x" % op, p))


def parse_object(data, off):
    c = Cur(data, off)
    flags = c.u16()
    m0 = c.u16()
    t = None
    rot = None
    scale = None
    if not flags & 1:
        t = tuple(s32(c.u32()) * FX for _ in range(3))
    if flags & 8:  # pivot-compressed rotation
        a, b = s16(c.u16()) * FX, s16(c.u16()) * FX
        rot = ("pivot", (flags >> 4) & 15, (flags >> 8) & 15, a, b)
    elif not flags & 2:
        m = [m0] + [c.u16() for _ in range(8)]
        rot = ("matrix", tuple(s16(v) * FX for v in m))
    if not flags & 4:
        scale = tuple(s32(c.u32()) * FX for _ in range(3))
    return {"translation": t, "rotation": rot, "scale": scale}


def parse_material(data, off):
    c = Cur(data, off)
    c.u16()
    c.u16()
    dif_amb, spe_emi, poly_attr, poly_mask, teximage, _, pltt_base, misc = (
        c.u32(), c.u32(), c.u32(), c.u32(), c.u32(), c.u32(), c.u16(), c.u16())
    width, height = c.u16(), c.u16()
    return {"dif_amb": dif_amb, "spe_emi": spe_emi, "polygon_attr": poly_attr, "polygon_attr_mask": poly_mask,
            "teximage_param": teximage & poly_mask, "alpha": ((poly_attr >> 16) & 31) / 31,
            "cull_back": not (poly_attr >> 6) & 1, "cull_front": not (poly_attr >> 7) & 1,
            "width": width, "height": height, "tex_matrix": bool(misc & 1), "texture": None, "palette": None}


def parse_model(data, off, name):
    c = Cur(data, off)
    size, rc_off, mat_off, piece_off, invb_off = c.u32(), c.u32(), c.u32(), c.u32(), c.u32()
    c.pos += 3
    n_obj, n_mat, n_piece = c.u8(), c.u8(), c.u8()
    c.pos += 2
    up, down = s32(c.u32()) * FX, s32(c.u32()) * FX
    n_verts, n_surfs, n_tris, n_quads = c.u16(), c.u16(), c.u16(), c.u16()
    bbox = [s16(c.u16()) * FX for _ in range(6)]
    c.pos += 8
    objects = [dict(parse_object(data, c.pos + o), name=nm) for (o,), nm in info_block(data, c.pos, "<I")]

    if mat_off & 0x80000000:
        # Firebrand extension: no material dictionary; a count byte then one index per material into
        # the environment's shared texturebatch.n (Graphics/Tracks/<env>/texturebatch.n).
        mo = off + (mat_off & 0x7FFFFFFF)
        count = data[mo]
        materials = [{"name": None, "batch_index": data[mo + 1 + i], "texture": None, "palette": None,
                      "tex_matrix": False} for i in range(count)]
    else:
        tex_pair_off, pal_pair_off = struct.unpack_from("<HH", data, off + mat_off)
        materials = [dict(parse_material(data, off + mat_off + o), name=nm)
                     for (o,), nm in info_block(data, off + mat_off + 4, "<I")]
        for key, pair_off in (("texture", tex_pair_off), ("palette", pal_pair_off)):
            for (o, num, _), nm in info_block(data, off + mat_off + pair_off, "<HBB"):
                for mid in data[off + mat_off + o: off + mat_off + o + num]:
                    materials[mid][key] = nm

    pieces = []
    for (o,), nm in info_block(data, off + piece_off, "<I"):
        p = off + piece_off + o
        _, sect, _, cmds_off, cmds_len = struct.unpack_from("<HHIII", data, p)
        cmds = data[p + cmds_off:p + cmds_off + cmds_len]
        prims, other = decode_display_list(cmds)
        pieces.append({"name": nm, "cmds": cmds, "prims": prims, "other": other})

    return {"name": name, "objects": objects, "materials": materials, "pieces": pieces,
            "ops": parse_render_ops(data, off + rc_off), "up_scale": up, "down_scale": down,
            "counts": {"verts": n_verts, "surfs": n_surfs, "tris": n_tris, "quads": n_quads}, "bbox": bbox}


def parse_tex0(data, off):
    (tex_len3, tex_info, tex_off, cmp_len3, cmp_info, cmp_off, cmp2_off, pal_len3, pal_info, pal_off) = (
        struct.unpack_from("<H", data, off + 0x0C)[0], struct.unpack_from("<H", data, off + 0x0E)[0],
        struct.unpack_from("<I", data, off + 0x14)[0], struct.unpack_from("<H", data, off + 0x1C)[0],
        struct.unpack_from("<H", data, off + 0x1E)[0], struct.unpack_from("<I", data, off + 0x24)[0],
        struct.unpack_from("<I", data, off + 0x28)[0], struct.unpack_from("<H", data, off + 0x30)[0],
        struct.unpack_from("<I", data, off + 0x34)[0], struct.unpack_from("<I", data, off + 0x38)[0])
    pal_block = data[off + pal_off: off + pal_off + (pal_len3 << 3)]
    textures = []
    for (params, extra), nm in info_block(data, off + tex_info, "<II"):
        w, h = 8 << ((params >> 20) & 7), 8 << ((params >> 23) & 7)
        fmt = (params >> 26) & 7
        o = (params & 0xFFFF) << 3
        ln = w * h * TEX_BPP[fmt] // 8
        if fmt == 5:
            d1 = data[off + cmp_off + o: off + cmp_off + o + ln]
            d2 = data[off + cmp2_off + o // 2: off + cmp2_off + o // 2 + ln // 2]
        else:
            d1, d2 = data[off + tex_off + o: off + tex_off + o + ln], b""
        textures.append({"name": nm, "params": params, "width": w, "height": h, "format": fmt,
                         "color0_transparent": bool(params >> 29 & 1), "data": d1, "data2": d2})
    palettes = [{"name": nm, "offset": po << 3} for (po, _), nm in info_block(data, off + pal_info, "<HH")]
    return textures, palettes, pal_block


def read_container(data):
    magic = data[:4]
    if magic not in (b"BMD0", b"BTX0", b"BTA0", b"BCA0", b"BTP0"):
        raise ValueError("not a Nitro 3D container: %r" % magic)
    n = struct.unpack_from("<H", data, 0x0E)[0]
    out = {"magic": magic.decode(), "models": [], "textures": [], "palettes": [], "pal_block": b"", "sections": []}
    for i in range(n):
        so = struct.unpack_from("<I", data, 0x10 + 4 * i)[0]
        tag = data[so:so + 4].decode("latin1")
        out["sections"].append(tag)
        if tag == "MDL0":
            for (o,), nm in info_block(data, so + 8, "<I"):
                out["models"].append(parse_model(data, so + o, nm))
        elif tag == "TEX0":
            out["textures"], out["palettes"], out["pal_block"] = parse_tex0(data, so)
    return out


def rgb555(v, alpha=255):
    return ((v & 31) * 255 // 31, ((v >> 5) & 31) * 255 // 31, ((v >> 10) & 31) * 255 // 31, alpha)


def decode_texture(tex, pal_block, pal_offset):
    """RGBA8888 pixels (list of rows) for any DS texture format."""
    w, h, fmt, d = tex["width"], tex["height"], tex["format"], tex["data"]

    def pal(i):
        o = pal_offset + 2 * i
        return struct.unpack_from("<H", pal_block, o)[0] if o + 2 <= len(pal_block) else 0

    px = [[(0, 0, 0, 0)] * w for _ in range(h)]
    if fmt == 7:
        for y in range(h):
            for x in range(w):
                v = struct.unpack_from("<H", d, 2 * (y * w + x))[0]
                px[y][x] = rgb555(v, 255 if v & 0x8000 else 0)
    elif fmt in (2, 3, 4):
        bpp = TEX_BPP[fmt]
        for y in range(h):
            for x in range(w):
                bit = (y * w + x) * bpp
                idx = (d[bit >> 3] >> (bit & 7)) & ((1 << bpp) - 1)
                a = 0 if idx == 0 and tex["color0_transparent"] else 255
                px[y][x] = rgb555(pal(idx), a)
    elif fmt in (1, 6):
        ib = 5 if fmt == 1 else 3
        for y in range(h):
            for x in range(w):
                v = d[y * w + x]
                idx, a = v & ((1 << ib) - 1), v >> ib
                a8 = (a * 255 // 7) if fmt == 1 else (a * 255 // 31)
                px[y][x] = rgb555(pal(idx), a8)
    elif fmt == 5:
        d2 = tex["data2"]
        for by in range(h // 4):
            for bx in range(w // 4):
                b = by * (w // 4) + bx
                texels = struct.unpack_from("<I", d, 4 * b)[0]
                extra = struct.unpack_from("<H", d2, 2 * b)[0]
                mode, base = extra >> 14, (extra & 0x3FFF) << 1
                c0, c1 = rgb555(pal(base)), rgb555(pal(base + 1))
                if mode == 0:
                    cols = [c0, c1, rgb555(pal(base + 2)), (0, 0, 0, 0)]
                elif mode == 1:
                    mid = tuple((a + b2) // 2 for a, b2 in zip(c0[:3], c1[:3])) + (255,)
                    cols = [c0, c1, mid, (0, 0, 0, 0)]
                elif mode == 2:
                    cols = [c0, c1, rgb555(pal(base + 2)), rgb555(pal(base + 3))]
                else:
                    m1 = tuple((5 * a + 3 * b2) // 8 for a, b2 in zip(c0[:3], c1[:3])) + (255,)
                    m2 = tuple((3 * a + 5 * b2) // 8 for a, b2 in zip(c0[:3], c1[:3])) + (255,)
                    cols = [c0, c1, m1, m2]
                for ty in range(4):
                    for tx in range(4):
                        sel = (texels >> (2 * (ty * 4 + tx))) & 3
                        px[by * 4 + ty][bx * 4 + tx] = cols[sel]
    return px
