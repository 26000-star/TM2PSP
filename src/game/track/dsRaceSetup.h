// The race configuration singleton (*0x020d6a30 in main's data): which environment, time of day, difficulty and
// track the next race loads, plus the track grid cell size that depends on the environment.
// Only the fields read by the ported code are declared; offsets are the original's.
#pragma once

#include "nitro.h"

enum dsEnvironment { ENV_STADIUM = 0, ENV_ISLAND = 1, ENV_SNOW = 2, ENV_COAST = 3, ENV_COUNT = 4 };

struct dsRaceSetup {
    static dsRaceSetup *Get();

    s32 mode = 0;          // +0x10: game mode (1 = race, 6/7 = editor/test, 8 = platform...)
    s32 environment = 0;   // +0x20
    s32 timeOfDay = 0;     // +0x24: 0 rise, 1 day, 2 set
    s32 difficulty = 0;    // +0x28: 0 practice, 1 easy, 2 medium, 3 hard, 4 extreme
    s32 track = 0;         // +0x2c: track number inside the difficulty
    s32 cellWidth = 0;     // +0xa4: horizontal cell size in whole units (16 on Coast)
    s32 cellHeight = 0;    // +0xa8: vertical cell size in whole units (4 on Coast)

    void SetEnvironment(s32 env);                      // 0x0209fc8c
    void GridSize(u32 *x, u32 *y, u32 *z) const;       // 0x0209fcd0: track grid in cells (22 x 20 x 22 on Coast)

    const char *EnvironmentName() const;               // 0x020a11f0: "sta" "isl" "sno" "coa"
    const char *EnvironmentLongName() const;           // 0x020a11dc: "stadiumGrass" "islandSea" "snowSnow" "coastSea"
    const char *TimeOfDayName() const;                 // 0x020a11b8: "rise" "day" "set"
    const char *DifficultyName() const;                // 0x020a11a4: "practice" "easy" "medium" "hard" "extreme"
    static const char *EnvironmentName(s32 env);       // 0x020a1170 (and 0x020a11cc, same table)

    // Cell sizes as fx32, the way every user computes them: d2i(i2d(n << 12) + 0.5).
    fx32 CellWidthFx() const;
    fx32 CellHeightFx() const;
};
