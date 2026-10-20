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
//  - +0x128 backing: translucent panels under the speed, laps and split times (BackingQuads);
//  - +0x3c messages (init 0x0210982c, show 0x02109658 / by key 0x02109778, update 0x021097d4, hide 0x02109610):
//    localised text (dsStringTable) in font slot 1 (Font28_White) centred at (128, 102), 252 pixels wide, palette
//    0, over a copy in palette 1 one pixel lower right (the vector at ov003 0x0213784c), for 120 frames:
//    "FreeWheeling" while the car is in a free-wheel zone (dsCarDynamics +0x4e8, from the HUD update) and
//    "CheckpointMissed" / "CheckpointsMissed" with the count when the finish line is crossed too early (0x02104620).
//    The third text object (+0x0c, font slot 0, palette 7) only holds "" in the solo race.
// Not ported: the bottom screen (best/target times, retries), the network/ghost modes and their message placement,
// the backing's panel +0x14 (shown by another HUD part, 0x02109b3c).
#pragma once

#include <string>
#include <vector>

#include "nitro.h"

class dsRaceProgress;

// The text banks are font slot 1's cells (Fonts/Cell/Font28_White, all 148) in the font palettes 0 and 1.
enum dsHudBank {
    HUD_BANK_SMALL, HUD_BANK_SPLIT, HUD_BANK_FONT28, HUD_BANK_TEXT, HUD_BANK_TEXT_SHADOW, HUD_BANK_COUNT
};

struct dsHudSprite {
    u8 bank = HUD_BANK_SMALL;
    bool visible = false;
    s16 cell = 0;
    fx32 x = 0, y = 0; // cell origin in DS top-screen pixels (fx32)
    s8 anchor = 0;     // port: screen edge the part follows on a screen wider than 4:3 (-1 left, 0 centre, 1 right)
};

// A quad of the HUD backing (*(hud + 0x128), ov003 0x02130e1c via itcm 0x01fffd4c): DS top-screen pixels with y
// counted from the BOTTOM edge (x / 128 - 1, (y - 96) / 95.3 in the 3D overlay); texture coordinates in texels of
// Graphics/hud/backing.n's texture "speedobg" (64 x 32, black with alpha), t1 at the top edge, t0 at the bottom.
struct dsHudQuad {
    s16 x, y, w, h;
    s16 s0, t0, s1, t1;
    s8 anchor; // port: as dsHudSprite::anchor
};

class dsHud {
public:
    // Paths of the banks under the VFS (cells, characters, palette), the number of cells the HUD uses (0: all) and
    // the palette every cell takes (-1: each OAM entry's own), for the drivers.
    static void BankFiles(int bank, const char **ncer, const char **ncgr, const char **nclr, int *cells,
                          int *palette);

    // HUD init (0x02103958) for a track with `lapCount` laps, then the race display switched on (0x02102bf4).
    void Init(int lapCount);
    // One frame (0x02102e9c) after the race update: new race events (checkpoint, lap, finish, restart), the speed
    // of the car (its |velocity|, dsCarDynamics +0x384), whether it is in a free-wheel zone (+0x4e8) and the
    // record's split times (in passing order) and finish time, or none.
    void Update(const dsRaceProgress &rp, fx32 speed, bool freeWheeling, int ticks,
                const std::vector<fx32> &recordSplits, fx32 recordTime);

    // The translucent panels drawn under the sprites (0x02130e1c): the speed's, the lap counter's while it is
    // shown, the split times' while they are shown (two lines or one). At most 4; returns the count.
    int BackingQuads(dsHudQuad *out) const;

    std::vector<dsHudSprite> sprites; // every sprite, in drawing order; draw the visible ones
    std::vector<fx32> splits;         // this run's checkpoint / lap times in passing order (for the record)
    std::string message;              // port: the message shown, "" when none (for the drivers' logs)

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
    void ShowMessage(const char *text, int frames); // 0x02109658
    void HideMessage();                         // 0x02109610

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
    // messages +0x3c: the glyph sprites follow the fixed ones (shadow copy first)
    size_t m_msgFirst = 0;
    int m_msgTimer = 0;      // +0x40
    bool m_freeWheel = false; // HUD +0x68: the free-wheel message was shown
};
