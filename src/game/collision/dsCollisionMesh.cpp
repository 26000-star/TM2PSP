#include "dsCollisionMesh.h"

#include <string>

#include "fs/dsFileSystem.h"
#include "math/dsMath.h"

static const u16 COLLISION_VERSION = 0x010e;   // 0x02065efc
static const fx32 CROSS_LIMIT = 0x64000;       // 0x02065b24: 100.0; larger cross products are scaled down by 2^7
static const u16 LIST_END = 0xffff;            // 0x02065c00

// @port main:02073500 done
void dsCollisionGrid::Init(const u32 d[3], const fx32 mn[3], const fx32 sz[3])
{
    for (int i = 0; i < 3; i++) {
        dim[i] = d[i];
        min[i] = mn[i];
        size[i] = sz[i];
    }
    for (int i = 0; i < 3; i++)
        cellSize[i] = FX_Div(size[i], (fx32)(dim[i] << 12));
    for (int i = 0; i < 3; i++)
        cellsPerUnit[i] = FX_DivFx64c((fx32)(dim[i] << 12), size[i]);
    // The 32.32 value back to fx32 ((u64 >> 20) truncated to 32 bits), then inverted.
    for (int i = 0; i < 3; i++)
        unitsPerCell[i] = FX_InvFx64c((fx32)(u32)((u64)cellsPerUnit[i] >> 20));
}

template <typename T> static bool ReadVec(dsFile *f, std::vector<T> &v, size_t n)
{
    v.resize(n);
    if (n == 0)
        return true;
    return f->Read(v.data(), (u32)(n * sizeof(T))) != 0;
}

// @port main:02065c04 done
// The decompiler shows the material read as nTris * 2 bytes into the +0x38 vector; the 875 files that have
// materials hold exactly nTris bytes there (end of file), so the port reads one byte per triangle.
bool dsCollisionMesh::Load(const char *name)
{
    std::string path = std::string("graphics/") + name + ".h";
    dsFile *f = dsFileSystem::Get()->Open(path.c_str());
    bool ok = false;
    struct { u16 version, nVerts, nTris; s16 hasMaterials; } h;
    if (f && f->IsOpen() && f->Read(&h, 8) != 0 && h.version == COLLISION_VERSION) {
        hasMaterials = h.hasMaterials;
        if (ReadVec(f, verts, h.nVerts) && ReadVec(f, tris, h.nTris)) {
            ComputeNormals();
            u32 nCells = 0;
            f->Read(&nCells, 4);
            if (nCells != 0) {
                m_cells.resize(nCells);
                f->Read(m_cells.data(), nCells * 2);
                u32 dim[3];
                fx32 mn[3], sz[3];
                f->Read(dim, 12);
                f->Read(mn, 12);
                f->Read(sz, 12);
                grid.Init(dim, mn, sz);
                u32 nNodes = 0;
                f->Read(&nNodes, 4);
                ReadVec(f, nodes, nNodes);
            }
            if (h.hasMaterials != 0)
                ReadVec(f, materials, h.nTris);
            ok = true;
        }
    }
    delete f;
    return ok;
}

// Per triangle: n = (b - a) x (c - a), kept raw in crosses, then normalised to fx16 (scaled down first when a
// component exceeds 100.0, so the squared length fits; a degenerate triangle gets (0, 0, 1)).
// @port main:02065804 done
void dsCollisionMesh::ComputeNormals()
{
    size_t n = tris.size();
    crosses.clear();
    crosses.reserve(n);
    normals.clear();
    normals.reserve(n * 3);
    for (size_t i = 0; i < n; i++) {
        const VecFx32 &a = verts[tris[i].v[0]];
        const VecFx32 &b = verts[tris[i].v[1]];
        const VecFx32 &c = verts[tris[i].v[2]];
        VecFx32 e1 = {b.x - a.x, b.y - a.y, b.z - a.z};
        VecFx32 e2 = {c.x - a.x, c.y - a.y, c.z - a.z};
        VecFx32 cr;
        VEC_CrossProduct(&e1, &e2, &cr);
        crosses.push_back(cr);
        if ((cr.x < 0 ? -cr.x : cr.x) > CROSS_LIMIT || (cr.y < 0 ? -cr.y : cr.y) > CROSS_LIMIT ||
            (cr.z < 0 ? -cr.z : cr.z) > CROSS_LIMIT) {
            cr.x >>= 7;
            cr.y >>= 7;
            cr.z >>= 7;
        }
        if (cr.x == 0 && cr.y == 0 && cr.z == 0)
            cr = {0, 0, FX32_ONE};
        else
            dsVec_Normalize(&cr, &cr);
        normals.push_back((fx16)cr.x);
        normals.push_back((fx16)cr.y);
        normals.push_back((fx16)cr.z);
    }
}

// @port main:02065b2c done
void dsCollisionMesh::GatherCell(const u32 c[3], std::vector<u32> &out) const
{
    if (c[0] >= grid.dim[0] || c[1] >= grid.dim[1] || c[2] >= grid.dim[2])
        return;
    u32 i = m_cells[c[0] + grid.dim[0] * c[1] + grid.dim[1] * grid.dim[0] * c[2]];
    while (i != LIST_END) {
        const dsCollisionNode &node = nodes[i];
        bool seen = false;
        for (u32 t : out)
            if (t == node.tri) {
                seen = true;
                break;
            }
        if (!seen)
            out.push_back(node.tri);
        i = node.next;
    }
}
