// NitroSystem G3D stand-in: BMD0 models (MDL0 + TEX0) loaded for a drawing back-end.
//
// This stands in for the DS geometry engine and NNS_G3dDraw: the model's render commands (SBC) select node
// matrices, materials and shapes; shape display lists (GXFIFO packed commands) are decoded into primitives
// (G3DecodeShape). Textures are decoded to RGBA from every DS format. Firebrand's track pieces reference the
// environment's shared texturebatch.n instead of their own materials (bit 31 of the materials offset): their
// material commands carry the batch texture index directly.
//
// Back-ends: src/shim/host/g3d_gl.cpp (OpenGL 1.x, immediate mode) and src/shim/psp/g3d_gu.cpp (sceGu; display
// lists converted to vertex arrays once). Both implement G3Model::Upload, Draw and DrawPart.
//
// Not emulated yet: the DS lighting equation, toon/highlight shading, fog, polygon IDs, edge marking,
// matrix-stack skinning (MTX_RESTORE inside display lists), texture matrix rotation.
#pragma once

#include <string>
#include <vector>

#include "nitro.h"

struct G3Texture {
    int w = 0, h = 0;
    bool repeatS = true, repeatT = true, flipS = false, flipT = false;
    bool translucent = false;      // has texels with 0 < alpha < 255
    std::vector<u8> rgba;
    unsigned gl = 0;               // host: GL texture name
    void *gu = nullptr;            // PSP: back-end texture (g3d_gu.cpp)
};

// The TEX0 block of a container: textures and palettes by name and index.
struct G3TexSet {
    struct Tex { std::string name; u32 params; int w, h, fmt; bool c0t; std::vector<u8> d1, d2; };
    struct Pal { std::string name; u32 offset; };
    std::vector<Tex> tex;
    std::vector<Pal> pal;
    std::vector<u8> palData;
    bool Parse(const std::vector<u8> &file, u32 off);
    // RGBA of texture ti with palette pi (pi < 0: none).
    G3Texture Decode(int ti, int pi) const;
    int FindTex(const std::string &n) const;
    int FindPal(const std::string &n) const;
};

struct G3Lit;

// A vertex of G3Model::DrawQuads: position in the units of the current matrix, raw DS texture coordinates.
struct G3QuadVertex {
    float pos[3];
    s16 s, t;                      // TEXCOORD values (1/16 texel)
};

// Texture SRT of a material (NNSG3dResMatData texture matrix, or a BTA0/SRT0 material animation): texture
// coordinates in texture units (1 = the texture's width / height) become s * scale - trans, the Maya convention
// the game's exporter uses (NNS_G3D texture matrix mode of the models; the sign of the translation is a port
// reading, not checked on the DS). Rotation is left out: every material of the race uses rotation zero.
struct G3TexSrt {
    bool used = false;
    float scale[2] = {1, 1}, trans[2] = {0, 0};
};

class G3Model {
public:
    G3Model() = default;
    G3Model(const G3Model &) = delete;
    G3Model &operator=(const G3Model &) = delete;
    ~G3Model();                    // frees the back-end textures and data (back-end file)
    bool Load(const std::vector<u8> &file);
    // Uses the batch textures for Firebrand batch materials (texturebatch.n of the environment).
    void Draw(const G3Model *batch) const;
    // One of the two passes (DrawOps): the DS draws every opaque polygon of the frame before any translucent one,
    // so a scene is drawn with pass 0 for all its models, then pass 1 (without depth writes) for all of them.
    void Draw(const G3Model *batch, int pass) const;
    // One shape with one material, without node matrix or scale (parts the game places itself: wheels).
    void DrawPart(int material, int shape, const G3Model *batch) const;
    void Upload();                 // creates the back-end textures (host: needs a current GL context)
    // Converts what Draw(batch) would draw ahead of time (PSP: the vertex arrays; host: nothing), so that the
    // first frame showing a model does not pay for it.
    void Prepare(const G3Model *batch) const;
    // Bounding sphere of everything Draw places (model space, node matrices and scales applied), for culling.
    void ComputeBounds();
    bool hasBounds = false;
    float center[3] = {0, 0, 0}, radius = 0;
    // Firebrand's Tracks/<env>/material.bin, loaded next to texturebatch.n (main 0x0205513c): u32 count,
    // u32 offset of the names (from +8), `count` NNS material records (u16 tag, u16 size, then
    // NNSG3dResMatData), then per material three 16-byte names: material, texture, palette. Piece models'
    // batch indices select these materials; their textures come from this model's TEX0 by name.
    bool LoadMaterialBin(const std::vector<u8> &file);
    // Materials without a texture take the texture of the same-named material of `from` (Cars/<env>/rwheel uses
    // lwheel's). Call before Upload of either model.
    void BorrowTextures(const G3Model &from);

    struct Material {
        int texture = -1;          // index in m_textures
        u32 difAmb = 0x7fff7fff, polyAttr = 0x1f00c0;
        u32 specEmi = 0;           // SPE_EMI: specular (bits 0-14), shininess table (15), emission (16-30)
        std::string name;
        int w = 0, h = 0;
        u32 texParam = 0;          // TEXIMAGE_PARAM bits 16..19: repeat S/T, flip S/T
        G3TexSrt srt;              // static texture matrix (flag bit 0)
    };
    struct Node { float m[16]; };
    struct Op { int kind, a; };    // kinds: 0 node, 1 material, 2 draw, 3 scale up, 4 scale down
    std::vector<Material> materials;
    std::vector<G3Texture> textures;
    std::vector<Node> nodes;
    std::vector<std::vector<u8>> shapes;
    std::vector<Op> ops;
    float upScale = 1, downScale = 1;
    bool batchMaterials = false;
    // PSP back-end: longest triangle edge kept, in model units after the scale ops (the GE drops triangles that
    // leave its guard band; 3 suits the track near the camera, the far sky dome needs much less splitting).
    float guardEdge = 3.0f;
    G3TexSet texSet;               // TEX0 of this container (kept for material.bin)
    mutable void *backend = nullptr; // back-end data (PSP: converted shapes)
    // Draw-time material overrides, as the game's material controllers (created by main 0x0206b0xx for the car's
    // "blight" / "rlight" / "hlight" materials; itcm 0x01fff814 draws the shapes only when the light is on,
    // 0x01fff858 sets the polygon alpha and DIF_AMB = SPE_EMI = the light colour). By material index (this
    // model's own materials, or the batch's for a model with batch materials: the start lights of a track piece),
    // empty = none. Prepare still converts hidden shapes.
    struct MatOverride {
        bool hidden = false;
        int alpha = -1;            // polygon alpha 1..31, -1 = the material's
        int color = -1;            // RGB555 for diffuse, ambient, specular and emission, -1 = the material's
    };
    mutable std::vector<MatOverride> overrides;
    // Texture SRT animation (<model>.na, BTA0 with an SRT0 block, loaded by main 0x02055f78 next to the model): per
    // animated material (index in this model's materials, or in the batch's for batch materials) the values of
    // scale S, scale T, rotation, translation S, translation T per frame (one value: constant).
    struct TexAnim {
        int material = -1;
        std::vector<float> comp[5];
    };
    std::vector<TexAnim> texAnims;
    int texAnimFrames = 0;
    // Reads a .na file; material names are looked up in `names` (the batch for batch materials). False if absent
    // or not a texture SRT animation.
    bool LoadTexAnim(const std::vector<u8> &file, const G3Model &names);
    // The texture SRT of `material` (of `src`, this model or the batch) at the current animation frame
    // (G3SetTexAnimFrame): the animation's values over the material's own.
    G3TexSrt TexSrt(const G3Model *src, int material) const;
    // A shadow volume as the DS draws Cars/<env>/shadow (itcm 0x01ffc7a4, model flag +0x86 bit 1): its shapes
    // first as shadow-mask polygons (back faces, polygon ID 0: the stencil is set where they fail the depth test),
    // then as shadow polygons (both faces, polygon ID 2) of colour `rgb555` and polygon alpha `alpha` where the
    // stencil is set and the depth test passes, each pixel once; the stencil is cleared. No depth writes.
    // Back-end file; needs a stencil buffer.
    void DrawShadowVolume(u16 rgb555, int alpha) const;
    // Quads (4 vertices each) textured with texture `texture` of this model as a custom draw sends them to the
    // geometry engine (the race's sea, itcm 0x01fff8cc): TEXIMAGE_PARAM repeat S/T, colour rgb555 modulated by
    // the texture, polygon alpha 31, both faces, fog if `fog`. A texture with partial alpha (A5I3) makes them
    // translucent: blended, without depth writes. PSP: each quad (a parallelogram) is cut into a quadtree for the
    // GE's guard band, cells split while larger than guardEdge and within 4 cell sizes of `eye` (3D), cells outside
    // the view frustum left out. Back-end file.
    void DrawQuads(int texture, const G3QuadVertex *v, int quads, u16 rgb555, bool fog, const float *eye) const;
    // The model's triangles in model space (node matrices and scales applied), 9 floats each, built once.
    const std::vector<float> &VolumeTriangles() const;
    mutable std::vector<float> volumeTris;
    // Applies overrides[material] (if any) to a material's resolved colours: returns false if it is hidden.
    bool ApplyOverride(int material, G3Lit *lit, u32 &dif, u32 &alpha) const;

private:
    // Two passes as the DS orders polygons: opaque first, then translucent (polygon alpha < 31 or a texture with
    // partial alpha), the latter without depth writes (POLYGON_ATTR bit 11 clear).
    void DrawOps(const std::vector<Op> &ops, const G3Model *batch) const;
    void DrawPass(const std::vector<Op> &ops, const G3Model *batch, int pass) const;
};

// Receives the primitives of a shape display list. Begin's mode is the DS one: 0 triangles, 1 quads,
// 2 triangle strip, 3 quad strip. Colors are RGB555, texture coordinates raw (texels x 16), positions in units.
struct G3ShapeSink {
    virtual ~G3ShapeSink() {}
    virtual void Begin(int mode) = 0;
    virtual void End() = 0;
    virtual void Color(u32 rgb555) = 0;
    virtual void Normal(float x, float y, float z) {}
    virtual void Restore(int index) {} // MTX_RESTORE: the following vertices use matrix stack entry `index`
    virtual void TexCoord(s16 s, s16 t) = 0;
    virtual void Vertex(const float *pos) = 0;
};
void G3DecodeShape(const std::vector<u8> &dl, G3ShapeSink &sink);

// ---- DS lighting: the geometry engine's lighting equation (GBATEK) with the race renderer's settings ----
// The game sets four directional lights and two colour sets (ov003 0x0211dc68 from Scripts/config/tracks), and
// at model load overrides the materials (main 0x02055970): car models ("cars" in the resource name) take set 0
// (lights 0 and 1), track and misc models set 1 (lights 2 and 3). Colours are RGB555 as on the DS.
struct G3LightEnv {
    struct Light {
        float dir[3] = {0, -1, 0}; // world-space unit vector (the direction the light travels)
        float color[3] = {1, 1, 1}; // 0..1 (5-bit / 31)
    };
    struct Set {
        u8 mask = 0;               // lights used (POLYGON_ATTR bits 0-3)
        u16 ambient = 0, diffuse = 0, specular = 0;
        bool table = false;        // specular through the shininess table (SPE_EMI bit 15)
    };
    Light lights[4];
    Set sets[2];
    u8 shininess[128] = {};
    bool fog = false;              // linear fog in view depth: density 0 at fogStart .. 1 at fogEnd
    u16 fogColor = 0;
    float fogStart = 0, fogEnd = 1;
};

// A material after the game's override rules (main 0x02055970).
struct G3Lit {
    bool lit = false;              // vertex colours come from the normals
    u8 mask = 0;
    u16 ambient = 0, diffuse = 0x7fff, specular = 0, emission = 0;
    bool table = false;
    bool fog = true;               // POLYGON_ATTR bit 15 (all but the "sky0" materials)
    int alpha = -1;                // replaced polygon alpha (0..31), -1 = the material's
};
G3Lit G3ResolveMaterial(const G3Model::Material &m, const G3LightEnv &env, int set);
// RGB555 vertex colour for the unit world-space normal n. eye: the camera's forward vector (the DS line of sight
// (0, 0, -1) in view space), used by the specular term.
u16 G3LightVertex(const G3Lit &l, const G3LightEnv &env, const float *n, const float *eye, bool specular);

// Lighting of the next draws (G3SetDrawLight(nullptr): unlit, material colours as before).
struct G3DrawLight {
    const G3LightEnv *env = nullptr;
    int set = 0;                   // 0: car, 1: track pieces
    float rot[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1}; // model -> world rotation, column-major
    float eye[3] = {0, 0, -1};
    bool specular = false;
    int variant = 0;               // PSP: vertex colours are baked per variant (piece rotation); -1 = every draw
};
void G3SetDrawLight(const G3DrawLight *l);
// Frame of the material animations for the next draws (the race advances it once per 30 Hz frame).
void G3SetTexAnimFrame(int frame);
// Detail level of the next draws. The PSP splits triangles for the GE's guard band (G3Model::guardEdge) only near
// the eye: level 1 and 2 keep edges up to G3DetailEdge(level) (8 and 24 units), for pieces at least 2.5 times that
// far from the eye (RaceSession::Cull). Each level is a separate conversion (Prepare). The host ignores it.
enum { G3_DETAIL_LEVELS = 3 };
void G3SetDrawDetail(int level);
int G3GetDrawDetail();
float G3DetailEdge(int level); // 0 for level 0 (the model's guardEdge)
const G3DrawLight *G3GetDrawLight();
// World normal of a model-space normal under a node matrix (column-major 4x4, null = identity), normalised.
void G3WorldNormal(const G3DrawLight &l, const float *node, const float *n, float *out);
