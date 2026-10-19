#include "dsTrack.h"

#include <algorithm>
#include <utility>

#include <ctype.h>
#include <stdio.h>
#include <string.h>

#include "collision/dsCollisionMesh.h"
#include "collision/dsCollisionWorld.h"
#include "math/dsMath.h"
#include "script/dsLua.h"
#include "track/dsPieceData.h"
#include "track/dsRaceSetup.h"

static dsScriptManager s_scriptManager;

// Height added to every piece: 0.35 (0x59a) + a per-environment offset (ov003 0x02134f88, all zero).
static const fx32 PIECE_Y_OFFSET = 0x59a;
static const fx32 s_envYOffset[ENV_COUNT] = {0, 0, 0, 0};
// Rotation angles of the four quarter turns (ov003 0x02134fcc): 0, pi/2, pi, 3pi/2 in fx32 radians.
static const fx32 s_rotationAngle[4] = {0, 0x1922, 0x3244, 0x4b66};
// Terrain pieces per environment (ov003 0x02135bcc, 6 names each; empty where there is no terrain).
static const char *const s_terrainNames[ENV_COUNT][6] = {
    {"", "", "", "", "", ""},
    {"beachcorner", "beachdeadend", "beachbase1", "beachbase1", "beachgrass", "beachbase5"},
    {"", "", "", "", "", ""},
    {"wcliffcorner", "wcliffdeadend02", "wcliffbase1a", "wcliffbase1a", "dcliffcrossa", "wcliffbase5"},
};

// @port main:0209fa5c done
void dsTrackInfo::SetAuthorTime(fx32 t)
{
    authorTime = t;
    if (t != 0) {
        goldTime = t + 0x2000;
        silverTime = goldTime + 0x3000;
        bronzeTime = silverTime + 0x5000;
    } else {
        goldTime = silverTime = bronzeTime = 0;
    }
}

// @port ov003:02100d7c done
dsTerrainFilter::dsTerrainFilter(s32 env)
{
    const dsRaceSetup *setup = dsRaceSetup::Get();
    m_active = env == ENV_COAST || env == ENV_ISLAND;
    m_editor = setup->mode == 6 || setup->mode == 7;
    if (!m_active)
        return;
    dsPieceDataTable *table = dsPieceDataTable::Get();
    m_groundLevel = table->GroundLevel();
    setup->GridSize(&m_grid[0], &m_grid[1], &m_grid[2]);
    m_cell[0] = setup->cellWidth;
    m_cell[1] = setup->cellHeight;
    for (int i = 0; i < 6; i++) {
        const dsPieceInfo *p = table->FindByName(s_terrainNames[env][i]);
        m_ids[i] = p ? p->id : 0;
    }
    m_ids[2] |= 0x30000;
}

// @port ov003:02100438 done
bool dsTerrainFilter::IsTerrainPiece(u32 id) const
{
    if (!m_active)
        return false;
    for (int i = 0; i < 6; i++)
        if (id == (m_ids[i] & 0xffff))
            return true;
    return false;
}

// @port ov003:0212090c partial (helper models, triggers and the start/finish arches come with the renderer)
dsTrackPiece::dsTrackPiece(s32 id_, const VecFx32 &p, u8 rot) : id(id_), rotation(rot)
{
    dsMtx43_Identity(&matrix);
    if (id != -1) {
        info = dsPieceDataTable::Get()->FindById((u32)id);
        if (info) {
            type = info->type;
            cullable = info->cullable;
        }
    }
    SetPosition(p);
}

// The cell of the position, and the model variant: above the table's ground level a piece that has an air model
// uses it. The original also (re)loads the model, its _low LOD and the collision shape here.
// @port ov003:0211fe08 partial
void dsTrackPiece::SetPosition(const VecFx32 &p)
{
    pos = p;
    const dsRaceSetup *setup = dsRaceSetup::Get();
    fx32 cw = setup->CellWidthFx(), ch = setup->CellHeightFx();
    cell[0] = (cw == 0 ? 0 : FX_Div(pos.x, cw)) >> 12;
    cell[1] = (ch == 0 ? 0 : FX_Div(pos.y, ch)) >> 12;
    cell[2] = (cw == 0 ? 0 : FX_Div(pos.z, cw)) >> 12;

    modelPath = std::string("Tracks/") + setup->EnvironmentName() + "/";
    if (id == -1) {
        modelPath += "TestArrow";
        air = false;
    } else if (info) {
        bool useAir = dsPieceDataTable::Get()->GroundLevel() < cell[1] && !info->air.empty();
        if (useAir)
            modelPath += info->air;
        else
            modelPath += info->ground.empty() ? info->air : info->ground;
        air = useAir;
    }
    UpdateTransform();
}

// Rotation about Y by a quarter turn, then the translation: a piece of W x D cells grows towards -x and +z from its
// origin cell; rotating it moves its footprint, so the origin moves by (W-1) or (D-1) cells to keep the footprint
// on the same cells.
// @port ov003:0211f088 done (with the MTX_RotY43_ part of 0x0211f2f0)
void dsTrackPiece::UpdateTransform()
{
    fx32 angle = s_rotationAngle[rotation & 3];
    MTX_RotY43_(&matrix, dsSin(angle), dsCos(angle));

    s32 w = 1, d = 1;
    if (id != -1 && info) {
        w = info->width;
        d = info->depth;
    }
    s32 cw = dsRaceSetup::Get()->cellWidth;
    fx32 x = pos.x, z = pos.z;
    switch (rotation) {
    case 1:
        x += dsRoundD(cw * -(d - 1) * 0x1000);
        break;
    case 2:
        x += dsRoundD(cw * -(w - 1) * 0x1000);
        z += dsRoundD(cw * (d - 1) * 0x1000);
        break;
    case 3:
        z += dsRoundD(cw * (w - 1) * 0x1000);
        break;
    }
    matrix.m[3][0] = x;
    matrix.m[3][1] = pos.y;
    matrix.m[3][2] = z;
}

dsTrackPiece::~dsTrackPiece()
{
    if (collision) {
        dsCollisionWorld::Get()->RemoveShape(collision);
        delete collision;
    }
}

// @port ov003:0211fe08 partial (collision part)
void dsTrackPiece::CreateCollision()
{
    if (collision || modelPath.empty())
        return;
    std::shared_ptr<dsCollisionMesh> mesh = dsCollisionWorld::Get()->LoadMesh(modelPath);
    if (!mesh)
        return;
    collision = new dsCollisionMeshShape(mesh, 0xffffffff);
    collision->SetMatrix(matrix);
    dsCollisionWorld::Get()->AddShape(collision);
}

dsTrack::~dsTrack()
{
    for (dsTrackPiece *p : pieces)
        delete p;
}

// Grid position -> world: FX_MUL with rounding, the fraction dropped, then the software-double round.
static fx32 CellToWorld(fx32 cell, fx32 v)
{
    s64 prod = (s64)cell * v + 0x800;
    s32 r = (s32)(((u32)prod >> 12 & 0xfffff000u) | (u32)((s32)(prod >> 32) << 20));
    return dsRoundD(r);
}

// @port ov003:0211b404 done (the scene node refresh at the end, 0x0208c884/0x02058740, belongs to the renderer)
bool dsTrack::LoadScene(const char *name, bool withPieces)
{
    const dsRaceSetup *setup = dsRaceSetup::Get();
    fx32 cw = setup->CellWidthFx(), ch = setup->CellHeightFx();
    dsLuaStateRef state = s_scriptManager.Run(name);
    if (!state.get())
        return false;
    dsLuaObject track = state->Global("Track");
    dsTerrainFilter *filter = nullptr;
    dsPieceDataTable *table = dsPieceDataTable::Get();
    for (dsLuaObject key = track.Begin(); !key.IsNil(); key = track.Next(key)) {
        dsLuaObject e = track.Value(key);
        const char *k = e.IndexString(0);
        if (!k)
            continue;
        if (strcmp(k, "Environment") == 0) {
            info.environment = e.IndexNumber(1) >> 12;
        } else if (strcmp(k, "TimeOfDay") == 0) {
            info.timeOfDay = e.IndexNumber(1) >> 12;
        } else if (strcmp(k, "AuthorTime") == 0) {
            info.SetAuthorTime(e.IndexNumber(1));
        } else if (strcmp(k, "AuthorName") == 0) {
            const char *s = e.IndexString(1);
            snprintf(info.authorName, 10, "%s", s ? s : "");
            info.authorName[11] = 0;
        } else if (strcmp(k, "LapCount") == 0) {
            info.lapCount = (u8)(e.IndexNumber(1) >> 12);
        } else if (withPieces) {
            if (!filter)
                filter = new dsTerrainFilter(info.environment);
            char lname[256];
            snprintf(lname, 0xff, "%s", k);
            for (char *c = lname; *c; c++) {
                if (*c == '/')
                    *c = '\\';
                *c = (char)tolower((unsigned char)*c);
            }
            dsPieceInfo *p = table->FindByName(lname);
            if (!p || filter->IsTerrainPiece(p->id))
                continue;
            u8 rot = (u8)(e.IndexNumber(1) >> 12);
            VecFx32 pos;
            pos.x = CellToWorld(cw, e.IndexNumber(2));
            pos.y = CellToWorld(ch, e.IndexNumber(3)) + PIECE_Y_OFFSET + s_envYOffset[info.environment];
            pos.z = CellToWorld(cw, e.IndexNumber(4));
            pieces.push_back(new dsTrackPiece(p->id, pos, rot));
        }
    }
    delete filter;
    return true;
}

// @port main:0208d404 partial (footprint walk instead of the grid bytes, see dsTrack.h)
int dsTrack::PieceRotationAt(const VecFx32 &p) const
{
    const dsRaceSetup *setup = dsRaceSetup::Get();
    fx32 cw = setup->cellWidth << 12, ch = setup->cellHeight << 12;
    if (cw == 0 || ch == 0 || p.x < 0 || p.y < 0 || p.z < 0)
        return -1;
    // main 0x0208cb80: x and z are shifted by half a cell first (grid +0x40c = (cellWidth >> 1) << 12, set by the
    // grid constructor main 0x0208da98), y is not.
    fx32 half = (setup->cellWidth >> 1) << 12;
    s32 c[3] = {FX_Div(p.x + half, cw) >> 12, FX_Div(p.y, ch) >> 12, FX_Div(p.z + half, cw) >> 12};
    // Footprints can overlap (a loop's 2 x 8 cells over a booster beside it), which the game's per-cell grid bytes
    // settle; without them, a piece with booster surfaces (material 7) wins, then the first one placed.
    int found = -1;
    for (const dsTrackPiece *pc : pieces) {
        if (!pc->info)
            continue;
        int w = pc->info->width, d = pc->info->depth, h = pc->info->height;
        if (pc->rotation & 1)
            std::swap(w, d);
        if (c[0] <= pc->cell[0] && pc->cell[0] - w < c[0] && pc->cell[2] <= c[2] && c[2] < pc->cell[2] + d &&
            pc->cell[1] <= c[1] && c[1] < pc->cell[1] + h) {
            const dsCollisionMesh *m = pc->collision ? pc->collision->mesh.get() : nullptr;
            if (m && std::find(m->materials.begin(), m->materials.end(), 7) != m->materials.end())
                return pc->rotation;
            if (found < 0)
                found = pc->rotation;
        }
    }
    return found;
}

// @port main:0208d3c4 partial (footprint walk instead of the grid bytes, see dsTrack.h)
dsTrackPiece *dsTrack::PieceAtCell(s32 x, s32 y, s32 z) const
{
    for (dsTrackPiece *pc : pieces) {
        int w = 1, d = 1, h = 1;
        if (pc->info) {
            w = pc->info->width;
            d = pc->info->depth;
            h = pc->info->height;
        }
        if (pc->rotation & 1)
            std::swap(w, d);
        if (x <= pc->cell[0] && pc->cell[0] - w < x && pc->cell[2] <= z && z < pc->cell[2] + d &&
            pc->cell[1] <= y && y < pc->cell[1] + h)
            return pc;
    }
    return nullptr;
}

int dsTrack::BuildTerrain()
{
    dsTerrainFilter f(info.environment);
    return f.Build(*this);
}

// 3 x 3 neighbourhoods (index (dz + 1) * 3 + dx + 1, 1 = solid piece, 0xff = any) and the terrain piece kind in
// the centre byte; tried in order, each under the four quarter turns (ov003 0x02134e8c, 0x02134e68).
static const u8 s_terrainPatterns[15][9] = {
    {0, 0, 0, 0, 0, 0, 0, 0, 0},       {0, 0, 1, 0, 0, 0, 0, 0, 0},       {255, 1, 255, 0, 1, 0, 0, 0, 0},
    {1, 0, 1, 0, 1, 0, 0, 0, 0},       {255, 1, 255, 0, 2, 1, 0, 0, 255}, {255, 1, 1, 0, 2, 0, 0, 0, 1},
    {1, 0, 1, 0, 2, 1, 0, 0, 255},     {1, 255, 255, 0, 2, 1, 0, 0, 255}, {0, 1, 0, 0, 2, 0, 0, 0, 1},
    {1, 0, 1, 1, 2, 0, 1, 0, 0},       {1, 0, 1, 0, 3, 0, 1, 0, 0},       {1, 0, 0, 0, 3, 0, 1, 0, 1},
    {255, 1, 0, 0, 2, 0, 0, 0, 255},   {0, 0, 1, 0, 5, 0, 1, 0, 0},       {255, 255, 255, 255, 4, 255, 255, 255, 255}};
static const u8 s_terrainTurns[4][9] = {
    {0, 1, 2, 3, 4, 5, 6, 7, 8}, {6, 3, 0, 7, 4, 1, 8, 5, 2}, {8, 7, 6, 5, 4, 3, 2, 1, 0}, {2, 5, 8, 1, 4, 7, 0, 3, 6}};

// A placed piece that counts as land: not terrain, not below the ground, with a ground model.
bool dsTerrainFilter::IsSolid(const dsTrackPiece *p) const
{
    return p && !p->autoTerrain && p->info && !p->info->belowGround && !p->info->ground.empty();
}

// @port ov003:02100480 done
bool dsTerrainFilter::HasSolidNeighbour(const dsTrack &t, s32 x, s32 z) const
{
    s32 x0 = x < 1 ? 0 : x - 1, z0 = z < 1 ? 0 : z - 1;
    s32 x1 = x < (s32)m_grid[0] - 1 ? x + 1 : x, z1 = z < (s32)m_grid[2] - 1 ? z + 1 : z;
    for (s32 i = x0; i <= x1; i++)
        for (s32 k = z0; k <= z1; k++)
            if (IsSolid(t.PieceAtCell(i, m_groundLevel, k)))
                return true;
    return false;
}

// @port ov003:02100998 partial (race path: the editor's piece budget checks and the removal of earlier terrain
// pieces, which do not exist after a scene load, are left out)
// @port ov003:02100750 partial (no existing terrain piece to replace in the race path)
// @port ov003:02100554 done
int dsTerrainFilter::Build(dsTrack &t) const
{
    if (!m_active)
        return 0;
    int added = 0;
    for (s32 x = 0; x < (s32)m_grid[0]; x++)
        for (s32 z = 0; z < (s32)m_grid[2]; z++) {
            dsTrackPiece *cur = t.PieceAtCell(x, m_groundLevel, z);
            if ((cur && !cur->autoTerrain) || !HasSolidNeighbour(t, x, z))
                continue;
            u8 mask[9] = {};
            int k = 0;
            for (s32 dz = -1; dz <= 1; dz++)
                for (s32 dx = -1; dx <= 1; dx++, k++) {
                    s32 i = x + dx, j = z + dz;
                    if (i >= 0 && j >= 0 && i < (s32)m_grid[0] && j < (s32)m_grid[2] &&
                        IsSolid(t.PieceAtCell(i, m_groundLevel, j)))
                        mask[k] = 1;
                }
            bool found = false;
            u32 id = 0, rot = 0;
            for (int pat = 0; !found && pat < 15; pat++) {
                const u8 *lp = s_terrainPatterns[pat];
                for (int r = 0; !found && r < 4; r++) {
                    found = true;
                    u32 v = m_ids[lp[4]];
                    id = v & 0xffff;
                    rot = ((v >> 16) & 0xff) + r;
                    for (int i = 0; i < 9; i++)
                        if (lp[i] != 0xff && mask[s_terrainTurns[r][i]] != lp[i] && i != 4) {
                            found = false;
                            break;
                        }
                }
            }
            if (!found || (cur && id == (u32)cur->id))
                continue;
            VecFx32 pos = {dsRoundD((x * m_cell[0]) << 12), 0x59a, dsRoundD((z * m_cell[0]) << 12)};
            dsTrackPiece *p = new dsTrackPiece((s32)id, pos, (u8)(rot & 3));
            p->autoTerrain = true;
            t.pieces.push_back(p);
            added++;
        }
    return added;
}
