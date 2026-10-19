#include "dsModelInfo.h"

#include <string.h>

#include <string>
#include <vector>

#include "fs/dsFileSystem.h"

static u32 Rd32(const u8 *p) { return (u32)p[0] | (u32)p[1] << 8 | (u32)p[2] << 16 | (u32)p[3] << 24; }
static u16 Rd16(const u8 *p) { return (u16)(p[0] | p[1] << 8); }

bool dsModelInfo::Load(const char *name)
{
    std::string path = std::string("graphics/") + name + ".n";
    dsFile *f = dsFileSystem::Get()->Open(path.c_str());
    if (!f || !f->IsOpen()) {
        delete f;
        return false;
    }
    std::vector<u8> d((size_t)f->GetSize());
    bool ok = !d.empty() && f->Read(d.data(), (u32)d.size()) == (int)d.size();
    delete f;
    if (!ok || d.size() < 0x14 || memcmp(d.data(), "BMD0", 4) != 0)
        return false;
    u32 mdl = Rd32(&d[0x10]); // first block: MDL0
    if (mdl + 0x10 > d.size() || memcmp(&d[mdl], "MDL0", 4) != 0)
        return false;
    // MDL0 dictionary at +8: offset of the entry data at dict+6, the first entry's model offset at its start
    u32 entry = mdl + 8 + Rd16(&d[mdl + 8 + 6]) + 4;
    u32 m = mdl + Rd32(&d[entry]);
    if (m + 0x40 > d.size())
        return false;
    posScale = (fx32)Rd32(&d[m + 0x1c]);
    invPosScale = (fx32)Rd32(&d[m + 0x20]);
    numVertex = Rd16(&d[m + 0x24]);
    numPolygon = Rd16(&d[m + 0x26]);
    numTriangle = Rd16(&d[m + 0x28]);
    numQuad = Rd16(&d[m + 0x2a]);
    for (int i = 0; i < 6; i++)
        box[i] = (fx16)Rd16(&d[m + 0x2c + 2 * i]);
    boxPosScale = (fx32)Rd32(&d[m + 0x38]);
    boxInvPosScale = (fx32)Rd32(&d[m + 0x3c]);
    return true;
}

static fx32 Scale(fx16 v, fx32 s)
{
    return (fx32)(((s64)v * s + 0x800) >> 12);
}

// @port main:0204c1a4 done (the model comes from the file instead of the resource cache)
bool dsModel_GetBox(const char *name, VecFx32 *size, VecFx32 *centre)
{
    dsModelInfo info;
    if (!info.Load(name))
        return false;
    fx32 s = info.posScale;
    size->x = Scale(info.box[3], s);
    size->y = Scale(info.box[4], s);
    size->z = Scale(info.box[5], s);
    centre->x = Scale(info.box[0], s) + size->x / 2;
    centre->y = Scale(info.box[1], s) + size->y / 2;
    fx32 z = Scale(info.box[2], s);
    centre->z = z < 1 ? z + size->z / 2 : z - size->z / 2;
    return true;
}
