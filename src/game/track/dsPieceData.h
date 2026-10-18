// Track piece catalogue of one environment, read from Scripts/PieceData/<env>/PieceData.luac (global PieceData).
// dsPieceDataTable is the singleton *0x020d6644 (0x44 bytes, constructor 0x0209f4e4); each entry is a dsPieceInfo
// (0x58 bytes). Model and collision names are derived from Name: "<name>ground" / "<name>air", which the track
// code prefixes with "Tracks/<env>/" (Graphics/Tracks/Coa/CoastRoadStartGround.n / .h, ...).
#pragma once

#include <map>
#include <string>
#include <vector>

#include "nitro.h"

enum dsPieceType { PIECE_NORMAL = 0, PIECE_START = 1, PIECE_FINISH = 2, PIECE_CHECKPOINT = 3, PIECE_MULTILAP = 4 };

struct dsPieceInfo {
    std::string ground;            // +0x00 char*: "<name>ground" (HasGround), or the name for single-model pieces
    std::string air;               // +0x04 char*: "<name>air" (HasAir), or the name
    std::string name;              // +0x08 char*: Name
    std::string helper;            // +0x0c char*: Helper (Misc/helpers/<env>/<helper>)
    VecFx32 initPosGround = {};    // +0x10 InitPosGround
    VecFx32 initPosAir = {};       // +0x1c InitPosAir
    VecFx32 triggerSize = {};      // +0x28 TriggerSize (default per environment)
    s16 initDir[3] = {0, 0, FX32_ONE}; // +0x34 InitDir
    u16 id = 0;                    // +0x3a ID (assigned from 1 when the script has none)
    u16 pillars[4] = {};           // +0x3c ids of "<name>pillar1/2/4/8" (Snow)
    u8 width = 1, height = 1, depth = 1; // +0x46 +0x47 +0x48 size in cells
    u8 costPoly = 1, costMem = 1;  // +0x49 +0x4a editor budget
    std::vector<u8> notchData;     // +0x4c NotchData (count bytes, default 1)
    std::vector<u8> columnNotches; // +0x50 ColumnNotches (count + 1 bytes, 0xff terminated)
    u8 type = PIECE_NORMAL;        // +0x54 Type
    // +0x55 bit flags
    bool hasGround = false;        // bit 0
    bool hasAir = false;           // bit 1
    bool hasNotches = false;       // bit 2 (also +0x56 bit 2)
    bool belowGround = false;      // bit 3 BelowGnd
    bool cullable = true;          // bit 4 Cullable (only read when BelowGnd is present)
    bool isPillar = false;         // bit 5 IsPillar
    bool hasPillars = false;       // bit 6 HasPillars (only read when IsPillar is present)
    bool designerTPV = false;      // bit 7 DesignerTPV
    // +0x56
    bool looseTPV = false;         // bit 0 IsLooseTPV
    bool disableNotch = false;     // bit 1 DisableNotch
};

class dsPieceDataTable {
public:
    static dsPieceDataTable *Get();                    // *0x020d6644 (created by the race loader 0x02099e6c)

    dsPieceDataTable();                                // 0x0209f4e4
    ~dsPieceDataTable();
    void Load(s32 env);                                // 0x0209e674
    void Clear();                                      // 0x0209e5a8

    dsPieceInfo *FindByName(const char *name);         // 0x0209f2dc (NULL when absent)
    dsPieceInfo *FindById(u32 id);                     // 0x0209f330 (NULL when absent)
    dsPieceInfo *GetById(s32 id);                      // 0x0209f38c: id -1 = the default piece

    const std::vector<dsPieceInfo *> &All() const { return m_all; }
    s32 GroundLevel() const { return m_groundLevel; }

private:
    std::map<std::string, dsPieceInfo *> m_byName;     // +0x04
    std::map<u32, dsPieceInfo *> m_byId;               // +0x10
    std::vector<dsPieceInfo *> m_all;                  // +0x1c
    dsPieceInfo *m_default = nullptr;                  // +0x34
    s32 m_groundLevel = 0;                             // +0x38: 3 on Coast; above it pieces use their air model
    fx32 m_cellWidth = 0, m_cellHeight = 0;            // +0x3c +0x40
};
