// Reads from a Nitro BMD0 model what the game logic needs before the renderer exists: the model info of the first
// model (NNSG3dResMdlInfo) and the bounding box the resource manager hands out (vtable +0xf4 of *0x020c8dcc).
#pragma once

#include "nitro.h"

struct dsModelInfo {
    fx32 posScale = 0;          // NNSG3dResMdlInfo +0x08 (model +0x1c)
    fx32 invPosScale = 0;
    u16 numVertex = 0, numPolygon = 0, numTriangle = 0, numQuad = 0;
    fx16 box[6] = {};           // boxX, boxY, boxZ, boxW, boxH, boxD (model +0x2c..+0x36)
    fx32 boxPosScale = 0, boxInvPosScale = 0;

    // name as the game passes it ("Cars/coa/0/lod1"): the file is graphics/<name>.n
    bool Load(const char *name);
};

// Box of a model scaled by posScale: size (W, H, D) and its centre (x + W/2, y + H/2, z -+ D/2). main 0x0204c1a4
bool dsModel_GetBox(const char *name, VecFx32 *size, VecFx32 *centre);
