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
// matrix-stack skinning (MTX_RESTORE inside display lists), texture SRT animation.
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

class G3Model {
public:
    G3Model() = default;
    G3Model(const G3Model &) = delete;
    G3Model &operator=(const G3Model &) = delete;
    ~G3Model();                    // frees the back-end textures and data (back-end file)
    bool Load(const std::vector<u8> &file);
    // Uses the batch textures for Firebrand batch materials (texturebatch.n of the environment).
    void Draw(const G3Model *batch) const;
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

    struct Material {
        int texture = -1;          // index in m_textures
        u32 difAmb = 0x7fff7fff, polyAttr = 0x1f00c0;
        int w = 0, h = 0;
        u32 texParam = 0;          // TEXIMAGE_PARAM bits 16..19: repeat S/T, flip S/T
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
    G3TexSet texSet;               // TEX0 of this container (kept for material.bin)
    mutable void *backend = nullptr; // back-end data (PSP: converted shapes)

private:
    void DrawOps(const std::vector<Op> &ops, const G3Model *batch) const;
};

// Receives the primitives of a shape display list. Begin's mode is the DS one: 0 triangles, 1 quads,
// 2 triangle strip, 3 quad strip. Colors are RGB555, texture coordinates raw (texels x 16), positions in units.
struct G3ShapeSink {
    virtual ~G3ShapeSink() {}
    virtual void Begin(int mode) = 0;
    virtual void End() = 0;
    virtual void Color(u32 rgb555) = 0;
    virtual void Normal(float x, float y, float z) {}
    virtual void TexCoord(s16 s, s16 t) = 0;
    virtual void Vertex(const float *pos) = 0;
};
void G3DecodeShape(const std::vector<u8> &dl, G3ShapeSink &sink);
