#include "dsPieceData.h"

#include <stdio.h>

#include "math/dsMath.h"
#include "script/dsLua.h"
#include "track/dsRaceSetup.h"

// Default TriggerSize per environment: x = 0x020b8f9c[env], y = 0x020b8fbc[env], z = y / 2.
static const fx32 s_triggerX[ENV_COUNT] = {0x1c000, 0x40000, 0x1c000, 0x10000};
static const fx32 s_triggerY[ENV_COUNT] = {0x8000, 0x8000, 0x8000, 0xe000};
static const u32 MAX_PIECE_ID = 350; // 0x0209f270: ids 1..349 (the auto-numbering array has 350 entries)

static dsScriptManager s_scriptManager;

// Created on first use; the original allocates it in the race loader (0x02099e6c) after the menus have chosen the
// environment, which the constructor reads.
dsPieceDataTable *dsPieceDataTable::Get()
{
    static dsPieceDataTable s_instance;
    return &s_instance;
}

// @port main:0209f4e4 done
dsPieceDataTable::dsPieceDataTable()
{
    m_default = new dsPieceInfo;
    m_default->notchData.assign((size_t)m_default->width * m_default->height * m_default->depth, 0);
    const dsRaceSetup *setup = dsRaceSetup::Get();
    m_groundLevel = setup->environment == ENV_COAST ? 3 : 0;
    m_cellWidth = setup->CellWidthFx();
    m_cellHeight = setup->CellHeightFx();
}

dsPieceDataTable::~dsPieceDataTable()
{
    Clear();
    delete m_default;
}

// @port main:0209e5a8 done
void dsPieceDataTable::Clear()
{
    m_byName.clear();
    m_byId.clear();
    for (dsPieceInfo *p : m_all)
        delete p;
    m_all.clear();
}

// @port main:0209e674 done
void dsPieceDataTable::Load(s32 env)
{
    dsRaceSetup *setup = dsRaceSetup::Get();
    setup->SetEnvironment(env);
    // The original makes a new table for each race (its constructor reads the environment); the port keeps one, so
    // the ground level is set here: kept from the first race's environment, it left Coast without terrain after
    // another environment, and Snow and Island with fewer pillars and terrain pieces after Coast.
    m_groundLevel = env == ENV_COAST ? 3 : 0;
    m_cellWidth = setup->CellWidthFx();
    m_cellHeight = setup->CellHeightFx();
    Clear();

    char path[200];
    snprintf(path, sizeof path, "PieceData/%s/PieceData", dsRaceSetup::EnvironmentName(env));
    dsLuaStateRef state = s_scriptManager.Run(path);
    if (!state.get())
        return;
    dsLuaObject table = state->Global("PieceData");
    s32 curEnv = setup->environment;
    dsPieceInfo *byId[MAX_PIECE_ID] = {};
    std::vector<dsPieceInfo *> unnumbered;
    int count = table.Count();
    if (count == 0)
        return;
    m_all.reserve((size_t)count + 1);

    for (dsLuaObject key = table.Begin(); !key.IsNil(); key = table.Next(key)) {
        dsLuaObject e = table.Value(key);
        dsPieceInfo *p = new dsPieceInfo;
        p->type = (u8)(e.FieldNumber("Type") >> 12);
        p->costMem = (u8)(e.FieldNumber("CostMem") >> 12);
        p->costPoly = (u8)(e.FieldNumber("CostPoly") >> 12);
        p->hasAir = (e.FieldNumber("HasAir") >> 12) != 0;
        p->hasGround = (e.FieldNumber("HasGround") >> 12) != 0;
        p->width = (u8)(e.FieldNumber("Width") >> 12);
        p->height = (u8)(e.FieldNumber("Height") >> 12);
        p->depth = (u8)(e.FieldNumber("Depth") >> 12);
        if (!e.HasField("ID")) {
            unnumbered.push_back(p);
        } else {
            p->id = (u16)(e.FieldNumber("ID") >> 12);
            m_all.push_back(p);
            byId[p->id] = p;
        }
        if (e.HasField("BelowGnd")) {
            p->belowGround = (e.FieldNumber("BelowGnd") >> 12) != 0;
            p->cullable = (e.FieldNumber("Cullable") >> 12) != 0;
        }
        if (e.HasField("IsPillar")) {
            p->isPillar = (e.FieldNumber("IsPillar") >> 12) != 0;
            p->hasPillars = (e.FieldNumber("HasPillars") >> 12) != 0;
        }
        if (e.HasField("Helper"))
            p->helper = e.FieldString("Helper");
        if (e.HasField("DesignerTPV"))
            p->designerTPV = (e.FieldNumber("DesignerTPV") >> 12) != 0;
        if (e.HasField("DisableNotch"))
            p->disableNotch = (e.FieldNumber("DisableNotch") >> 12) != 0;
        if (e.HasField("NotchData")) {
            dsLuaObject n = e.Field("NotchData");
            int c = n.Count();
            p->notchData.assign((size_t)c, 1);
            for (int i = 0; i < c; i++)
                p->notchData[i] = (u8)(n.IndexNumber(i) >> 12);
            p->hasNotches = true;
        }
        if (e.HasField("IsLooseTPV"))
            p->looseTPV = (e.FieldNumber("IsLooseTPV") >> 12) != 0;
        if (e.HasField("ColumnNotches")) {
            dsLuaObject n = e.Field("ColumnNotches");
            int c = n.Count();
            p->columnNotches.assign((size_t)c + 1, 0xff);
            for (int i = 0; i < c; i++)
                p->columnNotches[i] = (u8)(n.IndexNumber(i) >> 12);
        }
        if (e.HasField("InitDir")) {
            dsLuaObject d = e.Field("InitDir");
            for (int i = 0; i < 3; i++)
                p->initDir[i] = (s16)d.IndexNumber(i);
        }
        if (e.HasField("InitPosAir")) {
            dsLuaObject d = e.Field("InitPosAir");
            p->initPosAir = {d.IndexNumber(0), d.IndexNumber(1), d.IndexNumber(2)};
        }
        if (e.HasField("InitPosGround")) {
            dsLuaObject d = e.Field("InitPosGround");
            p->initPosGround = {d.IndexNumber(0), d.IndexNumber(1), d.IndexNumber(2)};
        }
        if (!e.HasField("TriggerSize")) {
            p->triggerSize = {s_triggerX[env], s_triggerY[env], FX_Div(s_triggerY[env], 0x2000)};
        } else {
            dsLuaObject d = e.Field("TriggerSize");
            p->triggerSize = {d.IndexNumber(0), d.IndexNumber(1), d.IndexNumber(2)};
        }
        const char *name = e.FieldString("Name");
        p->name = name ? name : "";
        if (p->hasGround)
            p->ground = p->name + "ground";
        if (p->hasAir)
            p->air = p->name + "air";
        if (!p->hasAir && !p->hasGround) {
            p->ground = p->name;
            if (!p->belowGround || curEnv == ENV_ISLAND || curEnv == ENV_COAST)
                p->air = p->name;
        }
        m_byName[p->name] = p;
        if (p->id != 0)
            m_byId[p->id] = p;
    }

    // Pieces without an ID take the first free one.
    for (dsPieceInfo *p : unnumbered) {
        for (u32 id = 1; id < MAX_PIECE_ID; id++) {
            if (!byId[id]) {
                p->id = (u16)id;
                byId[id] = p;
                m_byId[id] = p;
                break;
            }
        }
        m_all.push_back(p);
    }

    // Snow: link the pillar pieces "<name>pillar1/2/4/8" of the pieces that have them. operator[] of the original
    // inserts NULL entries for missing names; the port uses find, which leaves the map unchanged.
    if (curEnv == ENV_SNOW) {
        static const int s_pillarBits[4] = {1, 2, 4, 8}; // 0x020b8fac
        for (dsPieceInfo *p : m_all) {
            if (!p->hasPillars || p->isPillar)
                continue;
            for (int k = 0; k < 4; k++) {
                char pname[200];
                snprintf(pname, sizeof pname, "%spillar%d", p->name.c_str(), s_pillarBits[k]);
                dsPieceInfo *pillar = FindByName(pname);
                if (!pillar) {
                    p->pillars[0] = 0;
                    p->hasPillars = false;
                    break;
                }
                p->pillars[k] = pillar->id;
            }
        }
    }
}

// @port main:0209f2dc done
dsPieceInfo *dsPieceDataTable::FindByName(const char *name)
{
    auto it = m_byName.find(name);
    return it == m_byName.end() ? nullptr : it->second;
}

// @port main:0209f330 done
dsPieceInfo *dsPieceDataTable::FindById(u32 id)
{
    auto it = m_byId.find(id);
    return it == m_byId.end() ? nullptr : it->second;
}

// @port main:0209f38c done (port: an unknown id returns NULL instead of inserting a NULL entry)
dsPieceInfo *dsPieceDataTable::GetById(s32 id)
{
    if (id == -1)
        return m_default;
    return FindById((u32)id);
}
