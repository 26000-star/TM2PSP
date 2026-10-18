"""Converts a DS BMD0 model (nitro3d.py) into TMPM, a PSP-native model file for sceGu.

Design:
  - Geometry: every piece becomes one indexed GU_TRIANGLES mesh (quads split, strips unrolled,
    vertices deduplicated, 16-bit indices). Positions keep the DS 1.3.12 fixed-point int16 values
    (GU_VERTEX_16BIT; the renderer scales by 1/4096). UVs keep the DS texel*16 int16 values
    (GU_TEXTURE_16BIT; per-material texture scale = 2048/width). Normals are 8-bit, colours 5650,
    each only when the piece uses them.
  - Textures: palette formats become CLUT4/CLUT8 (A3I5/A5I3 fold their alpha into an 8888 CLUT),
    direct colour and 4x4-compressed become 5551 (DS colours are 5-bit, so this is near lossless).
    Texture data is swizzled for the GE.
  - Materials: texture index, alpha, culling, wrap modes, DS colours as 8888.

File layout (little endian), see read_tmpm() for the reverse:
  Header  : "TMPM" u16 version u16 flags | u16 n_tex n_mat n_mesh n_draw | f32 up_scale |
            f32 node[12] (3x4, row-major) | u32 off_tex off_mat off_mesh off_draw
  Texture : u16 w h | u8 psm swizzled clut_psm pad | u16 clut_count pad | u32 data_off data_size clut_off
  Material: s16 tex | u8 alpha(0..31) flags | u32 diffuse ambient specular emission | f32 su sv  (28 bytes)
  Mesh    : u32 vtype | u16 vsize pad | u32 n_verts n_idx vtx_off idx_off
  Draw    : u16 material mesh
"""
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import nitro3d  # noqa: E402

# sceGu constants (pspgu.h)
GU_PSM_5650, GU_PSM_5551, GU_PSM_4444, GU_PSM_8888, GU_PSM_T4, GU_PSM_T8 = 0, 1, 2, 3, 4, 5
GU_TEXTURE_16BIT = 2 << 0
GU_COLOR_5650 = 4 << 2
GU_NORMAL_8BIT = 1 << 5
GU_VERTEX_16BIT = 2 << 7

MAT_CULL_BACK, MAT_CULL_FRONT, MAT_REPEAT_S, MAT_REPEAT_T, MAT_MIRROR_S, MAT_MIRROR_T, MAT_TRANSLUCENT = (
    1, 2, 4, 8, 16, 32, 64)


def swizzle(data, width_bytes, height):
    """GE texture swizzle: 16-byte x 8-row blocks, row-major blocks."""
    out = bytearray(len(data))
    o = 0
    for by in range(0, height, 8):
        for bx in range(0, width_bytes, 16):
            for y in range(by, min(by + 8, height)):
                row = y * width_bytes + bx
                out[o:o + 16] = data[row:row + 16]
                o += 16
    return bytes(out)


def c8888(rgba):
    r, g, b, a = rgba
    return r | g << 8 | b << 16 | a << 24


def c5551(rgba):
    r, g, b, a = rgba
    return (r >> 3) | (g >> 3) << 5 | (b >> 3) << 10 | (1 if a >= 128 else 0) << 15


def convert_texture(tex, pal_block, pal_offset):
    """-> dict(psm, w, h, data(bytes, swizzled), clut_psm, clut(bytes))."""
    w, h, fmt = tex["width"], tex["height"], tex["format"]

    def pal(i):
        o = pal_offset + 2 * i
        v = struct.unpack_from("<H", pal_block, o)[0] if o + 2 <= len(pal_block) else 0
        return nitro3d.rgb555(v)

    if fmt in (2, 3):  # 2bpp / 4bpp palette -> T4 (DS and GE both store the left pixel in the low bits)
        bpp = nitro3d.TEX_BPP[fmt]
        out = bytearray(w * h // 2)
        for i in range(w * h):
            bit = i * bpp
            idx = (tex["data"][bit >> 3] >> (bit & 7)) & ((1 << bpp) - 1)
            out[i >> 1] |= idx << (4 * (i & 1))
        n = 1 << bpp
        cl = [pal(i)[:3] + ((0,) if i == 0 and tex["color0_transparent"] else (255,)) for i in range(n)]
        cl += [(0, 0, 0, 0)] * (16 - n)
        return {"psm": GU_PSM_T4, "w": w, "h": h, "data": swizzle(bytes(out), w // 2, h),
                "clut_psm": GU_PSM_8888, "clut": b"".join(struct.pack("<I", c8888(c)) for c in cl)}
    if fmt in (1, 4, 6):  # 8-bit index formats -> T8, alpha folded into the CLUT for A3I5/A5I3
        if fmt == 4:
            cl = [pal(i)[:3] + ((0,) if i == 0 and tex["color0_transparent"] else (255,)) for i in range(256)]
        else:
            ib = 5 if fmt == 1 else 3
            amax = 7 if fmt == 1 else 31
            cl = [pal(i & ((1 << ib) - 1))[:3] + ((i >> ib) * 255 // amax,) for i in range(256)]
        return {"psm": GU_PSM_T8, "w": w, "h": h, "data": swizzle(bytes(tex["data"]), w, h),
                "clut_psm": GU_PSM_8888, "clut": b"".join(struct.pack("<I", c8888(c)) for c in cl)}
    # Direct colour and 4x4-compressed: decode, store 5551.
    px = nitro3d.decode_texture(tex, pal_block, pal_offset)
    out = b"".join(struct.pack("<H", c5551(p)) for row in px for p in row)
    return {"psm": GU_PSM_5551, "w": w, "h": h, "data": swizzle(out, w * 2, h), "clut_psm": 0, "clut": b""}


def triangles(prim):
    v = prim["verts"]
    t = prim["type"]
    if t == "triangles":
        return [(v[i], v[i + 1], v[i + 2]) for i in range(0, len(v) - 2, 3)]
    if t == "quads":
        out = []
        for i in range(0, len(v) - 3, 4):
            out += [(v[i], v[i + 1], v[i + 2]), (v[i], v[i + 2], v[i + 3])]
        return out
    if t == "triangle_strip":
        return [(v[i], v[i + 1], v[i + 2]) if i % 2 == 0 else (v[i + 1], v[i], v[i + 2]) for i in range(len(v) - 2)]
    # Quad strip (v0 v1 v3 v2 per quad, two vertices per step): same winding as a triangle strip.
    out = []
    for i in range(0, len(v) - 3, 2):
        out += [(v[i], v[i + 1], v[i + 2]), (v[i + 1], v[i + 3], v[i + 2])]
    return out


def convert_mesh(piece):
    tris = [t for pr in piece["prims"] for t in triangles(pr)]
    has_uv = any(v[1] is not None for t in tris for v in t)
    has_col = any(v[2] is not None for t in tris for v in t)
    has_nrm = any(v[3] is not None for t in tris for v in t)
    vtype = GU_VERTEX_16BIT | (GU_TEXTURE_16BIT if has_uv else 0) | (GU_COLOR_5650 if has_col else 0) | (
        GU_NORMAL_8BIT if has_nrm else 0)
    verts, index, idx = [], {}, []
    for t in tris:
        for pos, uv, col, nrm, _ in t:
            key = (tuple(round(p * 4096) for p in pos), uv, col, nrm)
            if key not in index:
                index[key] = len(verts)
                verts.append(key)
            idx.append(index[key])
    if len(verts) > 65535:
        raise ValueError("mesh %s has %d vertices" % (piece["name"], len(verts)))
    out = bytearray()
    for pos, uv, col, nrm in verts:
        v = bytearray()
        if has_uv:
            u, w = uv or (0.0, 0.0)
            v += struct.pack("<hh", max(-32768, min(32767, round(u * 16))), max(-32768, min(32767, round(w * 16))))
        if has_col:
            r, g, b = col or (1.0, 1.0, 1.0)
            v += struct.pack("<H", round(r * 31) | round(g * 63) << 5 | round(b * 31) << 11)
        if has_nrm:
            n = nrm or (0.0, 1.0, 0.0)
            v += struct.pack("<bbb", *(max(-127, min(127, round(c * 127))) for c in n)) + b"\0"
        v += struct.pack("<hhh", *(max(-32768, min(32767, p)) for p in pos))
        if len(v) % 2:
            v += b"\0"
        out += v
    vsize = len(out) // max(1, len(verts))
    return {"vtype": vtype, "vsize": vsize, "verts": bytes(out), "n_verts": len(verts),
            "indices": struct.pack("<%dH" % len(idx), *idx), "n_idx": len(idx)}


def node_matrix(obj):
    """3x4 row-major matrix of a node (scale, then rotation, then translation)."""
    m = [[1.0, 0, 0, 0], [0, 1.0, 0, 0], [0, 0, 1.0, 0]]
    if obj["scale"]:
        for k in range(3):
            m[k][k] = obj["scale"][k]
    rot = obj["rotation"]
    if rot and rot[0] == "matrix":
        r = rot[1]
        m = [[sum(r[3 * i + k] * m[k][j] for k in range(3)) for j in range(3)] + [0.0] for i in range(3)]
    if obj["translation"]:
        for k in range(3):
            m[k][3] = obj["translation"][k]
    return [c for row in m for c in row]


def convert_model(container, model_index=0):
    m = container["models"][model_index]
    texs = container["textures"]
    pal_by_name = {p["name"]: p["offset"] for p in container["palettes"]}
    tex_index, textures, materials = {}, [], []
    for mat in m["materials"]:
        ti = -1
        if mat.get("texture"):
            name = mat["texture"]
            if name not in tex_index:
                t = next((t for t in texs if t["name"] == name), None)
                if t:
                    tex_index[name] = len(textures)
                    textures.append(convert_texture(t, container["pal_block"], pal_by_name.get(mat.get("palette"), 0)))
            ti = tex_index.get(name, -1)
        params = mat.get("teximage_param", 0)
        flags = (MAT_CULL_BACK if mat.get("cull_back") else 0) | (MAT_CULL_FRONT if mat.get("cull_front") else 0)
        flags |= (MAT_REPEAT_S if params >> 16 & 1 else 0) | (MAT_REPEAT_T if params >> 17 & 1 else 0)
        flags |= (MAT_MIRROR_S if params >> 18 & 1 else 0) | (MAT_MIRROR_T if params >> 19 & 1 else 0)
        alpha = round(mat.get("alpha", 1.0) * 31)
        if alpha < 31:
            flags |= MAT_TRANSLUCENT
        da, se = mat.get("dif_amb", 0x7FFF), mat.get("spe_emi", 0)

        def col(v):
            return c8888(nitro3d.rgb555(v & 0x7FFF))
        w = textures[ti]["w"] if ti >= 0 else 1
        h = textures[ti]["h"] if ti >= 0 else 1
        materials.append({"tex": ti, "alpha": alpha, "flags": flags, "diffuse": col(da), "ambient": col(da >> 16),
                          "specular": col(se), "emission": col(se >> 16), "su": 2048.0 / w, "sv": 2048.0 / h,
                          "batch_index": mat.get("batch_index")})
    meshes = [convert_mesh(p) for p in m["pieces"]]
    draws, cur = [], 0
    for op in m["ops"]:
        if op[0] == "material":
            cur = op[1]
        elif op[0] == "draw":
            draws.append((cur, op[1]))
    up = m["up_scale"] if any(o[0] == "scale_up" for o in m["ops"]) else 1.0
    return {"textures": textures, "materials": materials, "meshes": meshes, "draws": draws, "up_scale": up,
            "node": node_matrix(m["objects"][0]) if m["objects"] else [1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0]}


def align(buf, n=16):
    buf += b"\0" * (-len(buf) % n)


def write_tmpm(model):
    hdr_size = 4 + 4 + 8 + 4 + 48 + 16
    tex_tab = 24 * len(model["textures"])
    mat_tab = 28 * len(model["materials"])
    mesh_tab = 24 * len(model["meshes"])
    draw_tab = 4 * len(model["draws"])
    off_tex = (hdr_size + 15) & ~15
    off_mat = (off_tex + tex_tab + 15) & ~15
    off_mesh = (off_mat + mat_tab + 15) & ~15
    off_draw = (off_mesh + mesh_tab + 15) & ~15
    blob = bytearray(b"\0" * ((off_draw + draw_tab + 15) & ~15))
    struct.pack_into("<4sHH4Hf12f4I", blob, 0, b"TMPM", 1, 0, len(model["textures"]), len(model["materials"]),
                     len(model["meshes"]), len(model["draws"]), model["up_scale"], *model["node"],
                     off_tex, off_mat, off_mesh, off_draw)
    for i, t in enumerate(model["textures"]):
        align(blob)
        data_off = len(blob)
        blob += t["data"]
        align(blob)
        clut_off = len(blob) if t["clut"] else 0
        blob += t["clut"]
        struct.pack_into("<HHBBBBHHIII", blob, off_tex + 24 * i, t["w"], t["h"], t["psm"], 1, t["clut_psm"], 0,
                         len(t["clut"]) // 4, 0, data_off, len(t["data"]), clut_off)
    for i, mt in enumerate(model["materials"]):
        struct.pack_into("<hBB4I2f", blob, off_mat + 28 * i, mt["tex"], mt["alpha"], mt["flags"], mt["diffuse"],
                         mt["ambient"], mt["specular"], mt["emission"], mt["su"], mt["sv"])
    for i, ms in enumerate(model["meshes"]):
        align(blob)
        vtx_off = len(blob)
        blob += ms["verts"]
        align(blob)
        idx_off = len(blob)
        blob += ms["indices"]
        struct.pack_into("<IHHIIII", blob, off_mesh + 24 * i, ms["vtype"], ms["vsize"], 0, ms["n_verts"],
                         ms["n_idx"], vtx_off, idx_off)
    for i, (mat, mesh) in enumerate(model["draws"]):
        struct.pack_into("<HH", blob, off_draw + 4 * i, mat, mesh)
    align(blob)
    return bytes(blob)


def read_tmpm(data):
    """Parses a TMPM file back (used by tests)."""
    (magic, ver, flags, nt, nm, nme, nd, up), rest = struct.unpack_from("<4sHH4Hf", data, 0), None
    node = struct.unpack_from("<12f", data, 20)
    off_tex, off_mat, off_mesh, off_draw = struct.unpack_from("<4I", data, 68)
    meshes = [struct.unpack_from("<IHHIIII", data, off_mesh + 24 * i) for i in range(nme)]
    return {"magic": magic, "version": ver, "n_tex": nt, "n_mat": nm, "meshes": meshes,
            "draws": [struct.unpack_from("<HH", data, off_draw + 4 * i) for i in range(nd)], "up_scale": up, "node": node}


if __name__ == "__main__":
    src, dst = sys.argv[1], sys.argv[2]
    mdl = convert_model(nitro3d.read_container(open(src, "rb").read()))
    blob = write_tmpm(mdl)
    open(dst, "wb").write(blob)
    print("%s -> %s: %d bytes, %d textures, %d materials, %d meshes (%d tris), %d draws" % (
        os.path.basename(src), dst, len(blob), len(mdl["textures"]), len(mdl["materials"]), len(mdl["meshes"]),
        sum(m["n_idx"] for m in mdl["meshes"]) // 3, len(mdl["draws"])))
