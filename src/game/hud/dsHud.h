// The race HUD (*0x02137830, ov003: constructor 0x02104f1c, init 0x02103958, update 0x02102e9c), solo race mode.
//
// On the DS the HUD is a set of 2D sprites (main 0x02069584 / 0x020696b0: +0x28/+0x2c position, +0x3e cell,
// +0x10 visible, +0x44 screen) in a 256 x 192 screen. The parts the solo race shows on the top screen are ported
// with their layout and update rules; this object produces the sprites (bank, cell, DS position) and the drivers
// draw the banks' cells (g2d_cells.h) scaled to their screen:
//  - +0x58 countdown "3", "2", "1", "GO!" (HUD_Small, 0x02105ca8 / update 0x02105b78);
//  - +0x2c speedometer, km/h (HUD_Small, 0x02105fac / 0x02105de8);
//  - +0x04 lap counter "n/N" on lap tracks (HUD_Small, 0x0210941c / 0x02108d70 / 0x0210908c / 0x02109304);
//  - +0x6c / +0xc4 split time and difference to the record, 4 s at each checkpoint and at the finish
//    (SplitTimeSprites, 0x0210bf68 / 0x0210ba88 / 0x0210bd2c / 0x0210bca0 / 0x0210bcdc, solo mode 0x021048a0);
//  - +0x130 race clock (0x021057cc / 0x02105094 / 0x02105408): on the DS it is on the bottom screen in Font40;
//    port deviation: the PSP has one screen, so it is drawn on the race view, top right, in the game's small
//    clock variant (Font28, the layout of the bottom screen's best/target times).
// Not ported: the text messages (+0x3c, localised strings), the speedometer backing (Graphics/hud/backing.n, a
// textured quad behind the speed), the bottom screen (best/target times, retries), the network/ghost modes.
#pragma once

#include <vector>

#include "nitro.h"

class dsRaceProgress;

enum dsHudBank { HUD_BANK_SMALL, HUD_BANK_SPLIT, HUD_BANK_FONT28, HUD_BANK_COUNT };

struct dsHudSprite {
    u8 bank = HUD_BANK_SMALL;
    bool visible = false;
    s16 cell = 0;
    fx32 x = 0, y = 0; // cell origin in DS top-screen pixels (fx32)
    s8 anchor = 0;     // port: screen edge the part follows on a screen wider than 4:3 (-1 left, 0 centre, 1 right)
};

class dsHud {
public:
    // Paths of the banks under the VFS (cells, characters, palette) and the number of cells the HUD uses (0: all),
    // for the drivers.
    static void BankFiles(int bank, const char **ncer, const char **ncgr, const char **nclr, int *cells);

    // HUD init (0x02103958) for a track with `lapCount` laps, then the race display switched on (0x02102bf4).
    void Init(int lapCount);
    // One frame (0x02102e9c) after the race update: new race events (checkpoint, lap, finish, restart), the speed
    // of the car (its |velocity|, dsCarDynamics +0x384) and the record's split times (in passing order) and finish
    // time, or none.
    void Update(const dsRaceProgress &rp, fx32 speed, int ticks, const std::vector<fx32> &recordSplits,
                fx32 recordTime);

    std::vector<dsHudSprite> sprites; // every sprite; draw the visible ones
    std::vector<fx32> splits;         // this run's checkpoint / lap times in passing order (for the record)

private:
    struct SplitLine {     // 0x0210c1ec: 9 sprites
        int first = 0;     // index in `sprites`
        fx32 time = 0;     // +0x2c
        int digit = 0x1a;  // +0x34 cell of '0'
        int sep = 0x24;    // +0x38 cell of ':'
        int sign = 0x25;   // +0x3c cell of the sign; 0x25 ('+' white) is never shown
        bool shown = false; // +0x40
    };
    void Layout(SplitLine &l);                  // 0x0210ba88
    void Show(SplitLine &l, bool on);           // 0x0210bd2c
    void ShowSplit(fx32 time, bool hasRef, fx32 ref); // 0x021048a0
    void ClockLayout(fx32 t, bool dashes);      // 0x02105094
    void LapLayoutCurrent(int cur);             // the current-lap half of 0x02108d70 / 0x0210908c
    void LapShow(bool on);                      // 0x02109304

    int m_countdown = 0, m_speed = 0, m_laps = 0, m_clock = 0; // first sprite of each part
    // countdown +0x58
    bool m_cdActive = false;
    int m_cdState = -1, m_cdCounter = 0x3c;
    // laps +0x04
    bool m_lapsOn = false;
    int m_lapCur = -1, m_lapTotal = 1;
    fx32 m_lapEdge = 0;
    // splits
    SplitLine m_line[2];
    int m_splitTimer = 0;
    bool m_splitOn = false;
    // clock +0x130
    fx32 m_clockX[8] = {};
    int m_blink = 0;
    bool m_clockShown = true;
    size_t m_events = 0;
};
