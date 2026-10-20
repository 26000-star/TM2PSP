// The race's sea (Coast and Island): not a model but a custom scene node (main 0x0206e814, node type 4, draw order
// +0x08 = 4, drawn at once: +0x34 = 1) that dsRaceEnv_LoadSky (ov003 0x0211de20) creates with the texture of
// misc/<long environment name>/<same> (misc/coastSea/coastSea: the 128 x 128 A5I3 "islandsea" of
// Graphics/Misc/CoastSea/CoastSea.n; the file's one-quad model is never drawn) and a drawer object (constructor
// ov003 0x0213079c, 0x3c bytes, vtable 0x021373b8) kept in the race environment (+0x88). The node's draw (itcm
// 0x01ffcf3c) pushes the node's matrix and binds the texture (itcm 0x01ffddd4: TEXIMAGE_PARAM repeat S/T, texture
// coordinates through the identity texture matrix); the drawer (itcm 0x01fff8cc) pops that matrix, so the quads are
// in world space under the camera, and sends 5 x 5 quads straight to the geometry engine.
#pragma once

#include "nitro.h"

struct dsSeaBackdrop {
    s32 speed = 0;               // +0x14 phase step per draw: 0x11f on Island and Coast, 0 elsewhere
    s32 phase = 0;               // +0x18 texture scroll
    fx32 height = 0;             // +0x1c Coast 2.5, Island -3.96 (the car's sea levels), 0 elsewhere
    s32 scale = 1;               // +0x20 tile scale in units of 0x10002 (2 on Island and Coast)
    VecFx32 color = {FX32_ONE, FX32_ONE, FX32_ONE}; // +0x24 colour 4 of the race environment (+0x44); +0x30 unused

    // POLYGON_ATTR of the quads: no light, modulation, both faces, fog, alpha 31, polygon ID 0. The A5I3 texture
    // (texel alpha 15..27 / 31) makes the DS draw them as translucent polygons, over the opaque scene.
    static const u32 POLYGON_ATTR = 0x001f80c0;
    static const int QUADS = 25;

    // ov003 0x0213079c and the fields dsRaceEnv_LoadSky sets afterwards, for an environment (dsEnvironment) with its
    // colour 4 (Scripts/config/tracks, dsTrackLighting::colors[4]).
    void Init(s32 environment, const VecFx32 &color4);
    struct Vertex {
        float pos[3];            // world units
        s16 s, t;                // TEXCOORD (1/16 texel)
    };
    // The draw (itcm 0x01fff8cc): QUADS quads of 4 vertices in the game's order (x, then z), for a camera at height
    // eyeY (the scene camera's +0x50).
    void Quads(fx32 eyeY, Vertex *out) const;
    u16 Color() const;           // COLOR: each channel / 132 (5 bits)
    void Advance() { phase += speed; } // end of the draw, once per drawn frame
};
