// G3D stand-in back-end on sceGu (PSP), see g3d_model.h. Same drawing rules as the host back-end (g3d_gl.cpp):
// node matrices, material colour x vertex colour, polygon alpha, culling from POLYGON_ATTR, nearest filtering,
// no lighting. Differences forced by the hardware:
//  - display lists are converted once (on first draw) into GU_TRIANGLES vertex arrays per (material, shape),
//    with the material colour and alpha baked into the vertices; quads and strips become triangles;
//  - textures become 16-bit 5551 (DS colours are 5 bits, so lossless) or 8888 when they have partial alpha,
//    swizzled; the GE has no mirrored repeat, so a flipped repeating axis is stored mirrored at twice the size;
//    the GE takes at most 512 x 512, larger textures are halved (nearest).
#include "g3d_model.h"

#include <malloc.h>
#include <string.h>

#include <map>

#include <pspgu.h>
#include <pspgum.h>
#include <pspkernel.h>

namespace {

struct GuTex {
    void *data = nullptr;
    int w = 0, h = 0, psm = GU_PSM_5551;
    float su = 1, sv = 1;      // texture coordinate scale (0.5 on a mirrored axis)
    bool repeatS = true, repeatT = true;
    size_t bytes = 0;
};

struct GuVertex {
    float u, v;
    u32 color;
    float x, y, z;
};
const int GU_VTYPE = GU_TEXTURE_32BITF | GU_COLOR_8888 | GU_VERTEX_32BITF | GU_TRANSFORM_3D;

struct GuMesh {
    GuVertex *v = nullptr;
    int count = 0;
};

struct GuModelData {
    std::map<u64, GuMesh> meshes; // key: material source, material index, shape
};

size_t g_textureBytes, g_meshBytes;

// Longest triangle edge kept by the conversion, in world units (see GuSink::Emit).
const float G3_GU_MAX_EDGE = 3.0f;

// Swizzles a texture of `bw` bytes per row and h rows (bw multiple of 16, h multiple of 8) into dst.
void Swizzle(u8 *dst, const u8 *src, int bw, int h)
{
    int rowBlocks = bw / 16;
    for (int by = 0; by < h / 8; by++)
        for (int bx = 0; bx < rowBlocks; bx++)
            for (int y = 0; y < 8; y++) {
                memcpy(dst, src + (size_t)(by * 8 + y) * bw + bx * 16, 16);
                dst += 16;
            }
}

GuTex *MakeTexture(const G3Texture &t)
{
    GuTex *g = new GuTex;
    g->repeatS = t.repeatS;
    g->repeatT = t.repeatT;
    int w = t.w, h = t.h;
    bool mirS = t.repeatS && t.flipS, mirT = t.repeatT && t.flipT;
    int ew = mirS ? 2 * w : w, eh = mirT ? 2 * h : h;
    // Texel (x, y) of the expanded (mirrored) image.
    auto at = [&](int x, int y) -> const u8 * {
        if (x >= w)
            x = 2 * w - 1 - x;
        if (y >= h)
            y = 2 * h - 1 - y;
        return &t.rgba[((size_t)y * w + x) * 4];
    };
    int sx = 1, sy = 1;
    while (ew / sx > 512)
        sx *= 2;
    while (eh / sy > 512)
        sy *= 2;
    int ow = ew / sx, oh = eh / sy;
    if (ow < 8)
        ow = 8; // the narrowest DS texture is 8 texels: 16 bytes per row at 16 bits, as the swizzle needs
    g->w = ow;
    g->h = oh;
    g->su = mirS ? 0.5f : 1;
    g->sv = mirT ? 0.5f : 1;
    g->psm = t.translucent ? GU_PSM_8888 : GU_PSM_5551;
    int bpp = t.translucent ? 4 : 2;
    size_t size = (size_t)ow * oh * bpp;
    u8 *linear = (u8 *)malloc(size);
    for (int y = 0; y < oh; y++)
        for (int x = 0; x < ow; x++) {
            const u8 *c = at((x * sx) % ew, (y * sy) % eh);
            u8 *o = linear + ((size_t)y * ow + x) * bpp;
            if (bpp == 4) {
                memcpy(o, c, 4);
            } else {
                u16 v = (u16)((c[0] >> 3) | (c[1] >> 3) << 5 | (c[2] >> 3) << 10 | (c[3] >= 128 ? 0x8000 : 0));
                o[0] = (u8)v;
                o[1] = (u8)(v >> 8);
            }
        }
    g->data = memalign(16, size);
    Swizzle((u8 *)g->data, linear, ow * bpp, oh);
    free(linear);
    sceKernelDcacheWritebackRange(g->data, size);
    g->bytes = size;
    g_textureBytes += size;
    return g;
}

// Display list -> triangle list with the material colour baked in.
struct GuSink : G3ShapeSink {
    std::vector<GuVertex> out, prim;
    int mode = 0;
    float tw, th;
    u32 color;
    float alpha;
    float u = 0, v = 0;
    GuSink(const G3Texture *t, const GuTex *g, u32 dif, float a)
        : tw(t && t->w ? 1.0f / (16.0f * t->w) * (g ? g->su : 1) : 0),
          th(t && t->h ? 1.0f / (16.0f * t->h) * (g ? g->sv : 1) : 0), alpha(a)
    {
        Color(dif);
    }
    void Begin(int m) override
    {
        mode = m;
        prim.clear();
    }
    void End() override
    {
        size_t n = prim.size();
        auto tri = [&](size_t a, size_t b, size_t c) { Emit(prim[a], prim[b], prim[c], 0); };
        switch (mode) {
        case 0:
            for (size_t i = 0; i + 2 < n; i += 3)
                tri(i, i + 1, i + 2);
            break;
        case 1:
            for (size_t i = 0; i + 3 < n; i += 4) {
                tri(i, i + 1, i + 2);
                tri(i, i + 2, i + 3);
            }
            break;
        case 2:
            for (size_t i = 0; i + 2 < n; i++) {
                if (i & 1)
                    tri(i + 1, i, i + 2);
                else
                    tri(i, i + 1, i + 2);
            }
            break;
        case 3: // GL quad strip: quad k = v2k, v2k+1, v2k+3, v2k+2
            for (size_t i = 0; i + 3 < n; i += 2) {
                tri(i, i + 1, i + 3);
                tri(i, i + 3, i + 2);
            }
            break;
        }
        prim.clear();
    }
    // The GE drops a whole triangle when a vertex projects outside its guard band (about +-2048 pixels around the
    // screen centre), which large track triangles near the camera do: the road behind the car vanished on a real
    // PSP (PPSSPP does not reproduce it). Triangles are split along their longest edge until every edge is at most
    // maxEdge model units.
    float maxEdge = 1e30f;
    static GuVertex Mid(const GuVertex &a, const GuVertex &b)
    {
        GuVertex m;
        m.u = (a.u + b.u) * 0.5f;
        m.v = (a.v + b.v) * 0.5f;
        m.x = (a.x + b.x) * 0.5f;
        m.y = (a.y + b.y) * 0.5f;
        m.z = (a.z + b.z) * 0.5f;
        u32 c = 0;
        for (int k = 0; k < 32; k += 8)
            c |= ((((a.color >> k) & 255) + ((b.color >> k) & 255) + 1) / 2) << k;
        m.color = c;
        return m;
    }
    static float Len2(const GuVertex &a, const GuVertex &b)
    {
        float dx = a.x - b.x, dy = a.y - b.y, dz = a.z - b.z;
        return dx * dx + dy * dy + dz * dz;
    }
    void Emit(const GuVertex &a, const GuVertex &b, const GuVertex &c, int depth)
    {
        float ab = Len2(a, b), bc = Len2(b, c), ca = Len2(c, a), lim = maxEdge * maxEdge;
        if (depth < 10 && (ab > lim || bc > lim || ca > lim)) {
            if (ab >= bc && ab >= ca) {
                GuVertex m = Mid(a, b);
                Emit(a, m, c, depth + 1);
                Emit(m, b, c, depth + 1);
            } else if (bc >= ca) {
                GuVertex m = Mid(b, c);
                Emit(a, b, m, depth + 1);
                Emit(a, m, c, depth + 1);
            } else {
                GuVertex m = Mid(c, a);
                Emit(a, b, m, depth + 1);
                Emit(m, b, c, depth + 1);
            }
            return;
        }
        out.push_back(a);
        out.push_back(b);
        out.push_back(c);
    }
    void Color(u32 c) override
    {
        u32 r = (c & 31) * 255 / 31, g = ((c >> 5) & 31) * 255 / 31, b = ((c >> 10) & 31) * 255 / 31;
        u32 a = (u32)(alpha * 255.0f + 0.5f);
        color = r | g << 8 | b << 16 | a << 24;
    }
    void TexCoord(s16 s, s16 t) override
    {
        u = s * tw;
        v = t * th;
    }
    void Vertex(const float *p) override { prim.push_back({u, v, color, p[0], p[1], p[2]}); }
};

} // namespace

G3Model::~G3Model()
{
    for (G3Texture &t : textures)
        if (t.gu) {
            GuTex *g = (GuTex *)t.gu;
            g_textureBytes -= g->bytes;
            free(g->data);
            delete g;
        }
    if (GuModelData *data = (GuModelData *)backend) {
        for (auto &m : data->meshes) {
            g_meshBytes -= sizeof(GuVertex) * m.second.count;
            free(m.second.v);
        }
        delete data;
    }
}

void G3Model::Upload()
{
    for (G3Texture &t : textures) {
        if (t.gu || t.rgba.empty())
            continue;
        t.gu = MakeTexture(t);
        // The RGBA copy is no longer needed: free it (the 64 MB budget matters more than reloading).
        std::vector<u8>().swap(t.rgba);
    }
    if (!backend)
        backend = new GuModelData;
}

void G3Model::Draw(const G3Model *batch) const
{
    DrawOps(ops, batch);
}

void G3Model::DrawPart(int material, int shape, const G3Model *batch) const
{
    DrawOps({{1, material}, {2, shape}}, batch);
}

void G3Model::DrawOps(const std::vector<Op> &ops, const G3Model *batch) const
{
    if (!backend)
        backend = new GuModelData;
    GuModelData *data = (GuModelData *)backend;
    sceGumPushMatrix();
    float scale = 1;
    const G3Model *src = nullptr;
    int mat = -1;
    bool skip = false;
    const G3Texture *tex = nullptr;
    u32 dif = 0x7fff;
    float alpha = 1;
    for (const Op &o : ops) {
        switch (o.kind) {
        case 0:
            sceGumPopMatrix();
            sceGumPushMatrix();
            if (o.a < (int)nodes.size())
                sceGumMultMatrix((const ScePspFMatrix4 *)nodes[o.a].m);
            if (scale != 1) {
                ScePspFVector3 s = {scale, scale, scale};
                sceGumScale(&s);
            }
            break;
        case 1:
            tex = nullptr;
            dif = 0x7fff;
            alpha = 1;
            skip = false;
            mat = o.a;
            src = batchMaterials ? batch : this;
            if (src && o.a < (int)src->materials.size()) {
                const Material &m = src->materials[o.a];
                dif = m.difAmb & 0x7fff;
                if (m.texture >= 0)
                    tex = &src->textures[m.texture];
                // POLYGON_ATTR bit 6 = draw back faces, bit 7 = draw front faces; alpha 0 (wireframe) is drawn
                // opaque, 1..30 translucent, as on the host.
                bool back = (m.polyAttr >> 6) & 1, front = (m.polyAttr >> 7) & 1;
                if (back && front) {
                    sceGuDisable(GU_CULL_FACE);
                } else {
                    sceGuEnable(GU_CULL_FACE);
                    // The GE culls the faces that are not front faces (counter-clockwise, as GL_CCW on the
                    // host, checked on the screenshot); to keep only the back faces the winding is swapped.
                    sceGuFrontFace(front ? GU_CCW : GU_CW);
                }
                skip = !back && !front;
                u32 a = (m.polyAttr >> 16) & 31;
                alpha = a == 0 ? 1.0f : a / 31.0f;
            } else {
                src = nullptr;
            }
            if (tex && tex->gu) {
                const GuTex *g = (const GuTex *)tex->gu;
                sceGuEnable(GU_TEXTURE_2D);
                sceGuTexMode(g->psm, 0, 0, GU_TRUE);
                sceGuTexImage(0, g->w, g->h, g->w, g->data);
                sceGuTexWrap(g->repeatS ? GU_REPEAT : GU_CLAMP, g->repeatT ? GU_REPEAT : GU_CLAMP);
            } else {
                sceGuDisable(GU_TEXTURE_2D);
            }
            break;
        case 2: {
            if (o.a >= (int)shapes.size() || skip)
                break;
            u64 key = (u64)(uintptr_t)src << 32 ^ (u64)(u32)(mat & 0xffff) << 16 ^ (u64)(u32)o.a;
            auto it = data->meshes.find(key);
            if (it == data->meshes.end()) {
                GuSink sink(tex, tex ? (const GuTex *)tex->gu : nullptr, dif, alpha);
                sink.maxEdge = G3_GU_MAX_EDGE / scale;
                G3DecodeShape(shapes[o.a], sink);
                GuMesh m;
                m.count = (int)sink.out.size();
                if (m.count) {
                    size_t bytes = sizeof(GuVertex) * m.count;
                    m.v = (GuVertex *)memalign(16, bytes);
                    memcpy(m.v, sink.out.data(), bytes);
                    sceKernelDcacheWritebackRange(m.v, bytes);
                    g_meshBytes += bytes;
                }
                it = data->meshes.emplace(key, m).first;
            }
            if (it->second.count)
                sceGumDrawArray(GU_TRIANGLES, GU_VTYPE, it->second.count, nullptr, it->second.v);
            break;
        }
        case 3: {
            scale = upScale;
            ScePspFVector3 s = {upScale, upScale, upScale};
            sceGumScale(&s);
            break;
        }
        case 4:
            if (scale != 1) {
                ScePspFVector3 s = {1 / scale, 1 / scale, 1 / scale};
                sceGumScale(&s);
            }
            scale = 1;
            break;
        }
    }
    sceGumPopMatrix();
    sceGuDisable(GU_CULL_FACE);
}

size_t G3GuTextureBytes() { return g_textureBytes; }
size_t G3GuMeshBytes() { return g_meshBytes; }
