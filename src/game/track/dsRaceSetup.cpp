#include "dsRaceSetup.h"

#include "math/dsMath.h"

static const char *const s_envNames[ENV_COUNT] = {"sta", "isl", "sno", "coa"};          // 0x020c11f8
static const char *const s_todNames[3] = {"rise", "day", "set"};                       // 0x020c11ec
static const char *const s_difficultyNames[5] = {"practice", "easy", "medium", "hard", "extreme"}; // 0x020c123c
// 0x020b8fcc: grid size in cells, 3 bytes per environment
static const u8 s_gridSize[ENV_COUNT][3] = {{20, 20, 20}, {22, 20, 22}, {20, 20, 20}, {22, 20, 22}};

dsRaceSetup *dsRaceSetup::Get()
{
    static dsRaceSetup s_instance;
    return &s_instance;
}

// @port main:0209fc8c done
void dsRaceSetup::SetEnvironment(s32 env)
{
    environment = env;
    if (env == ENV_COAST) {
        cellWidth = 0x10;
        cellHeight = 4;
    } else if (env == ENV_ISLAND) {
        cellWidth = 0x40;
        cellHeight = 8;
    } else {
        cellWidth = 0x20;
        cellHeight = 8;
    }
}

// @port main:0209fcd0 done
void dsRaceSetup::GridSize(u32 *x, u32 *y, u32 *z) const
{
    *x = s_gridSize[environment][0];
    *y = s_gridSize[environment][1];
    *z = s_gridSize[environment][2];
}

// @port main:020a11f0 done
const char *dsRaceSetup::EnvironmentName() const { return s_envNames[environment]; }
// @port main:020a11b8 done
const char *dsRaceSetup::TimeOfDayName() const { return s_todNames[timeOfDay]; }
// @port main:020a11a4 done
const char *dsRaceSetup::DifficultyName() const { return s_difficultyNames[difficulty]; }
// @port main:020a1170 done  @port main:020a11cc done
const char *dsRaceSetup::EnvironmentName(s32 env) { return s_envNames[env]; }

fx32 dsRaceSetup::CellWidthFx() const { return dsRoundD(cellWidth << FX32_SHIFT); }
fx32 dsRaceSetup::CellHeightFx() const { return dsRoundD(cellHeight << FX32_SHIFT); }
