// G3D stand-in back-end on sceGu (PSP), see g3d_model.h. Same drawing rules as the host back-end (g3d_gl.cpp):
// node matrices, material colour x vertex colour, polygon alpha, culling from POLYGON_ATTR, nearest filtering,
// no lighting. Differences forced by the hardware:
//  - display lists are converted once (on first draw) into GU_TRIANGLES vertex arrays per (material, shape),
//    with the material colour and alpha baked into the vertices; quads and strips become triangles;
//  - textures with at most 16 / 256 colours become T4 / T8 with an 8888 CLUT of their exact colours (lossless),
//    the others 16-bit 5551 (DS colours are 5 bits) or 8888 when they have partial alpha; all swizzled, in VRAM
//    while the space the driver gives (G3GuSetVram) lasts; the GE has no mirrored repeat, so a flipped repeating axis is stored mirrored at twice the size;
//    the GE takes at most 512 x 512, larger textures are halved (nearest).
#include "g3d_model.h"

#include <alloca.h>
#include <malloc.h>
#include <math.h>
#include <string.h>

#include <map>

#include <pspgu.h>
#include <pspgum.h>
#include <pspkernel.h>

namespace {

struct GuTex {
    void *data = nullptr;
    u32 *clut = nullptr;       // T4 / T8: 8888 palette, clutBlocks blocks of 8 colours
    int clutBlocks = 0;
    bool inVram = false;
    int w = 0, h = 0, tbw = 0, psm = GU_PSM_5551;
    float su = 1, sv = 1;      // texture coordinate scale (0.5 on a mirrored axis)
    bool repeatS = true, repeatT = true;
    size_t bytes = 0;
};

// Conversion vertex: raw DS values (texture coordinates in 1/16 texel, positions in model units).
struct GuVertex {
    float u, v;
    u32 color;
    float x, y, z;
    float n[3];   // last NORMAL in node space (lit meshes), for the relighting of dynamic meshes
    bool hasN;    // the colour came from that normal
};

// Drawn vertex, 16 bytes: the DS positions are 4.12 fixed point within +-8 (VTX_16; VTX_10 and VTX_XY/XZ/YZ are
// coarser), so x 4096 they fit an s16 exactly, and the texture coordinates are the DS s16 values (1/16 texel). The
// GE reads 16-bit values as s16 / 32768 in 3D mode: the model matrix is scaled by 8 and the texture scale is
// 32768 / (16 x texture size) at draw time. Midpoints of split triangles round to the nearest 1/4096 unit and
// 1/16 texel, the same way on both sides of a shared edge.
struct GuVertex16 {
    s16 u, v;
    u32 color;
    s16 x, y, z, pad;
};
const int GU_VTYPE = GU_TEXTURE_16BIT | GU_COLOR_8888 | GU_VERTEX_16BIT | GU_TRANSFORM_3D;
const float GU_POS_SCALE = 8.0f; // 32768 / 4096

struct GuMesh {
    GuVertex16 *v = nullptr;
    int count = 0;
    float su = 0, sv = 0; // sceGuTexScale for this mesh (0: untextured)
    // Dynamic lighting (the car): the distinct node-space normals and, per vertex, the index of its normal (-1:
    // colour from a COLOR command); the colours are recomputed into a copy of `v` in the display list at every draw.
    std::vector<float> normals;
    std::vector<s16> normalOf;
};

int g_clampedVertices;
bool g_prepareOnly; // DrawOps converts without drawing (Prepare) // positions or texture coordinates outside the s16 range (clamped; expected 0)

s16 ToS16(float f)
{
    long r = lrintf(f);
    if (r > 32767 || r < -32768) {
        g_clampedVertices++;
        r = r > 0 ? 32767 : -32768;
    }
    return (s16)r;
}

struct GuModelData {
    std::map<u64, GuMesh> meshes; // key: material source, material index, shape
};

size_t g_textureBytes, g_meshBytes;

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

// VRAM left to textures by the driver (G3GuSetVram): a bump allocator, reset when no VRAM texture is alive (all
// of a track's models are deleted together when the track is unloaded).
u8 *g_vramBase, *g_vramTop;
size_t g_vramSize, g_vramBytes;
int g_vramTextures;

void *AllocTexture(size_t size, bool *inVram)
{
    size_t a = (size + 63) & ~(size_t)63;
    if (g_vramBase && (size_t)(g_vramTop - g_vramBase) + a <= g_vramSize) {
        void *p = g_vramTop;
        g_vramTop += a;
        g_vramBytes += a;
        g_vramTextures++;
        *inVram = true;
        return p;
    }
    *inVram = false;
    return memalign(64, size);
}

void FreeTexture(GuTex *g)
{
    if (!g->inVram) {
        free(g->data);
    } else {
        g_vramBytes -= (g->bytes + 63) & ~(size_t)63;
        if (--g_vramTextures == 0)
            g_vramTop = g_vramBase;
    }
    free(g->clut);
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
    g->w = ow;
    g->h = oh;
    g->su = mirS ? 0.5f : 1;
    g->sv = mirT ? 0.5f : 1;
    std::vector<u32> img((size_t)ow * oh);
    for (int y = 0; y < oh; y++)
        for (int x = 0; x < ow; x++)
            memcpy(&img[(size_t)y * ow + x], at(x * sx, y * sy), 4);

    // Palette: up to 16 colours -> T4, up to 256 -> T8, with an 8888 CLUT of the exact decoded colours (most DS
    // textures are paletted; tex4x4 ones often have more colours). Otherwise 5551 (DS colours are 5 bits) or 8888
    // when the texture has partial alpha.
    std::vector<u32> pal;
    std::map<u32, int> index;
    for (u32 c : img) {
        if (index.count(c))
            continue;
        if (pal.size() == 256)
            break;
        index[c] = (int)pal.size();
        pal.push_back(c);
    }
    bool paletted = index.size() == pal.size() && pal.size() <= 256;
    for (u32 c : img)
        if (paletted && !index.count(c))
            paletted = false;
    int bppx2; // bits per texel / 4
    if (paletted && pal.size() <= 16) {
        g->psm = GU_PSM_T4;
        bppx2 = 1;
    } else if (paletted) {
        g->psm = GU_PSM_T8;
        bppx2 = 2;
    } else {
        g->psm = t.translucent ? GU_PSM_8888 : GU_PSM_5551;
        bppx2 = t.translucent ? 8 : 4;
    }
    // Buffer width: swizzled rows are at least 16 bytes (the texture itself keeps its size).
    int minW = 32 / bppx2;
    g->tbw = ow < minW ? minW : ow;
    size_t rowBytes = (size_t)g->tbw * bppx2 / 2, size = rowBytes * oh;
    std::vector<u8> linear(size, 0);
    for (int y = 0; y < oh; y++)
        for (int x = 0; x < ow; x++) {
            u32 c = img[(size_t)y * ow + x];
            u8 *row = &linear[(size_t)y * rowBytes];
            const u8 *p = (const u8 *)&c;
            switch (g->psm) {
            case GU_PSM_T4:
                row[x / 2] |= (u8)(index[c] << ((x & 1) * 4));
                break;
            case GU_PSM_T8:
                row[x] = (u8)index[c];
                break;
            case GU_PSM_8888:
                memcpy(&row[x * 4], p, 4);
                break;
            default: {
                u16 v = (u16)((p[0] >> 3) | (p[1] >> 3) << 5 | (p[2] >> 3) << 10 | (p[3] >= 128 ? 0x8000 : 0));
                row[x * 2] = (u8)v;
                row[x * 2 + 1] = (u8)(v >> 8);
                break;
            }
            }
        }
    if (paletted) {
        g->clutBlocks = (int)(pal.size() + 7) / 8;
        g->clut = (u32 *)memalign(64, g->clutBlocks * 32);
        memset(g->clut, 0, g->clutBlocks * 32);
        memcpy(g->clut, pal.data(), pal.size() * 4);
        sceKernelDcacheWritebackRange(g->clut, g->clutBlocks * 32);
    }
    // The swizzle needs whole 8-row blocks: DS textures are at least 8 texels high.
    g->data = AllocTexture(size, &g->inVram);
    if (g->inVram) {
        std::vector<u8> sw(size);
        Swizzle(sw.data(), linear.data(), (int)rowBytes, oh);
        memcpy(g->data, sw.data(), size);
        sceKernelDcacheWritebackInvalidateRange(g->data, size);
    } else {
        Swizzle((u8 *)g->data, linear.data(), (int)rowBytes, oh);
        sceKernelDcacheWritebackRange(g->data, size);
    }
    g->bytes = size;
    g_textureBytes += size;
    return g;
}

// Display list -> triangle list with the material colour baked in.
struct GuSink : G3ShapeSink {
    std::vector<GuVertex> out, prim;
    int mode = 0;
    u32 color;
    float alpha;
    float u = 0, v = 0;
    // Lighting (G3SetDrawLight): a NORMAL command computes the vertex colour, as on the DS.
    const G3DrawLight *light = nullptr;
    const G3Lit *lit = nullptr;
    const float *node = nullptr;
    float cn[3] = {0, 0, 0};
    bool hasN = false;
    GuSink(u32 dif, float a) : alpha(a)
    {
        Color(dif);
    }
    void Normal(float x, float y, float z) override
    {
        if (!light || !lit->lit)
            return;
        float n[3] = {x, y, z}, w[3];
        G3WorldNormal(*light, node, n, w);
        Color(G3LightVertex(*lit, *light->env, w, light->eye, light->specular));
        for (int i = 0; i < 3; i++)
            cn[i] = node ? node[i] * x + node[4 + i] * y + node[8 + i] * z : n[i];
        hasN = true;
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
    // maxEdge model units (G3Model::guardEdge, 3 for the track).
    float maxEdge = 1e30f;
    static GuVertex Mid(const GuVertex &a, const GuVertex &b)
    {
        GuVertex m;
        m.u = (a.u + b.u) * 0.5f;
        m.v = (a.v + b.v) * 0.5f;
        m.x = (a.x + b.x) * 0.5f;
        m.y = (a.y + b.y) * 0.5f;
        m.z = (a.z + b.z) * 0.5f;
        for (int k = 0; k < 3; k++)
            m.n[k] = (a.n[k] + b.n[k]) * 0.5f;
        m.hasN = a.hasN && b.hasN;
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
        color = Color8888(c, alpha);
        hasN = false; // a COLOR command (or a new normal, which sets it again) replaces the lit colour
    }
    static u32 Color8888(u32 c, float alpha)
    {
        u32 r = (c & 31) * 255 / 31, g = ((c >> 5) & 31) * 255 / 31, b = ((c >> 10) & 31) * 255 / 31;
        u32 a = (u32)(alpha * 255.0f + 0.5f);
        return r | g << 8 | b << 16 | a << 24;
    }
    void TexCoord(s16 s, s16 t) override
    {
        u = s;
        v = t;
    }
    void Vertex(const float *p) override
    {
        prim.push_back({u, v, color, p[0], p[1], p[2], {cn[0], cn[1], cn[2]}, hasN});
    }
};

} // namespace

G3Model::~G3Model()
{
    for (G3Texture &t : textures)
        if (t.gu) {
            GuTex *g = (GuTex *)t.gu;
            g_textureBytes -= g->bytes;
            FreeTexture(g);
            delete g;
        }
    if (GuModelData *data = (GuModelData *)backend) {
        for (auto &m : data->meshes) {
            g_meshBytes -= sizeof(GuVertex16) * m.second.count + m.second.normals.size() * sizeof(float) +
                           m.second.normalOf.size() * sizeof(s16);
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
    // prepare: convert the meshes only, no GE command (G3Model::Prepare).
    const bool draw = !g_prepareOnly;
    if (!backend)
        backend = new GuModelData;
    GuModelData *data = (GuModelData *)backend;
    const ScePspFVector3 s8 = {GU_POS_SCALE, GU_POS_SCALE, GU_POS_SCALE};
    if (draw) {
        sceGumPushMatrix();
        sceGumScale(&s8);
    }
    float scale = 1;
    const G3Model *src = nullptr;
    int mat = -1, nodeIndex = 0;
    const float *node = nullptr;
    const G3DrawLight *light = G3GetDrawLight();
    (void)node;
    G3Lit lit;
    bool skip = false;
    const G3Texture *tex = nullptr;
    u32 dif = 0x7fff;
    float alpha = 1;
    for (const Op &o : ops) {
        switch (o.kind) {
        case 0:
            nodeIndex = o.a;
            node = o.a < (int)nodes.size() ? nodes[o.a].m : nullptr;
            if (!draw)
                break;
            sceGumPopMatrix();
            sceGumPushMatrix();
            if (o.a < (int)nodes.size())
                sceGumMultMatrix((const ScePspFMatrix4 *)nodes[o.a].m);
            {
                ScePspFVector3 s = {scale * GU_POS_SCALE, scale * GU_POS_SCALE, scale * GU_POS_SCALE};
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
                if (!draw) {
                } else if (back && front) {
                    sceGuDisable(GU_CULL_FACE);
                } else {
                    sceGuEnable(GU_CULL_FACE);
                    // The GE culls the faces that are not front faces (counter-clockwise, as GL_CCW on the
                    // host, checked on the screenshot); to keep only the back faces the winding is swapped.
                    sceGuFrontFace(front ? GU_CCW : GU_CW);
                }
                skip = !back && !front;
                u32 a = (m.polyAttr >> 16) & 31;
                if (light) {
                    lit = G3ResolveMaterial(m, *light->env, light->set);
                    dif = lit.diffuse;
                    if (lit.alpha >= 0)
                        a = (u32)lit.alpha;
                    if (draw) {
                        if (light->env->fog && lit.fog)
                            sceGuEnable(GU_FOG);
                        else
                            sceGuDisable(GU_FOG);
                    }
                }
                alpha = a == 0 ? 1.0f : a / 31.0f;
            } else {
                src = nullptr;
            }
            if (!draw) {
            } else if (tex && tex->gu) {
                const GuTex *g = (const GuTex *)tex->gu;
                sceGuEnable(GU_TEXTURE_2D);
                if (g->clut) {
                    sceGuClutMode(GU_PSM_8888, 0, 0xff, 0);
                    sceGuClutLoad(g->clutBlocks, g->clut);
                }
                sceGuTexMode(g->psm, 0, 0, GU_TRUE);
                sceGuTexImage(0, g->w, g->h, g->tbw, g->data);
                sceGuTexWrap(g->repeatS ? GU_REPEAT : GU_CLAMP, g->repeatT ? GU_REPEAT : GU_CLAMP);
            } else {
                sceGuDisable(GU_TEXTURE_2D);
            }
            break;
        case 2: {
            if (o.a >= (int)shapes.size() || skip)
                break;
            // Lit colours depend on the draw's rotation: baked per variant (piece rotation); variant -1 (the car)
            // is converted at every draw into the display list's memory.
            bool dynamic = light && light->variant < 0;
            int variant = light ? light->variant + 1 : 0;
            u64 key = (u64)(uintptr_t)src << 32 ^ (u64)(u32)(mat & 0xffff) << 16 ^ (u64)(u32)o.a ^
                      (u64)(u32)(variant & 0xff) << 56 ^ (u64)(u32)(nodeIndex & 0xff) << 48;
            auto it = data->meshes.find(key);
            if (it == data->meshes.end()) {
                GuSink sink(dif, alpha);
                sink.maxEdge = guardEdge / scale;
                if (light) {
                    sink.light = light;
                    sink.lit = &lit;
                    sink.node = node;
                }
                G3DecodeShape(shapes[o.a], sink);
                GuMesh m;
                m.count = (int)sink.out.size();
                const GuTex *g = tex ? (const GuTex *)tex->gu : nullptr;
                if (g && tex->w && tex->h) {
                    m.su = 32768.0f / (16.0f * tex->w) * g->su;
                    m.sv = 32768.0f / (16.0f * tex->h) * g->sv;
                }
                if (m.count) {
                    size_t bytes = sizeof(GuVertex16) * m.count;
                    m.v = (GuVertex16 *)memalign(16, bytes);
                    for (int k = 0; k < m.count; k++) {
                        const GuVertex &a = sink.out[k];
                        m.v[k] = {ToS16(a.u), ToS16(a.v), a.color, ToS16(a.x * 4096), ToS16(a.y * 4096),
                                  ToS16(a.z * 4096), 0};
                    }
                    if (dynamic) {
                        std::map<std::vector<float>, int> ids;
                        m.normalOf.resize(m.count);
                        for (int k = 0; k < m.count; k++) {
                            const GuVertex &a = sink.out[k];
                            if (!a.hasN) {
                                m.normalOf[k] = -1;
                                continue;
                            }
                            std::vector<float> n(a.n, a.n + 3);
                            auto f = ids.find(n);
                            if (f == ids.end()) {
                                f = ids.emplace(n, (int)ids.size()).first;
                                m.normals.insert(m.normals.end(), a.n, a.n + 3);
                            }
                            m.normalOf[k] = (s16)f->second;
                        }
                        g_meshBytes += m.normals.size() * sizeof(float) + m.normalOf.size() * sizeof(s16);
                    }
                    sceKernelDcacheWritebackRange(m.v, bytes);
                    g_meshBytes += bytes;
                }
                it = data->meshes.emplace(key, m).first;
            }
            const GuMesh &mesh = it->second;
            GuVertex16 *drawV = mesh.v;
            if (dynamic && draw && mesh.count && !mesh.normals.empty()) {
                // Colours of this draw: the vertices are copied into the display list and relit.
                GuVertex16 *v = (GuVertex16 *)sceGuGetMemory(sizeof(GuVertex16) * mesh.count);
                memcpy(v, mesh.v, sizeof(GuVertex16) * mesh.count);
                size_t nc = mesh.normals.size() / 3;
                u32 *rgb = (u32 *)alloca(nc * sizeof(u32));
                for (size_t i = 0; i < nc; i++) {
                    float w[3];
                    G3WorldNormal(*light, nullptr, &mesh.normals[i * 3], w);
                    rgb[i] = GuSink::Color8888(G3LightVertex(lit, *light->env, w, light->eye, light->specular), 0);
                }
                for (int k = 0; k < mesh.count; k++)
                    if (mesh.normalOf[k] >= 0)
                        v[k].color = rgb[mesh.normalOf[k]] | (v[k].color & 0xff000000);
                drawV = v;
            }
            if (mesh.count && draw) {
                if (mesh.su)
                    sceGuTexScale(mesh.su, mesh.sv);
                sceGumDrawArray(GU_TRIANGLES, GU_VTYPE, mesh.count, nullptr, drawV);
            }
            break;
        }
        case 3: {
            scale = upScale;
            ScePspFVector3 s = {upScale, upScale, upScale};
            if (draw)
                sceGumScale(&s);
            break;
        }
        case 4:
            if (scale != 1 && draw) {
                ScePspFVector3 s = {1 / scale, 1 / scale, 1 / scale};
                sceGumScale(&s);
            }
            scale = 1;
            break;
        }
    }
    if (draw) {
        sceGumPopMatrix();
        sceGuDisable(GU_CULL_FACE);
        sceGuDisable(GU_FOG);
    }
}

void G3Model::Prepare(const G3Model *batch) const
{
    g_prepareOnly = true;
    DrawOps(ops, batch);
    g_prepareOnly = false;
}

size_t G3GuTextureBytes() { return g_textureBytes; }
size_t G3GuVramBytes() { return g_vramBytes; }
int G3GuClampedVertices() { return g_clampedVertices; }
void G3GuSetVram(void *base, size_t size)
{
    g_vramBase = g_vramTop = (u8 *)base;
    g_vramSize = size;
}
size_t G3GuMeshBytes() { return g_meshBytes; }
