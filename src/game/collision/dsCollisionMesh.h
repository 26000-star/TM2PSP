// Triangle mesh collision resource, loaded from graphics/<name>.h (0xd8 bytes, constructor 0x02066078, vtable
// 0x020bdfdc; Load is slot +0x1c). One per track piece model; the collision shapes (type 2, 0x02067e20) share it
// through the resource cache and place it with their matrix.
//
// File layout (all little endian, read straight into the vectors):
//   u16 version = 0x010e, u16 nVerts, u16 nTris, s16 hasMaterials
//   nVerts x { fx32 x, y, z }
//   nTris  x { u16 a, b, c }
//   u32 nCells; when nCells != 0:
//     nCells x u16   head of each cell's triangle list (0xffff = empty); cell = x + gx*y + gx*gy*z
//     u32 gx, gy, gz; fx32 min[3]; fx32 size[3]   the grid over the mesh bounds
//     u32 nNodes; nNodes x { u16 tri, u16 next }  singly linked lists (next 0xffff ends)
//   when hasMaterials != 0: nTris x u8   surface of each triangle (0..11, 255)
// Face normals are computed at load time.
#pragma once

#include <vector>

#include "nitro.h"

struct dsCollisionGrid {
    u32 dim[3] = {};            // +0x00
    fx32 min[3] = {};           // +0x0c
    fx32 size[3] = {};          // +0x18
    fx64c cellsPerUnit[3] = {}; // +0x24: (dim << 12) / size, 32.32
    fx64c unitsPerCell[3] = {}; // +0x3c: inverse of cellsPerUnit
    fx32 cellSize[3] = {};      // +0x54: size / dim

    void Init(const u32 dim[3], const fx32 min[3], const fx32 size[3]); // 0x02073500
};

struct dsCollisionTri {
    u16 v[3];
};

struct dsCollisionNode {
    u16 tri;
    u16 next;
};

class dsCollisionMesh {
public:
    bool Load(const char *name);                       // 0x02065c04: name without "graphics/" and ".h"
    void ComputeNormals();                             // 0x02065804

    // Appends to out the triangles listed in grid cell c (x, y, z), skipping ones already in out. 0x02065b2c
    void GatherCell(const u32 c[3], std::vector<u32> &out) const;
    bool HasGrid() const { return !m_cells.empty(); }

    std::vector<VecFx32> verts;        // +0x14
    std::vector<dsCollisionTri> tris;  // +0x20
    std::vector<u8> materials;         // +0x38 (empty when the file has none)
    dsCollisionGrid grid;              // +0x44
    std::vector<dsCollisionNode> nodes; // +0xa4
    std::vector<u16> m_cells;          // +0xb0
    std::vector<VecFx32> crosses;      // +0xb8: unnormalised (b - a) x (c - a)
    std::vector<fx16> normals;         // +0xc4: 3 per triangle
    s16 hasMaterials = 0;
};
