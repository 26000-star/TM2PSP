// NitroSystem G3D on the host: BMD0 models (MDL0 + TEX0) drawn with OpenGL 1.x.
//
// This stands in for the DS geometry engine and NNS_G3dDraw: the model's render commands (SBC) select node
// matrices, materials and shapes; shape display lists (GXFIFO packed commands) are replayed as GL primitives.
// Textures are decoded to RGBA from every DS format. Firebrand's track pieces reference the environment's shared
// texturebatch.n instead of their own materials (bit 31 of the materials offset): their material commands carry
// the batch texture index directly.
//
// Not emulated yet: the DS lighting equation (GL lights approximate it), toon/highlight shading, fog, polygon
// IDs, edge marking, matrix-stack skinning (MTX_RESTORE inside display lists), texture SRT animation.
#pragma once

#include <string>
#include <vector>

#include "nitro.h"

struct G3Texture {
    int w = 0, h = 0;
    bool repeatS = true, repeatT = true, flipS = false, flipT = false;
    bool translucent = false;      // has texels with 0 < alpha < 255
    std::vector<u8> rgba;
    unsigned gl = 0;
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
    bool Load(const std::vector<u8> &file);
    // Uses the batch textures for Firebrand batch materials (texturebatch.n of the environment).
    void Draw(const G3Model *batch) const;
    // One shape with one material, without node matrix or scale (parts the game places itself: wheels).
    void DrawPart(int material, int shape, const G3Model *batch) const;
    void Upload();                 // creates the GL textures (needs a current context)
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

private:
    void DrawOps(const std::vector<Op> &ops, const G3Model *batch) const;
    void DrawShape(const std::vector<u8> &dl, const G3Texture *t) const;
};
