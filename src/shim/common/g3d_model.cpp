// NitroSystem G3D models (BMD0: MDL0 + TEX0), platform-independent part: parsing, texture decoding, display-list
// decoding. Layouts follow tools/assets/nitro3d.py (apicula, GBATEK). The drawing back-ends are
// src/shim/host/g3d_gl.cpp (OpenGL 1.x) and src/shim/psp/g3d_gu.cpp (sceGu).
#include "g3d_model.h"

#include <math.h>
#include <string.h>

#include <algorithm>

static u32 R32(const std::vector<u8> &d, size_t o) { return o + 4 <= d.size() ? d[o] | d[o + 1] << 8 | d[o + 2] << 16 | (u32)d[o + 3] << 24 : 0; }
static u16 R16(const std::vector<u8> &d, size_t o) { return o + 2 <= d.size() ? (u16)(d[o] | d[o + 1] << 8) : 0; }
static s32 SBits(u32 v, int n) { return (s32)(v << (32 - n)) >> (32 - n); }

// Nitro dictionary: entries of `size` bytes, then 16-byte names.
struct Dict {
    std::vector<size_t> data;      // file offset of each entry
    std::vector<std::string> names;
};
static Dict ReadDict(const std::vector<u8> &d, size_t off)
{
    Dict r;
    if (off + 4 > d.size())
        return r;
    int count = d[off + 1];
    size_t p = off + 4 + 8 + 4 * count;
    u16 size = R16(d, p);
    p += 4;
    for (int i = 0; i < count; i++, p += size)
        r.data.push_back(p);
    for (int i = 0; i < count; i++, p += 16) {
        char n[17] = {};
        if (p + 16 <= d.size())
            memcpy(n, &d[p], 16);
        r.names.push_back(n);
    }
    return r;
}

static const int TEX_BPP[8] = {0, 8, 2, 4, 8, 2, 8, 16};

bool G3TexSet::Parse(const std::vector<u8> &d, u32 off)
{
    u32 texInfo = R16(d, off + 0x0e), texOff = R32(d, off + 0x14);
    u32 cmpOff = R32(d, off + 0x24), cmp2Off = R32(d, off + 0x28);
    u32 palLen = R16(d, off + 0x30) << 3, palInfo = R32(d, off + 0x34), palOff = R32(d, off + 0x38);
    if (off + palOff + palLen <= d.size())
        palData.assign(d.begin() + off + palOff, d.begin() + off + palOff + palLen);
    Dict td = ReadDict(d, off + texInfo);
    for (size_t i = 0; i < td.data.size(); i++) {
        Tex t;
        t.name = td.names[i];
        t.params = R32(d, td.data[i]);
        t.w = 8 << ((t.params >> 20) & 7);
        t.h = 8 << ((t.params >> 23) & 7);
        t.fmt = (t.params >> 26) & 7;
        t.c0t = (t.params >> 29) & 1;
        size_t o = (size_t)(t.params & 0xffff) << 3, ln = (size_t)t.w * t.h * TEX_BPP[t.fmt] / 8;
        size_t b1 = off + (t.fmt == 5 ? cmpOff : texOff) + o;
        if (b1 + ln <= d.size())
            t.d1.assign(d.begin() + b1, d.begin() + b1 + ln);
        if (t.fmt == 5) {
            size_t b2 = off + cmp2Off + o / 2;
            if (b2 + ln / 2 <= d.size())
                t.d2.assign(d.begin() + b2, d.begin() + b2 + ln / 2);
        }
        tex.push_back(t);
    }
    Dict pd = ReadDict(d, off + palInfo);
    for (size_t i = 0; i < pd.data.size(); i++)
        pal.push_back({pd.names[i], (u32)R16(d, pd.data[i]) << 3});
    return true;
}

int G3TexSet::FindTex(const std::string &n) const
{
    for (size_t i = 0; i < tex.size(); i++)
        if (tex[i].name == n)
            return (int)i;
    return -1;
}

int G3TexSet::FindPal(const std::string &n) const
{
    for (size_t i = 0; i < pal.size(); i++)
        if (pal[i].name == n)
            return (int)i;
    return -1;
}

static void Rgb555(u16 v, u8 a, u8 *o)
{
    o[0] = (u8)((v & 31) * 255 / 31);
    o[1] = (u8)(((v >> 5) & 31) * 255 / 31);
    o[2] = (u8)(((v >> 10) & 31) * 255 / 31);
    o[3] = a;
}

G3Texture G3TexSet::Decode(int ti, int pi) const
{
    G3Texture r;
    if (ti < 0 || ti >= (int)tex.size())
        return r;
    const Tex &t = tex[ti];
    r.w = t.w;
    r.h = t.h;
    r.rgba.assign((size_t)t.w * t.h * 4, 0);
    u32 po = pi >= 0 && pi < (int)pal.size() ? pal[pi].offset : 0;
    auto palc = [&](u32 i) -> u16 {
        size_t o = po + 2 * i;
        return o + 2 <= palData.size() ? (u16)(palData[o] | palData[o + 1] << 8) : 0;
    };
    const std::vector<u8> &d = t.d1;
    auto at = [&](size_t i) -> u8 { return i < d.size() ? d[i] : 0; };
    for (int y = 0; y < t.h; y++)
        for (int x = 0; x < t.w; x++) {
            u8 *o = &r.rgba[((size_t)y * t.w + x) * 4];
            size_t i = (size_t)y * t.w + x;
            switch (t.fmt) {
            case 7: {
                u16 v = (u16)(at(2 * i) | at(2 * i + 1) << 8);
                Rgb555(v, v & 0x8000 ? 255 : 0, o);
                break;
            }
            case 2: case 3: case 4: {
                int bpp = TEX_BPP[t.fmt];
                size_t bit = i * bpp;
                u32 idx = (at(bit >> 3) >> (bit & 7)) & ((1u << bpp) - 1);
                Rgb555(palc(idx), idx == 0 && t.c0t ? 0 : 255, o);
                break;
            }
            case 1: case 6: {
                int ib = t.fmt == 1 ? 5 : 3;
                u8 v = at(i);
                u32 idx = v & ((1u << ib) - 1), a = v >> ib;
                u8 a8 = (u8)(t.fmt == 1 ? a * 255 / 7 : a * 255 / 31);
                Rgb555(palc(idx), a8, o);
                if (a8 != 0 && a8 != 255)
                    r.translucent = true;
                break;
            }
            default:
                break;
            }
        }
    if (t.fmt == 5) {
        const std::vector<u8> &d2 = t.d2;
        for (int by = 0; by < t.h / 4; by++)
            for (int bx = 0; bx < t.w / 4; bx++) {
                size_t b = (size_t)by * (t.w / 4) + bx;
                u32 texels = 4 * b + 3 < d.size() ? d[4 * b] | d[4 * b + 1] << 8 | d[4 * b + 2] << 16 | (u32)d[4 * b + 3] << 24 : 0;
                u16 extra = 2 * b + 1 < d2.size() ? (u16)(d2[2 * b] | d2[2 * b + 1] << 8) : 0;
                u32 mode = extra >> 14, base = (extra & 0x3fff) << 1;
                u8 c[4][4] = {};
                Rgb555(palc(base), 255, c[0]);
                Rgb555(palc(base + 1), 255, c[1]);
                if (mode == 0 || mode == 2)
                    Rgb555(palc(base + 2), 255, c[2]);
                if (mode == 2)
                    Rgb555(palc(base + 3), 255, c[3]);
                for (int k = 0; k < 3; k++) {
                    if (mode == 1)
                        c[2][k] = (u8)((c[0][k] + c[1][k]) / 2);
                    if (mode == 3) {
                        c[2][k] = (u8)((5 * c[0][k] + 3 * c[1][k]) / 8);
                        c[3][k] = (u8)((3 * c[0][k] + 5 * c[1][k]) / 8);
                    }
                }
                if (mode == 1)
                    c[2][3] = 255;
                if (mode == 3)
                    c[2][3] = c[3][3] = 255;
                for (int ty = 0; ty < 4; ty++)
                    for (int tx = 0; tx < 4; tx++) {
                        u32 sel = (texels >> (2 * (ty * 4 + tx))) & 3;
                        memcpy(&r.rgba[(((size_t)by * 4 + ty) * t.w + bx * 4 + tx) * 4], c[sel], 4);
                    }
            }
    }
    return r;
}

// GL matrix (column major) of a DS object transform: v' = v * (S R) + T.
static void ObjectMatrix(const std::vector<u8> &d, size_t off, float *m)
{
    const float FX = 1.0f / 4096;
    u16 flags = R16(d, off);
    s16 m0 = (s16)R16(d, off + 2);
    size_t p = off + 4;
    float t[3] = {0, 0, 0}, r[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1}, s[3] = {1, 1, 1};
    if (!(flags & 1)) {
        for (int i = 0; i < 3; i++, p += 4)
            t[i] = (s32)R32(d, p) * FX;
    }
    if (flags & 8) {
        // Pivot-compressed rotation: +-1 at the pivot, a 2x2 block of A, B, +-B, +-A elsewhere.
        float a = (s16)R16(d, p) * FX, b = (s16)R16(d, p + 2) * FX;
        p += 4;
        int pivot = (flags >> 4) & 15, neg = (flags >> 8) & 15;
        for (float &v : r)
            v = 0;
        r[pivot] = (neg & 1) ? -1.0f : 1.0f;
        int pr = pivot / 3, pc = pivot % 3, k = 0;
        float vals[4] = {a, b, (neg & 2) ? -b : b, (neg & 4) ? -a : a};
        for (int row = 0; row < 3; row++)
            for (int col = 0; col < 3; col++)
                if (row != pr && col != pc)
                    r[row * 3 + col] = vals[k++];
    } else if (!(flags & 2)) {
        r[0] = m0 * FX;
        for (int i = 1; i < 9; i++, p += 2)
            r[i] = (s16)R16(d, p) * FX;
    }
    if (!(flags & 4)) {
        for (int i = 0; i < 3; i++, p += 4)
            s[i] = (s32)R32(d, p) * FX;
    }
    memset(m, 0, 16 * sizeof(float));
    for (int row = 0; row < 3; row++)
        for (int col = 0; col < 3; col++)
            m[row * 4 + col] = s[row] * r[row * 3 + col];
    m[12] = t[0];
    m[13] = t[1];
    m[14] = t[2];
    m[15] = 1;
}

bool G3Model::Load(const std::vector<u8> &d)
{
    if (d.size() < 0x14 || memcmp(d.data(), "BMD0", 4) != 0)
        return false;
    int nsec = R16(d, 0x0e);
    size_t mdl = 0, tex0 = 0;
    for (int i = 0; i < nsec; i++) {
        u32 so = R32(d, 0x10 + 4 * i);
        if (so + 4 > d.size())
            continue;
        if (!memcmp(&d[so], "MDL0", 4))
            mdl = so;
        if (!memcmp(&d[so], "TEX0", 4))
            tex0 = so;
    }
    if (!mdl)
        return false;
    G3TexSet &ts = texSet;
    if (tex0)
        ts.Parse(d, (u32)tex0);
    Dict models = ReadDict(d, mdl + 8);
    if (models.data.empty())
        return false;
    size_t off = mdl + R32(d, models.data[0]);
    u32 rcOff = R32(d, off + 4), matOff = R32(d, off + 8), shapeOff = R32(d, off + 12);
    upScale = (s32)R32(d, off + 0x1c) / 4096.0f;
    downScale = (s32)R32(d, off + 0x20) / 4096.0f;
    size_t objDict = off + 0x40;
    Dict objs = ReadDict(d, objDict);
    for (size_t i = 0; i < objs.data.size(); i++) {
        Node n;
        ObjectMatrix(d, objDict + R32(d, objs.data[i]), n.m);
        nodes.push_back(n);
    }

    if (matOff & 0x80000000u) {
        batchMaterials = true;
    } else {
        size_t mo = off + matOff;
        u16 texPair = R16(d, mo), palPair = R16(d, mo + 2);
        Dict md = ReadDict(d, mo + 4);
        std::vector<int> texOf(md.data.size(), -1), palOf(md.data.size(), -1);
        for (size_t i = 0; i < md.data.size(); i++) {
            size_t p = mo + R32(d, md.data[i]);
            Material m;
            m.difAmb = R32(d, p + 4);
            m.polyAttr = R32(d, p + 12);
            m.w = R16(d, p + 0x2c);
            m.h = R16(d, p + 0x2e);
            materials.push_back(m);
        }
        for (int k = 0; k < 2; k++) {
            Dict pd = ReadDict(d, mo + (k == 0 ? texPair : palPair));
            for (size_t i = 0; i < pd.data.size(); i++) {
                u16 o = R16(d, pd.data[i]);
                u8 num = d[pd.data[i] + 2];
                int idx = k == 0 ? ts.FindTex(pd.names[i]) : ts.FindPal(pd.names[i]);
                for (int j = 0; j < num; j++) {
                    size_t mid = d[mo + o + j];
                    if (mid < materials.size())
                        (k == 0 ? texOf : palOf)[mid] = idx;
                }
            }
        }
        for (size_t i = 0; i < materials.size(); i++) {
            if (texOf[i] < 0)
                continue;
            G3Texture t = ts.Decode(texOf[i], palOf[i]);
            u32 tp = ts.tex[texOf[i]].params;
            (void)tp;
            materials[i].texture = (int)textures.size();
            textures.push_back(t);
        }
    }
    // A container without MDL0 materials but with a TEX0 (texturebatch.n): texture i with palette i.
    if (tex0 && (batchMaterials || textures.empty())) {
        for (size_t i = 0; i < ts.tex.size(); i++) {
            int pi = ts.FindPal(ts.tex[i].name + "_pl");
            textures.push_back(ts.Decode((int)i, pi >= 0 ? pi : (int)i));
        }
    }

    Dict sd = ReadDict(d, off + shapeOff);
    for (size_t i = 0; i < sd.data.size(); i++) {
        size_t p = off + shapeOff + R32(d, sd.data[i]);
        u32 co = R32(d, p + 8), cl = R32(d, p + 12);
        std::vector<u8> dl;
        if (p + co + cl <= d.size())
            dl.assign(d.begin() + p + co, d.begin() + p + co + cl);
        shapes.push_back(dl);
    }

    static const int RP[256] = {
        /*00*/ 0, 0, 2, 1, 1, 1, 3, 1, 1, -1, -1, 0, 2, 2, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
        -1, -1, -1, -1, -1, -1, -1, -1, /*24*/ 1, -1, 4, -1, -1, -1, -1, 0, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
        -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, /*40*/ 0, -1, -1, -1, 1, -1, 4, 2};
    auto params = [&](u8 op) -> int {
        if (op == 0x66)
            return 5;
        if (op == 0x80)
            return 0;
        return op < 0x48 ? RP[op] : -1;
    };
    size_t p = off + rcOff;
    while (p < d.size()) {
        u8 op = d[p++];
        if (op == 0x09) {
            p += 2 + 3 * d[p + 1];
            continue;
        }
        int n = params(op);
        if (n < 0)
            break;
        std::vector<u8> a(d.begin() + p, d.begin() + std::min(d.size(), p + n));
        p += n;
        if (op == 0x01)
            break;
        if (op == 0x04 || op == 0x24 || op == 0x44)
            ops.push_back({1, a[0]});
        else if (op == 0x05)
            ops.push_back({2, a[0]});
        else if (op == 0x06 || op == 0x26 || op == 0x46 || op == 0x66)
            ops.push_back({0, a[0]});
        else if (op == 0x0b)
            ops.push_back({3, 0});
        else if (op == 0x2b)
            ops.push_back({4, 0});
    }
    return true;
}

bool G3Model::LoadMaterialBin(const std::vector<u8> &d)
{
    u32 count = R32(d, 0), names = 8 + R32(d, 4);
    if (count == 0 || names + count * 48 > d.size())
        return false;
    std::vector<G3Texture> tex;
    std::vector<Material> mats;
    size_t p = 8;
    for (u32 i = 0; i < count; i++) {
        Material m;
        m.difAmb = R32(d, p + 4);
        m.polyAttr = R32(d, p + 12);
        m.texParam = R32(d, p + 20);
        m.w = R16(d, p + 0x24);
        m.h = R16(d, p + 0x26);
        p += R16(d, p + 2);
        auto name = [&](int k) {
            char n[17] = {};
            memcpy(n, &d[names + i * 48 + k * 16], 16);
            return std::string(n);
        };
        int ti = texSet.FindTex(name(1)), pi = texSet.FindPal(name(2));
        if (ti >= 0) {
            G3Texture t = texSet.Decode(ti, pi);
            t.repeatS = (m.texParam >> 16) & 1;
            t.repeatT = (m.texParam >> 17) & 1;
            t.flipS = (m.texParam >> 18) & 1;
            t.flipT = (m.texParam >> 19) & 1;
            m.texture = (int)tex.size();
            tex.push_back(t);
        }
        mats.push_back(m);
    }
    textures = tex;
    materials = mats;
    return true;
}

// Parameter counts of the GXFIFO commands 0x00..0x7f (-1: unknown, ends the list).
static const int GPU_PARAMS[0x80] = {
    /*00*/ 0, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
    /*10*/ 1, 0, 1, 1, 1, 0, 16, 12, 16, 12, 9, 3, 3, -1, -1, -1,
    /*20*/ 1, 1, 1, 2, 1, 1, 1, 1, 1, 1, 1, 1, -1, -1, -1, -1,
    /*30*/ 1, 1, 1, 1, 32, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
    /*40*/ 1, 0, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
    /*50*/ 1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
    /*60*/ 1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
    /*70*/ 3, 2, 1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1};

void G3DecodeShape(const std::vector<u8> &dl, G3ShapeSink &sink)
{
    const float FX = 1.0f / 4096;
    float pos[3] = {0, 0, 0};
    bool inPrim = false;
    size_t i = 0, n = dl.size();
    while (i + 4 <= n) {
        u8 opsb[4] = {dl[i], dl[i + 1], dl[i + 2], dl[i + 3]};
        i += 4;
        for (u8 op : opsb) {
            int k = op < 0x80 ? GPU_PARAMS[op] : -1;
            if (k < 0) {
                if (inPrim)
                    sink.End();
                return;
            }
            u32 p0 = k ? R32(dl, i) : 0, p1 = k > 1 ? R32(dl, i + 4) : 0;
            i += 4 * k;
            bool vtx = false;
            switch (op) {
            case 0x40:
                if (inPrim)
                    sink.End();
                sink.Begin((int)(p0 & 3));
                inPrim = true;
                break;
            case 0x41:
                if (inPrim)
                    sink.End();
                inPrim = false;
                break;
            case 0x20:
                sink.Color(p0 & 0x7fff);
                break;
            case 0x21:
                sink.Normal(SBits(p0 & 1023, 10) / 512.0f, SBits((p0 >> 10) & 1023, 10) / 512.0f,
                            SBits((p0 >> 20) & 1023, 10) / 512.0f);
                break;
            case 0x22:
                sink.TexCoord((s16)(p0 & 0xffff), (s16)(p0 >> 16));
                break;
            case 0x23:
                pos[0] = (s16)(p0 & 0xffff) * FX;
                pos[1] = (s16)(p0 >> 16) * FX;
                pos[2] = (s16)(p1 & 0xffff) * FX;
                vtx = true;
                break;
            case 0x24:
                pos[0] = SBits(p0 & 1023, 10) / 64.0f;
                pos[1] = SBits((p0 >> 10) & 1023, 10) / 64.0f;
                pos[2] = SBits((p0 >> 20) & 1023, 10) / 64.0f;
                vtx = true;
                break;
            case 0x25:
                pos[0] = (s16)(p0 & 0xffff) * FX;
                pos[1] = (s16)(p0 >> 16) * FX;
                vtx = true;
                break;
            case 0x26:
                pos[0] = (s16)(p0 & 0xffff) * FX;
                pos[2] = (s16)(p0 >> 16) * FX;
                vtx = true;
                break;
            case 0x27:
                pos[1] = (s16)(p0 & 0xffff) * FX;
                pos[2] = (s16)(p0 >> 16) * FX;
                vtx = true;
                break;
            case 0x28:
                pos[0] += SBits(p0 & 1023, 10) * FX;
                pos[1] += SBits((p0 >> 10) & 1023, 10) * FX;
                pos[2] += SBits((p0 >> 20) & 1023, 10) * FX;
                vtx = true;
                break;
            default:
                break;
            }
            if (vtx && inPrim)
                sink.Vertex(pos);
        }
    }
    if (inPrim)
        sink.End();
}

// Bounding sphere: the ops are replayed as the back-ends draw them (node matrix, then the current scale; scale up /
// down), every decoded vertex is transformed, and the sphere is centred on the box of the points.
namespace {
struct BoundSink : G3ShapeSink {
    float m[16];
    std::vector<float> pts;
    void Begin(int) override {}
    void End() override {}
    void Color(u32) override {}
    void TexCoord(s16, s16) override {}
    void Vertex(const float *p) override
    {
        for (int i = 0; i < 3; i++)
            pts.push_back(m[i] * p[0] + m[4 + i] * p[1] + m[8 + i] * p[2] + m[12 + i]);
    }
};

void MulScale(float *m, float s)
{
    for (int i = 0; i < 12; i++)
        m[i] *= s;
}
} // namespace

void G3Model::ComputeBounds()
{
    BoundSink sink;
    static const float id[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    memcpy(sink.m, id, sizeof id);
    float scale = 1;
    for (const Op &o : ops) {
        switch (o.kind) {
        case 0:
            memcpy(sink.m, o.a < (int)nodes.size() ? nodes[o.a].m : id, sizeof id);
            if (scale != 1)
                MulScale(sink.m, scale);
            break;
        case 2:
            if (o.a < (int)shapes.size())
                G3DecodeShape(shapes[o.a], sink);
            break;
        case 3:
            scale = upScale;
            MulScale(sink.m, upScale);
            break;
        case 4:
            if (scale != 1)
                MulScale(sink.m, 1 / scale);
            scale = 1;
            break;
        }
    }
    hasBounds = !sink.pts.empty();
    if (!hasBounds)
        return;
    float lo[3], hi[3];
    for (int i = 0; i < 3; i++)
        lo[i] = hi[i] = sink.pts[i];
    for (size_t k = 0; k < sink.pts.size(); k += 3)
        for (int i = 0; i < 3; i++) {
            lo[i] = std::min(lo[i], sink.pts[k + i]);
            hi[i] = std::max(hi[i], sink.pts[k + i]);
        }
    float r2 = 0;
    for (int i = 0; i < 3; i++)
        center[i] = (lo[i] + hi[i]) * 0.5f;
    for (size_t k = 0; k < sink.pts.size(); k += 3) {
        float dx = sink.pts[k] - center[0], dy = sink.pts[k + 1] - center[1], dz = sink.pts[k + 2] - center[2];
        r2 = std::max(r2, dx * dx + dy * dy + dz * dz);
    }
    radius = sqrtf(r2);
}
