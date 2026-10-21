#include "dsEditorTrack.h"

#include <algorithm>

#include <ctype.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "math/dsMath.h"
#include "script/dsLua.h"
#include "track/dsPieceData.h"
#include "track/dsRaceSetup.h"

static dsScriptManager s_scripts;

// Most pieces a grid holds, per environment (main 0x020b8f5c, grid +0x418).
static const int s_maxPieces[ENV_COUNT] = {250, 127, 240, 127};
// Editor memory limit per environment (ov002 0x020ee804) and the polygon limit (ov002 0x020ebe90).
static const int s_memLimit[ENV_COUNT] = {612, 630, 612, 642};
static const int POLY_LIMIT = 190;
// Extra budget of a Snow piece standing on pillars, per height in cells (ov002 0x020ee814), doubled for pieces
// whose CostMem is under 6.
static const int s_pillarCost[16] = {0, 2, 2, 4, 2, 4, 4, 5, 2, 4, 4, 5, 4, 5, 4, 6};

static std::string Lower(const std::string &s)
{
    std::string r = s;
    for (char &c : r)
        c = (char)tolower((unsigned char)c);
    return r;
}

// @port ov002:020e79d0 partial + ov002:020e763c partial (the folder lists; the icons' sprites are the screen's)
bool dsBlockMenu::Load(const char *script)
{
    folders.clear();
    dsLuaStateRef st = s_scripts.Run(script);
    if (!st.get())
        return false;
    // FOLDER is 4096 and PIECE 0 in the scripts (globals of the same names).
    std::vector<std::string> todo = {"TopFolder"};
    dsPieceDataTable *table = dsPieceDataTable::Get();
    while (!todo.empty()) {
        std::string name = todo.back();
        todo.pop_back();
        if (folders.count(name))
            continue;
        dsLuaObject f = st->Global(name.c_str());
        std::vector<dsBlockMenuEntry> &list = folders[name];
        int n = f.Count();
        for (int i = 0; i < n; i++) {
            dsLuaObject e = f.Index(i);
            dsBlockMenuEntry b;
            b.folder = e.IndexNumber(0) != 0;
            const char *s = e.IndexString(1);
            b.name = s ? s : "";
            const char *icon = e.IndexString(2);
            b.icon = icon ? (int)strtol(icon, nullptr, 16) : 0;
            if (e.Count() > 3) {
                b.count = e.IndexNumber(3) >> 12;
                puzzle = true;
            }
            if (b.folder) {
                todo.push_back(b.name);
            } else if (const dsPieceInfo *p = table->FindByName(Lower(b.name).c_str())) {
                b.id = p->id;
            }
            list.push_back(b);
        }
    }
    return true;
}

const std::vector<dsBlockMenuEntry> *dsBlockMenu::Folder(const std::string &name) const
{
    auto it = folders.find(name);
    return it == folders.end() ? nullptr : &it->second;
}

dsEditorTrack::dsEditorTrack(s32 e) : env(e)
{
    dsRaceSetup *setup = dsRaceSetup::Get();
    setup->SetEnvironment(env);
    dsPieceDataTable::Get()->Load(env);
    setup->GridSize(&gridSize[0], &gridSize[1], &gridSize[2]);
    border = env == ENV_ISLAND || env == ENV_COAST;
    maxPieces = s_maxPieces[env];
    // tracks/editor_<env>: the new track's header (environment, time of day 2, lap count 1).
    std::string tpl = std::string("tracks/editor_") + dsRaceSetup::EnvironmentName(env);
    track.LoadScene(tpl.c_str(), false);
    track.info.environment = env;
    header.environment = (u8)env;
    header.timeOfDay = (u8)track.info.timeOfDay;
    header.lapCount = track.info.lapCount ? track.info.lapCount : 1;
    Index();
}

dsEditorTrack::~dsEditorTrack() {}

VecFx32 dsEditorTrack::CellPosition(s32 env, s32 x, s32 y, s32 z)
{
    (void)env;
    const dsRaceSetup *setup = dsRaceSetup::Get();
    fx32 cw = setup->CellWidthFx(), ch = setup->CellHeightFx();
    // ov003 0x0211a1f0: d2i(i2d(n * size) + 0.5) with the product rounded like FX_MUL, plus 0.35 and the
    // environment's offset (all zero) for y.
    auto mul = [](fx32 size, s32 n) {
        s64 prod = (s64)size * (n << 12) + 0x800;
        s32 r = (s32)(((u32)prod >> 12 & 0xfffff000u) | (u32)((s32)(prod >> 32) << 20));
        return dsRoundD(r);
    };
    VecFx32 p = {mul(cw, x), mul(ch, y) + 0x59a, mul(cw, z)};
    return p;
}

// @port main:0209e0a8 done (the turned notch mask) + main:0208d1d4 done (its walk)
void dsEditorTrack::Footprint(const dsPieceInfo &info, int rot, s32 x, s32 y, s32 z, bool pillars,
                              std::vector<s32> *cells) const
{
    int W = info.width, H = info.height, D = info.depth, n = W * H * D;
    std::vector<u8> src(info.notchData);
    src.resize((size_t)n, 1);
    if (info.notchData.empty())
        std::fill(src.begin(), src.end(), 1);
    std::vector<u8> mask;
    mask.reserve((size_t)n);
    if ((W == 1 && D == 1) || rot == 0) {
        mask = src;
    } else if (rot == 1) {
        for (int h = 0; h < H; h++)
            for (int w = W - 1; w >= 0; w--)
                for (int d = 0; d < D; d++)
                    mask.push_back(src[(d + D * h) * W + w]);
    } else if (rot == 2) {
        for (int h = 0; h < H; h++) {
            int top = (h + 1) * W * D;
            for (int i = 0; i < W * D; i++)
                mask.push_back(src[top - i - 1]);
        }
    } else {
        for (int h = 0; h < H; h++)
            for (int w = 0; w < W; w++)
                for (int d = D - 1; d >= 0; d--)
                    mask.push_back(src[(d + D * h) * W + w]);
    }
    int wx = W, dz = D; // columns towards -x, rows towards +z
    if (rot == 1 || rot == 3)
        std::swap(wx, dz);
    cells->clear();
    for (s32 base = y, left = pillars ? y : 0; left >= 0; base--, left--) {
        size_t i = 0;
        for (int h = 0; h < H; h++)
            for (int d = 0; d < dz; d++)
                for (int w = 1 - wx; w <= 0; w++, i++) {
                    if (i >= mask.size() || !mask[i])
                        continue;
                    s32 cx = x + w, cy = base + h, cz = z + d;
                    if (cx < 0 || cy < 0 || cz < 0 || cx >= (s32)gridSize[0] || cy >= (s32)gridSize[1] ||
                        cz >= (s32)gridSize[2])
                        continue;
                    cells->push_back((cy * (s32)gridSize[2] + cz) * (s32)gridSize[0] + cx);
                }
    }
}

// @port main:0208cf94 done
bool dsEditorTrack::InBounds(const dsPieceInfo &info, int rot, s32 x, s32 y, s32 z) const
{
    if (x < 0 || y < 0 || z < 0 || x >= (s32)gridSize[0] || y >= (s32)gridSize[1] || z >= (s32)gridSize[2])
        return false;
    s32 alongZ = info.depth, alongX = info.width;
    if (rot == 1 || rot == 3)
        std::swap(alongZ, alongX);
    if ((s32)gridSize[1] < y + info.height)
        return false;
    if (!border) {
        if (z + (alongZ - 1) >= (s32)gridSize[2])
            return false;
        if (x < alongX - 1)
            return false;
    } else {
        if (z < 1 || z + (alongZ - 1) >= (s32)gridSize[2] - 1)
            return false;
        if (x < alongX || x >= (s32)gridSize[0] - 1)
            return false;
    }
    return true;
}

void dsEditorTrack::Index()
{
    m_cells.assign((size_t)gridSize[0] * gridSize[1] * gridSize[2], 0);
    for (dsTrackPiece *p : track.pieces)
        Mark(p, true);
}

void dsEditorTrack::Mark(dsTrackPiece *p, bool on)
{
    if (!p->info)
        return;
    size_t k = std::find(track.pieces.begin(), track.pieces.end(), p) - track.pieces.begin();
    std::vector<s32> cells;
    Footprint(*p->info, p->rotation, p->cell[0], p->cell[1], p->cell[2], p->pillars[0] != nullptr, &cells);
    for (s32 c : cells) {
        if (on)
            m_cells[(size_t)c] = (u16)(k + 1);
        else if (m_cells[(size_t)c] == k + 1)
            m_cells[(size_t)c] = 0;
    }
}

dsTrackPiece *dsEditorTrack::PieceAt(s32 x, s32 y, s32 z) const
{
    if (x < 0 || y < 0 || z < 0 || x >= (s32)gridSize[0] || y >= (s32)gridSize[1] || z >= (s32)gridSize[2])
        return nullptr;
    u16 v = m_cells[((size_t)y * gridSize[2] + z) * gridSize[0] + x];
    return v && v <= track.pieces.size() ? track.pieces[v - 1] : nullptr;
}

// @port main:0208d1d4 done (as a list) + main:0208d098 done (locked)
void dsEditorTrack::Overlaps(const dsPieceInfo &info, int rot, s32 x, s32 y, s32 z,
                             std::vector<dsTrackPiece *> *out, bool *locked) const
{
    out->clear();
    *locked = false;
    // Pillars: a piece that would stand above Snow's ground keeps the column under it (piece +0x94).
    bool pillars = info.hasPillars && y > dsPieceDataTable::Get()->GroundLevel() && !info.air.empty();
    std::vector<s32> cells;
    Footprint(info, rot, x, y, z, pillars, &cells);
    for (s32 c : cells) {
        u16 v = m_cells[(size_t)c];
        if (!v)
            continue;
        dsTrackPiece *p = track.pieces[v - 1];
        if (std::find(out->begin(), out->end(), p) != out->end())
            continue;
        if (IsLocked(p))
            *locked = true;
        out->push_back(p);
    }
}

bool dsEditorTrack::IsLocked(const dsTrackPiece *p) const
{
    return std::find(m_locked.begin(), m_locked.end(), p) != m_locked.end();
}

void dsEditorTrack::SetLocked(dsTrackPiece *p, bool on)
{
    auto it = std::find(m_locked.begin(), m_locked.end(), p);
    if (on && it == m_locked.end())
        m_locked.push_back(p);
    else if (!on && it != m_locked.end())
        m_locked.erase(it);
}

dsTrackPiece *dsEditorTrack::NewPiece(u32 id, int rot, s32 x, s32 y, s32 z)
{
    return new dsTrackPiece((s32)id, CellPosition(env, x, y, z), (u8)(rot & 3));
}

int dsEditorTrack::memLimit() const { return s_memLimit[env]; }

int dsEditorTrack::CountOf(u32 id) const
{
    int n = 0;
    for (const dsTrackPiece *p : track.pieces)
        n += !p->autoTerrain && (u32)p->id == id;
    return n;
}

static int PillarCost(const dsTrackPiece *p)
{
    if (!p->pillars[0] || !p->info)
        return 0;
    int h = (int)std::min<s32>(p->cell[1], 15);
    return p->info->costMem < 6 ? s_pillarCost[h] * 2 : s_pillarCost[h];
}

// @port ov002:020ebe14 done (the pillars' share from ov002:020ebfb4)
void dsEditorTrack::AddBudget(const dsTrackPiece *p)
{
    if (!p->info)
        return;
    polyUsed += p->info->costPoly;
    if (!p->autoTerrain && CountOf((u32)p->id) == 1)
        memUsed += p->info->costMem;
    memUsed += 1;
    polyUsed += PillarCost(p);
    memUsed += PillarCost(p);
}

// @port ov002:020ebfb4 partial (the undo record is the port's Snapshot)
void dsEditorTrack::SubBudget(const dsTrackPiece *p)
{
    if (!p->info)
        return;
    polyUsed -= PillarCost(p) + p->info->costPoly;
    memUsed -= PillarCost(p) + 1;
    if (!p->autoTerrain && CountOf((u32)p->id) == 1)
        memUsed -= p->info->costMem;
}

// @port ov002:020ebe90 done
bool dsEditorTrack::WouldOverflow(u32 id) const
{
    if (puzzle)
        return false;
    const dsPieceInfo *info = dsPieceDataTable::Get()->FindById(id);
    if (!info)
        return true;
    int poly = polyUsed + info->costPoly;
    int mem = memUsed + 1 + (CountOf(id) == 0 ? info->costMem : 0);
    if ((int)track.pieces.size() >= maxPieces)
        return true;
    return poly >= POLY_LIMIT || mem >= s_memLimit[env];
}

float dsEditorTrack::Usage() const
{
    float a = (float)polyUsed / POLY_LIMIT, b = (float)memUsed / s_memLimit[env];
    float u = a > b ? a : b;
    return u < 0 ? 0 : u > 1 ? 1 : u;
}

// @port ov002:020ddf98 partial (the touch screen's move of an existing block reuses Place)
dsEditorTrack::PlaceResult dsEditorTrack::Place(u32 id, int rot, s32 x, s32 y, s32 z, dsTrackPiece **placed)
{
    const dsPieceInfo *info = dsPieceDataTable::Get()->FindById(id);
    if (!info)
        return NO_PIECE;
    if (!InBounds(*info, rot, x, y, z))
        return OUT_OF_BOUNDS;
    std::vector<dsTrackPiece *> under;
    bool locked;
    Overlaps(*info, rot, x, y, z, &under, &locked);
    if (locked)
        return LOCKED;
    // The budget once the pieces under it are gone.
    int poly = polyUsed, mem = memUsed;
    std::vector<dsTrackPiece *> keep = track.pieces;
    for (dsTrackPiece *p : under) {
        SubBudget(p);
        track.pieces.erase(std::find(track.pieces.begin(), track.pieces.end(), p));
    }
    if (WouldOverflow(id)) {
        track.pieces = keep;
        polyUsed = poly;
        memUsed = mem;
        return FULL;
    }
    for (dsTrackPiece *p : under)
        delete p;
    dsTrackPiece *p = NewPiece(id, rot, x, y, z);
    track.pieces.push_back(p);
    AddBudget(p);
    RebuildTerrain();
    if (placed)
        *placed = p;
    return PLACED;
}

dsEditorTrack::PlaceResult dsEditorTrack::CanPlace(u32 id, int rot, s32 x, s32 y, s32 z)
{
    const dsPieceInfo *info = dsPieceDataTable::Get()->FindById(id);
    if (!info)
        return NO_PIECE;
    if (!InBounds(*info, rot, x, y, z))
        return OUT_OF_BOUNDS;
    std::vector<dsTrackPiece *> under;
    bool locked;
    Overlaps(*info, rot, x, y, z, &under, &locked);
    if (locked)
        return LOCKED;
    int poly = polyUsed, mem = memUsed;
    std::vector<dsTrackPiece *> keep = track.pieces;
    for (dsTrackPiece *p : under) {
        SubBudget(p);
        track.pieces.erase(std::find(track.pieces.begin(), track.pieces.end(), p));
    }
    bool full = WouldOverflow(id);
    track.pieces = keep;
    polyUsed = poly;
    memUsed = mem;
    return full ? FULL : PLACED;
}

bool dsEditorTrack::Remove(dsTrackPiece *p)
{
    if (!p || p->autoTerrain || IsLocked(p))
        return false;
    auto it = std::find(track.pieces.begin(), track.pieces.end(), p);
    if (it == track.pieces.end())
        return false;
    SubBudget(p);
    track.pieces.erase(it);
    delete p;
    RebuildTerrain();
    return true;
}

void dsEditorTrack::Clear()
{
    std::vector<dsTrackPiece *> keep;
    for (dsTrackPiece *p : track.pieces) {
        if (IsLocked(p))
            keep.push_back(p);
        else
            delete p;
    }
    track.pieces = keep;
    polyUsed = memUsed = 0;
    for (dsTrackPiece *p : track.pieces)
        AddBudget(p);
    RebuildTerrain();
}

// @port ov003:02100b2c partial (the terrain is rebuilt whole: the old terrain pieces go, then dsTerrainFilter::Build)
void dsEditorTrack::RebuildTerrain()
{
    std::vector<dsTrackPiece *> keep;
    for (dsTrackPiece *p : track.pieces) {
        if (p->autoTerrain) {
            SubBudget(p);
            delete p;
        } else {
            keep.push_back(p);
        }
    }
    track.pieces = keep;
    size_t before = track.pieces.size();
    Index();
    track.BuildTerrain();
    for (size_t i = before; i < track.pieces.size(); i++)
        AddBudget(track.pieces[i]);
    Index();
}

static bool IsMultilap(const dsTrackPiece *p) { return p->type == PIECE_MULTILAP; } // ov003 0x0211e674
static bool IsStart(const dsTrackPiece *p) { return p->type == PIECE_START; }       // ov003 0x0211e69c
static bool IsFinish(const dsTrackPiece *p) { return p->type == PIECE_FINISH; }     // ov003 0x0211e688

dsTrackPiece *dsEditorTrack::StartPiece() const
{
    for (dsTrackPiece *p : track.pieces)
        if (IsStart(p) || IsMultilap(p))
            return p;
    return nullptr;
}

// @port ov002:020e5808 done (main 0x0208cce8 / 0x0208cca8 count start and finish blocks, a multilap counting as both)
dsEditorCheck dsEditorTrack::Validate()
{
    int starts = 0, finishes = 0;
    for (const dsTrackPiece *p : track.pieces) {
        starts += IsStart(p) || IsMultilap(p);
        finishes += IsFinish(p) || IsMultilap(p);
    }
    dsTrackPiece *start = StartPiece();
    if (!start)
        return CHECK_NO_START;
    if (starts > 1)
        return CHECK_MANY_STARTS;
    if (!IsMultilap(start)) {
        if (finishes == 0)
            return CHECK_NO_FINISH;
        header.lapCount = 1;
    } else {
        if (finishes != 1)
            return CHECK_MULTILAP_MIX;
        if (header.lapCount < 2)
            header.lapCount = 2;
    }
    track.info.lapCount = header.lapCount;
    return CHECK_OK;
}

// ---- bit-packed custom tracks ----
// Bits are taken from the low bit of each byte up, every field from its low bit (ov003 0x021195c0 / 0x02119710).
// Header (ov003 0x02119bec): 12 x (name[i] 8, author[i] 8); laps 8; challenge type 1; +0x02 8; environment 3;
// time of day 2; difficulty 4; date: year 7, month 4, day 5, hour 5, minute 6, and 6 bits the writer fills with the
// hour again (the reader puts them in the seconds); author, gold, silver and bronze times as u32 (bytes from the
// most significant); +0x50 +0x51 +0x52 8 each; piece count 7. Then per piece (ov003 0x0211a1f0): id 8 + 2 high
// bits, rotation 2, cell x, y, z 5 each. Terrain pieces are not written (the loader rebuilds them).
namespace {
struct BitWriter {
    std::vector<u8> bytes;
    u32 bit = 0;
    void Put(u32 v, int n)
    {
        for (int i = 0; i < n; i++, bit++) {
            if ((bit >> 3) >= bytes.size())
                bytes.push_back(0);
            bytes[bit >> 3] |= (u8)(((v >> i) & 1) << (bit & 7));
        }
    }
    void Put32(u32 v)
    {
        Put(v >> 24 & 0xff, 8);
        Put(v >> 16 & 0xff, 8);
        Put(v >> 8 & 0xff, 8);
        Put(v & 0xff, 8);
    }
};
struct BitReader {
    const std::vector<u8> &bytes;
    u32 bit = 0;
    bool bad = false;
    u32 Get(int n)
    {
        u32 v = 0;
        for (int i = 0; i < n; i++, bit++) {
            if ((bit >> 3) >= bytes.size()) {
                bad = true;
                return v;
            }
            v |= (u32)((bytes[bit >> 3] >> (bit & 7)) & 1) << i;
        }
        return v;
    }
    u32 Get32()
    {
        u32 a = Get(8), b = Get(8), c = Get(8), d = Get(8);
        return a << 24 | b << 16 | c << 8 | d;
    }
};

void ReadHeaderBits(BitReader &r, dsEditorTrack::Header *h)
{
    for (int i = 0; i < 12; i++) {
        h->name[i] = (char)r.Get(8);
        h->author[i] = (char)r.Get(8);
    }
    h->name[12] = h->author[12] = 0;
    h->lapCount = (u8)r.Get(8);
    h->platform = (u8)r.Get(1);
    h->unknown2 = (u8)r.Get(8);
    h->environment = (u8)r.Get(3);
    h->timeOfDay = (u8)r.Get(2);
    h->difficulty = (u8)r.Get(4);
    static const int dateBits[6] = {7, 4, 5, 5, 6, 6};
    for (int i = 0; i < 6; i++)
        h->date[i] = (u8)r.Get(dateBits[i]);
    h->authorTime = (fx32)r.Get32();
    h->goldTime = (fx32)r.Get32();
    h->silverTime = (fx32)r.Get32();
    h->bronzeTime = (fx32)r.Get32();
    for (int i = 0; i < 3; i++)
        h->extra[i] = (u8)r.Get(8);
}
} // namespace

// @port ov003:0211a7e0 done (with 0x02119710; the date's last field repeats the hour, as the DS writes it)
std::vector<u8> dsEditorTrack::SavePacked() const
{
    BitWriter w;
    for (int i = 0; i < 12; i++) {
        w.Put((u8)header.name[i], 8);
        w.Put((u8)header.author[i], 8);
    }
    w.Put(header.lapCount, 8);
    w.Put(header.platform, 1);
    w.Put(header.unknown2, 8);
    w.Put(header.environment, 3);
    w.Put(header.timeOfDay, 2);
    w.Put(header.difficulty, 4);
    w.Put(header.date[0], 7);
    w.Put(header.date[1], 4);
    w.Put(header.date[2], 5);
    w.Put(header.date[3], 5);
    w.Put(header.date[4], 6);
    w.Put(header.date[3], 6);
    w.Put32((u32)header.authorTime);
    w.Put32((u32)header.goldTime);
    w.Put32((u32)header.silverTime);
    w.Put32((u32)header.bronzeTime);
    for (int i = 0; i < 3; i++)
        w.Put(header.extra[i], 8);
    std::vector<const dsTrackPiece *> list;
    for (const dsTrackPiece *p : track.pieces)
        if (!p->autoTerrain && p->id >= 0)
            list.push_back(p);
    if (list.size() > 126)
        list.resize(126);
    w.Put((u32)list.size(), 7);
    for (const dsTrackPiece *p : list) {
        w.Put((u32)p->id & 0xff, 8);
        w.Put((u32)p->id >> 8 & 3, 2);
        w.Put(p->rotation & 3, 2);
        w.Put((u32)p->cell[0] & 31, 5);
        w.Put((u32)p->cell[1] & 31, 5);
        w.Put((u32)p->cell[2] & 31, 5);
    }
    return w.bytes;
}

bool dsEditorTrack::ReadHeader(const std::vector<u8> &data, Header *out)
{
    BitReader r{data};
    ReadHeaderBits(r, out);
    return !r.bad;
}

// @port ov003:0211a1f0 done (pieces) + ov003:02119bec done (header)
bool dsEditorTrack::LoadPacked(const std::vector<u8> &data)
{
    BitReader r{data};
    Header h;
    ReadHeaderBits(r, &h);
    u32 count = r.Get(7);
    if (r.bad || h.environment != env)
        return false;
    for (dsTrackPiece *p : track.pieces)
        delete p;
    track.pieces.clear();
    m_locked.clear();
    header = h;
    dsTerrainFilter filter(env);
    for (u32 i = 0; i < count && !r.bad; i++) {
        u32 id = r.Get(8);
        id |= r.Get(2) << 8;
        u32 rot = r.Get(2);
        s32 x = (s32)r.Get(5), y = (s32)r.Get(5), z = (s32)r.Get(5);
        if (filter.IsTerrainPiece(id) || !dsPieceDataTable::Get()->FindById(id))
            continue;
        track.pieces.push_back(NewPiece(id, (int)rot, x, y, z));
    }
    track.info.lapCount = header.lapCount;
    track.info.timeOfDay = header.timeOfDay;
    track.info.environment = env;
    track.info.SetAuthorTime(header.authorTime);
    polyUsed = memUsed = 0;
    std::vector<dsTrackPiece *> all = track.pieces;
    track.pieces.clear();
    for (dsTrackPiece *p : all) {
        track.pieces.push_back(p);
        AddBudget(p);
    }
    RebuildTerrain();
    return !r.bad;
}

// ---- free draw ----
// Road blocks per environment (ov002 0x020eff0c) and their connections (ov002 0x020ee7a0, [rotation][block]): bit 1
// +z side, 2 -x side, 4 -z side, 8 +x side. A placed block's rotation is the table's minus two (the DS creates it
// with the table's rotation, then turns it back twice with ov003 0x0211fbd8), so a neighbour's table entry at its
// own rotation gives the sides facing the new cell.
static const char *const s_roadNames[ENV_COUNT][4] = {
    {"straight", "corner", "tshaped", "cross"},
    {"highwaystraight", "highwaycorner", "highwaytshaped", "highwaycross"},
    {"woodroadstraight", "woodroadcorner", "woodroadtshaped", "woodroadx"},
    {"coastroadstraight", "coastroadcorner", "coastroadtshaped", "coastroadcross"}};
static const u8 s_roadMask[4][4] = {{5, 12, 13, 15}, {10, 6, 14, 15}, {5, 3, 7, 15}, {10, 9, 11, 15}};

// @port ov002:020e62cc done
void dsEditorTrack::FreeDrawBegin()
{
    for (int k = 0; k < 4; k++) {
        const dsPieceInfo *p = dsPieceDataTable::Get()->FindByName(s_roadNames[env][k]);
        roadIds[k] = p ? p->id : 0;
    }
    m_strokeFirst = true;
}

// @port ov002:020e5b48 done
dsTrackPiece *dsEditorTrack::FreeDrawFit(s32 x, s32 y, s32 z, u8 mask)
{
    struct {
        s32 dx, dz;
        u8 bit;
    } const sides[4] = {{0, -1, 4}, {1, 0, 8}, {0, 1, 1}, {-1, 0, 2}};
    for (const auto &sd : sides) {
        const dsTrackPiece *n = PieceAt(x + sd.dx, y, z + sd.dz);
        if (!n || n->autoTerrain)
            continue;
        for (int k = 0; k < 4; k++)
            if (roadIds[k] && (u32)n->id == roadIds[k])
                mask |= s_roadMask[n->rotation & 3][k] & sd.bit;
    }
    if (!mask)
        return nullptr;
    for (int r = 0; r < 4; r++)
        for (int k = 0; k < 4; k++)
            if (s_roadMask[r][k] == mask && roadIds[k])
                return NewPiece(roadIds[k], (r + 2) & 3, x, y, z);
    return nullptr;
}

// ov002 0x020e5990 + 0x020ec334 (Place), unless ov002 0x020e5a38 finds the same block there already (a straight
// turned half a turn, or a cross turned any way, counts as the same).
bool dsEditorTrack::FreeDrawPut(dsTrackPiece *p)
{
    const dsTrackPiece *old = PieceAt(p->cell[0], p->cell[1], p->cell[2]);
    bool same = old && old->id == p->id &&
                (old->rotation == p->rotation || (u32)p->id == roadIds[3] ||
                 ((u32)p->id == roadIds[0] && (old->rotation & 1) == (p->rotation & 1)));
    u32 id = (u32)p->id;
    int rot = p->rotation;
    s32 c[3] = {p->cell[0], p->cell[1], p->cell[2]};
    delete p;
    return !same && Place(id, rot, c[0], c[1], c[2]) == PLACED;
}

// @port ov002:020e5f58 partial (the stroke's preview piece, ov002 +0x40 / +0x44, is the editor screen's cursor)
bool dsEditorTrack::FreeDrawStep(s32 x, s32 y, s32 z)
{
    y = std::max<s32>(y, dsPieceDataTable::Get()->GroundLevel());
    if (!m_strokeFirst && x == m_strokePrev[0] && y == m_strokePrev[1] && z == m_strokePrev[2])
        return false;
    s32 px = m_strokePrev[0], py = m_strokePrev[1], pz = m_strokePrev[2];
    bool first = m_strokeFirst || py != y || (abs(px - x) + abs(pz - z)) != 1; // a jump starts a new stroke
    m_strokeFirst = false;
    m_strokePrev[0] = x;
    m_strokePrev[1] = y;
    m_strokePrev[2] = z;
    if (first) {
        // The first cell: fitted to the roads around it, else a straight across x.
        dsTrackPiece *p = FreeDrawFit(x, y, z, 0);
        m_strokeLoose = !p;
        if (!p && roadIds[0])
            p = NewPiece(roadIds[0], 1, x, y, z);
        return p && FreeDrawPut(p);
    }
    // The previous cell opens towards the new one (bit of its side), then the new cell is fitted to its
    // neighbours: with only the previous cell beside it, a straight along the stroke.
    u8 dir = x < px ? 2 : x > px ? 8 : z < pz ? 4 : 1;
    bool changed = false;
    // That first straight turns a quarter (ov003 0x0211fbd8) when the stroke leaves it along z.
    if (m_strokeLoose && (dir == 1 || dir == 4) && roadIds[0])
        changed |= FreeDrawPut(NewPiece(roadIds[0], 2, px, py, pz));
    m_strokeLoose = false;
    if (dsTrackPiece *p = FreeDrawFit(px, py, pz, dir))
        changed |= FreeDrawPut(p);
    dsTrackPiece *q = FreeDrawFit(x, y, z, 0);
    if (!q && roadIds[0])
        q = NewPiece(roadIds[0], (dir == 1 || dir == 4) ? 2 : 1, x, y, z);
    if (q)
        changed |= FreeDrawPut(q);
    return changed;
}

dsEditorTrack::Snapshot dsEditorTrack::Save() const
{
    Snapshot s;
    for (const dsTrackPiece *p : track.pieces)
        if (!p->autoTerrain)
            s.pieces.push_back({(u32)p->id, p->rotation, {p->cell[0], p->cell[1], p->cell[2]}, IsLocked(p)});
    return s;
}

void dsEditorTrack::Restore(const Snapshot &s)
{
    for (dsTrackPiece *p : track.pieces)
        delete p;
    track.pieces.clear();
    m_locked.clear();
    polyUsed = memUsed = 0;
    for (const Snapshot::P &sp : s.pieces) {
        dsTrackPiece *p = NewPiece(sp.id, sp.rot, sp.cell[0], sp.cell[1], sp.cell[2]);
        track.pieces.push_back(p);
        if (sp.locked)
            m_locked.push_back(p);
        AddBudget(p);
    }
    RebuildTerrain();
}

bool dsEditorTrack::LoadScene(const char *scene, bool lockAll)
{
    dsTrack t;
    if (!t.LoadScene(scene, true))
        return false;
    for (dsTrackPiece *p : track.pieces)
        delete p;
    track.pieces.clear();
    m_locked.clear();
    track.info = t.info;
    header.lapCount = t.info.lapCount ? t.info.lapCount : 1;
    header.timeOfDay = (u8)t.info.timeOfDay;
    polyUsed = memUsed = 0;
    for (dsTrackPiece *p : t.pieces) {
        track.pieces.push_back(p);
        if (lockAll)
            m_locked.push_back(p);
        AddBudget(p);
    }
    t.pieces.clear();
    RebuildTerrain();
    return true;
}
