// Race HUD (see dsHud.h). Positions are the game's, in fx32 DS pixels.
#include "hud/dsHud.h"

#include <stdio.h>

#include "race/dsRaceProgress.h"
#include "text/dsFont.h"
#include "text/dsStringTable.h"

namespace {

const fx32 PX = FX32_ONE;

// Digit advances: speedometer (ov003 0x02134f14) and lap counter (0x02134f3c), in pixels. The game turns them into
// fx32 through doubles (d2i(i2d(w << 12) + 0.5)), which is exact.
const int kSpeedWidth[10] = {11, 11, 11, 11, 11, 11, 11, 11, 11, 11};
const int kLapWidth[10] = {10, 8, 11, 11, 12, 11, 11, 9, 10, 10};

const fx32 kTimeMax = 0x0176ffd7; // 99:59.99, clamp of the clock and split displays

// HUD_Small cells
const int CELL_SLASH = 0xe, CELL_ONE = 10, CELL_TWO = 0xb, CELL_THREE = 0xc, CELL_GO = 0xd;
// Font28 cells (character - 0x21)
const int FONT_ZERO = 0xf, FONT_COLON = 0x19, FONT_DASH = 0xc;

// Port placement of the clock (bottom screen on the DS, see dsHud.h): top right, right-aligned at x = 252 like
// the speedometer (70 pixels wide), baseline at y = 28 (its digits span 12..28, the first split row).
const fx32 CLOCK_X = (252 - 70) * PX, CLOCK_Y = 28 * PX;

// Messages (0x0210982c): font slot 1, centred at (128, 102) in 252 pixels; the shadow copy is offset by the vector
// at ov003 0x0213784c ((1, 1), set by __sinit 0x02135280). 0x78 frames.
const int MSG_FONT = 1, MSG_FRAMES = 0x78;
const fx32 MSG_X = 128 * PX, MSG_Y = 102 * PX, MSG_WIDTH = 252 * PX, MSG_SHADOW = 1 * PX;
const fx32 RETRY_X = 252 * PX, RETRY_Y = 46 * PX; // port: under the clock (CLOCK_Y 28)

void TimeDigits(fx32 t, int d[6])
{
    if (t < 0)
        t = 0;
    else if (t >= kTimeMax)
        t = kTimeMax;
    int sec = t >> 12;
    int cs = (int)(((s64)(t - (sec << 12)) * 100) >> 12); // (frac * 0x64000 + 0x800) >> 24
    int min = sec / 60;
    sec -= min * 60;
    d[0] = min / 10 > 9 ? 9 : min / 10;
    d[1] = min % 10;
    d[2] = sec / 10;
    d[3] = sec % 10;
    d[4] = cs / 10;
    d[5] = cs % 10;
}

} // namespace

void dsHud::BankFiles(int bank, const char **ncer, const char **ncgr, const char **nclr, int *cells, int *palette)
{
    static const char *const kFiles[HUD_BANK_COUNT][3] = {
        {"graphics/2dsprites/common/hud/cell/hud_small.ncer", "graphics/2dsprites/common/hud/cell/hud_small.ncgr",
         "graphics/2dsprites/common/hud/cell/hud_ui_palette.nclr"},
        {"graphics/2dsprites/common/hud/cell/splittimesprites.ncer",
         "graphics/2dsprites/common/hud/cell/splittimesprites.ncgr",
         "graphics/2dsprites/common/hud/cell/hud_ui_palette.nclr"},
        {"graphics/2dsprites/common/fonts/cell/font28_white.ncer",
         "graphics/2dsprites/common/fonts/cell/font28_white.ncgr",
         "graphics/2dsprites/common/hud/cell/hud_ui_palette.nclr"},
        // text: font slot 1's resource with the font manager's palette (Fonts/Cell/Font)
        {"graphics/2dsprites/common/fonts/cell/font28_white.ncer",
         "graphics/2dsprites/common/fonts/cell/font28_white.ncgr",
         "graphics/2dsprites/common/fonts/cell/font.nclr"},
        {"graphics/2dsprites/common/fonts/cell/font28_white.ncer",
         "graphics/2dsprites/common/fonts/cell/font28_white.ncgr",
         "graphics/2dsprites/common/fonts/cell/font.nclr"},
    };
    *ncer = kFiles[bank][0];
    *ncgr = kFiles[bank][1];
    *nclr = kFiles[bank][2];
    *cells = bank == HUD_BANK_FONT28 ? FONT_COLON + 1 : 0; // the clock's characters: '-', digits, ':'
    *palette = bank == HUD_BANK_TEXT ? 0 : bank == HUD_BANK_TEXT_SHADOW ? 1 : -1;
}

// @port ov003:02103958 partial (solo race parts only) / ov003:02105ca8 done / ov003:02105fac done /
// ov003:0210941c done / ov003:0210bf68 done / ov003:021057cc partial (Font28 variant, port placement) /
// ov003:02108d70 partial (the 9999-laps layout is left out) / ov003:02102bf4 partial (messages, backing)
void dsHud::Init(int lapCount)
{
    sprites.assign(1 + 3 + 5 + 18 + 8, dsHudSprite());
    int k = 0;
    // +0x58 countdown: HUD_Small at (128, 75), hidden.
    m_countdown = k;
    sprites[k++] = {HUD_BANK_SMALL, false, 0, 128 * PX, 75 * PX, 0};
    // +0x2c speedometer: 3 HUD_Small digits at (252, 179), placed by the update.
    m_speed = k;
    for (int i = 0; i < 3; i++)
        sprites[k++] = {HUD_BANK_SMALL, false, 0, 252 * PX, 179 * PX, 1};
    // +0x04 laps: 5 HUD_Small sprites at y = 179.
    m_laps = k;
    for (int i = 0; i < 5; i++)
        sprites[k++] = {HUD_BANK_SMALL, false, 0, 0, 179 * PX, -1};
    // +0x6c / +0xc4 split lines: 9 SplitTimeSprites each at x = 128 - 80 / 2, y = 12 and 31; 10 pixels per
    // character, 5 after the separators (sprites 3 and 6).
    for (int l = 0; l < 2; l++) {
        m_line[l] = SplitLine();
        m_line[l].first = k;
        fx32 x = 88 * PX;
        for (int i = 0; i < 9; i++) {
            s16 cell = (s16)(i == 3 || i == 6 ? 0x24 : 0x1a);
            sprites[k++] = {HUD_BANK_SPLIT, false, cell, x, (l ? 31 : 12) * PX, 0};
            x += (i == 3 || i == 6) ? 5 * PX : 10 * PX;
        }
    }
    // +0x130 clock: 8 Font28 sprites, 10 pixels per digit, 5 after the separators (sprites 2 and 5).
    m_clock = k;
    fx32 x = CLOCK_X;
    for (int i = 0; i < 8; i++) {
        m_clockX[i] = x;
        sprites[k++] = {HUD_BANK_FONT28, true, (s16)FONT_ZERO, x, CLOCK_Y, 1};
        x += (i == 2 || i == 5) ? 5 * PX : 10 * PX;
    }
    m_cdActive = false;
    m_cdState = -1;
    m_cdCounter = 0x3c;
    m_splitTimer = 0;
    m_splitOn = false;
    m_blink = 0;
    m_clockShown = true;
    m_events = 0;
    splits.clear();
    // +0x3c messages: no text yet.
    m_msgFirst = sprites.size();
    m_msgTimer = 0;
    m_freeWheel = false;
    message.clear();

    // Lap counter (0x02108d70, only for 2 laps or more): the total right-aligned at x = 33 (two digits: at 50),
    // '/' 6 pixels left of it, the current lap right-aligned on the '/'.
    m_lapTotal = lapCount;
    m_lapCur = -1;
    m_lapsOn = lapCount >= 2;
    dsHudSprite *s = &sprites[m_laps];
    if (m_lapsOn) {
        fx32 x4;
        if (lapCount < 10) {
            x4 = 33 * PX - kLapWidth[lapCount] * PX;
            s[4].x = x4;
            s[4].visible = false;
            s[3].cell = (s16)lapCount;
            s[3].x = x4;
        } else {
            int ones = lapCount % 10, tens = lapCount / 10 % 10;
            s[4].cell = (s16)ones;
            s[4].x = 50 * PX - kLapWidth[ones] * PX;
            x4 = s[4].x - kLapWidth[tens] * PX;
            s[3].cell = (s16)tens;
            s[3].x = x4;
        }
        m_lapEdge = x4 - 6 * PX;
        s[2].cell = CELL_SLASH;
        s[2].x = m_lapEdge;
        LapShow(true);
    }
}

// Current lap right-aligned on the '/' (0x0210908c, two-digit layout of the normal counter).
void dsHud::LapLayoutCurrent(int cur)
{
    dsHudSprite *s = &sprites[m_laps];
    if (m_lapCur < 10 && cur > 9)
        s[0].visible = true;
    if (cur < 10) {
        s[1].cell = (s16)(cur % 10);
        s[1].x = m_lapEdge - kLapWidth[cur % 10] * PX;
    } else {
        int ones = cur % 10, tens = cur / 10 % 10;
        s[1].cell = (s16)ones;
        s[1].x = m_lapEdge - kLapWidth[ones] * PX;
        s[0].cell = (s16)tens;
        s[0].x = s[1].x - kLapWidth[tens] * PX;
    }
    m_lapCur = cur;
}

// @port ov003:02109304 done
void dsHud::LapShow(bool on)
{
    dsHudSprite *s = &sprites[m_laps];
    for (int i = 0; i < 5; i++)
        s[i].visible = on;
    if (on) {
        if (m_lapTotal < 10)
            s[4].visible = false;
        if (m_lapCur < 10)
            s[0].visible = false;
    }
}

// Characters of a split line: sign, minutes (2), ':', seconds (2), ':', hundredths (2).
// @port ov003:0210ba88 done
void dsHud::Layout(SplitLine &l)
{
    int d[6];
    TimeDigits(l.time, d);
    dsHudSprite *s = &sprites[l.first];
    s[0].cell = (s16)l.sign;
    s[1].cell = (s16)(l.digit + d[0]);
    s[2].cell = (s16)(l.digit + d[1]);
    s[3].cell = (s16)l.sep;
    s[4].cell = (s16)(l.digit + d[2]);
    s[5].cell = (s16)(l.digit + d[3]);
    s[6].cell = (s16)l.sep;
    s[7].cell = (s16)(l.digit + d[4]);
    s[8].cell = (s16)(l.digit + d[5]);
    s[0].visible = l.shown && l.sign != 0x25;
}

// @port ov003:0210bd2c done (port: the white sign of the time line, never laid out, stays hidden)
void dsHud::Show(SplitLine &l, bool on)
{
    dsHudSprite *s = &sprites[l.first];
    for (int i = 0; i < 9; i++)
        s[i].visible = on && (i != 0 || l.sign != 0x25);
    l.shown = on;
}

// Split display of the solo race: the time on the first line, the difference to the record on the second, blue
// with '-' when ahead (or equal), red with '+' when behind; 120 frames.
// @port ov003:021048a0 partial (port: the record is the best run of the save file, and without a record the
// difference line stays hidden; the game compares with its record's split table, main 0x0209558c) /
// ov003:0210bca0 done / ov003:0210bcdc done / ov003:0210bc78 done
void dsHud::ShowSplit(fx32 time, bool hasRef, fx32 ref)
{
    fx32 diff = time - (hasRef ? ref : 0);
    SplitLine &l2 = m_line[1];
    if (diff < 1) {
        diff = -diff;
        l2.digit = 0, l2.sep = 10, l2.sign = 0xc;    // blue, '-'
    } else {
        l2.digit = 0xd, l2.sep = 0x17, l2.sign = 0x18; // red, '+'
    }
    Layout(l2);
    m_line[0].time = time;
    Layout(m_line[0]);
    l2.time = diff;
    Layout(l2);
    Show(m_line[0], true);
    Show(l2, hasRef);
    m_splitTimer = 0x78;
    m_splitOn = true;
}

// Clock characters: M M : S S : c c from base cell '0', the digit '1' moved 2 pixels right (narrow glyph);
// dashes before the start (countdown mode +0x8d).
// @port ov003:02105094 done
void dsHud::ClockLayout(fx32 t, bool dashes)
{
    dsHudSprite *s = &sprites[m_clock];
    if (!dashes) {
        int d[6];
        TimeDigits(t, d);
        const int cells[8] = {FONT_ZERO + d[0], FONT_ZERO + d[1], FONT_COLON, FONT_ZERO + d[2],
                              FONT_ZERO + d[3], FONT_COLON, FONT_ZERO + d[4], FONT_ZERO + d[5]};
        for (int i = 0; i < 8; i++)
            s[i].cell = (s16)cells[i];
    } else {
        for (int i = 0; i < 8; i++)
            s[i].cell = (s16)(i == 2 || i == 5 ? FONT_COLON : FONT_DASH);
    }
    for (int i = 0; i < 8; i++)
        s[i].x = m_clockX[i] + (s[i].cell == FONT_ZERO + 1 ? 0x2000 : 0); // small font: 2 pixels
}

// @port ov003:02102e9c partial (solo race parts) / ov003:02105b78 done / ov003:02105de8 done /
// ov003:0210908c partial (normal counter) / ov003:02105408 done (display) / ov003:02104a6c partial (restart:
// splits hidden, countdown on) / ov003:02104670 partial (finish: split against the record time, clock blinks) /
// ov003:02104620 done (as the MISSED event)
void dsHud::Update(const dsRaceProgress &rp, fx32 speed, bool freeWheeling, int ticks,
                   const std::vector<fx32> &recordSplits, fx32 recordTime)
{
    if (sprites.empty())
        return;
    // Race events since the last frame: what dsRace / the race mode tell the HUD.
    bool go = false;
    for (; m_events < rp.events.size(); m_events++) {
        const dsRaceEvent &ev = rp.events[m_events];
        if (ev.type == dsRaceEvent::RESTART) {
            Show(m_line[0], false);
            Show(m_line[1], false);
            m_splitTimer = 0;
            m_splitOn = false;
            m_blink = 0;
            m_clockShown = true;
            for (int i = 0; i < 8; i++)
                sprites[m_clock + i].visible = true;
            m_cdState = -1;
            m_cdCounter = 0x3c;
            splits.clear();
            HideMessage(); // the HUD's display switch (0x02102a60 / 0x02102034) hides it around a restart
        } else if (ev.type == dsRaceEvent::CHECKPOINT) {
            size_t k = splits.size();
            splits.push_back(ev.time);
            ShowSplit(ev.time, k < recordSplits.size(), k < recordSplits.size() ? recordSplits[k] : 0);
        } else if (ev.type == dsRaceEvent::GO) {
            go = true;
        } else if (ev.type == dsRaceEvent::FINISH) {
            ShowSplit(rp.finishTime, recordTime > 0, recordTime);
            m_blink = 1000; // 0x0210538c(clock, 1000)
        } else if (ev.type == dsRaceEvent::MISSED) {
            // 0x02104620: "You missed %d checkpoint(s)."
            const char *fmt = dsStringTable::Get()->Text(ev.value == 1 ? "CheckpointMissed" : "CheckpointsMissed");
            char text[200];
            snprintf(text, sizeof text, fmt, ev.value);
            ShowMessage(text, MSG_FRAMES);
        }
    }
    // Free-wheel zone: the message once on entry, hidden on exit.
    if (!freeWheeling) {
        if (m_freeWheel) {
            HideMessage();
            m_freeWheel = false;
        }
    } else if (!m_freeWheel) {
        ShowMessage(dsStringTable::Get()->Text("FreeWheeling"), MSG_FRAMES);
        m_freeWheel = true;
    }

    // Speedometer: |velocity| x 3.6, right-aligned from x = 252, leading zeros hidden.
    {
        int kmh = (int)(((s64)speed * 0x399a + 0x800) >> 12) >> 12;
        int h = kmh / 100, rem = kmh - h * 100, t = rem / 10, o = rem - t * 10;
        int d[3] = {h, t, o};
        fx32 x = 252 * PX;
        for (int i = 2; i >= 0; i--) {
            dsHudSprite &s = sprites[m_speed + i];
            if ((i == 0 && h == 0) || (i == 1 && t == 0 && h == 0)) {
                s.visible = false;
                continue;
            }
            int v = d[i] > 9 ? 9 : d[i] < 0 ? 0 : d[i];
            s.visible = true;
            s.cell = (s16)v;
            x -= kSpeedWidth[v] * PX;
            s.x = x;
            s.y = 179 * PX;
        }
    }
    // Lap counter: current lap = laps - laps left + 1, at most the lap count.
    if (m_lapsOn) {
        int cur = m_lapTotal - rp.lapsLeft + 1;
        if (cur > m_lapTotal)
            cur = m_lapTotal;
        if (cur != m_lapCur)
            LapLayoutCurrent(cur);
    }
    // Message time-out (0x021097d4).
    if (m_msgTimer > 0 && --m_msgTimer == 0)
        HideMessage();
    // Clock: blinking for 1000 frames after the finish (every 10 frames), then the digits.
    if (m_blink) {
        m_blink--;
        for (int i = 0; i < 8; i++) {
            dsHudSprite &s = sprites[m_clock + i];
            if (m_blink == 0)
                s.visible = m_clockShown;
            else if (m_clockShown && m_blink % 10 == 0)
                s.visible = !s.visible;
        }
    }
    ClockLayout(rp.clock, rp.countdown);
    // Countdown: "3" above 2 s, "2" above 1 s, "1" above 0, then "GO!" for 60 ticks.
    {
        dsHudSprite &s = sprites[m_countdown];
        if (m_cdState == 0) {
            m_cdCounter -= ticks;
            if (m_cdCounter < 1) {
                m_cdState = -1;
                m_cdActive = false;
                s.visible = false;
            }
        }
        // The game's HUD still sees the clock's countdown mode in the frame the race mode says "go" (clock 0).
        if (rp.countdown || go) {
            m_cdActive = true;
            s.visible = true;
            fx32 c = rp.clock;
            if (c > 0x2000) {
                m_cdState = 3;
                s.cell = CELL_THREE;
            } else if (c > 0x1000) {
                m_cdState = 2;
                s.cell = CELL_TWO;
            } else if (c > 0) {
                m_cdState = 1;
                s.cell = CELL_ONE;
            } else {
                m_cdState = 0;
                m_cdCounter = 0x3c;
                s.cell = CELL_GO;
            }
        }
    }
    // Split display time-out.
    if (m_splitOn && --m_splitTimer < 1) {
        Show(m_line[0], false);
        Show(m_line[1], false);
        m_splitOn = false;
    }
}

// Both text objects take the text (laid out by 0x0207e9f8 with their alignment and width) and are shown; the
// glyphs become sprites, the shadow copy first so that the white text covers it.
// @port ov003:02109658 partial (the network modes' placement is left out) / ov003:02109778 done
void dsHud::ShowMessage(const char *text, int frames)
{
    sprites.resize(m_msgFirst);
    const dsFont *font = dsFontManager::Get()->Font(MSG_FONT);
    if (font) {
        std::vector<dsGlyph> g = dsTextLayout(*font, text, 0, 0, 1, 1, MSG_WIDTH);
        for (int pass = 0; pass < 2; pass++) {
            fx32 ox = MSG_X + (pass ? 0 : MSG_SHADOW), oy = MSG_Y + (pass ? 0 : MSG_SHADOW);
            u8 bank = pass ? HUD_BANK_TEXT : HUD_BANK_TEXT_SHADOW;
            for (const dsGlyph &e : g)
                sprites.push_back({bank, true, (s16)e.cell, ox + e.x, oy + e.y, 0});
        }
    }
    m_msgTimer = frames;
    message = text;
}

// @port ov003:0210b53c partial (the "Retries" line of the bottom-screen panel, on the race view)
void dsHud::SetRetries(int retries)
{
    if (retries == m_retries)
        return;
    m_retries = retries;
    retrySprites.clear();
    const dsFont *font = dsFontManager::Get()->Font(MSG_FONT);
    if (retries < 0 || !font)
        return;
    char text[64];
    snprintf(text, sizeof text, "%s %d", dsStringTable::Get()->Text("Retries"), retries);
    std::vector<dsGlyph> g = dsTextLayout(*font, text, 0, 0, 2, 0, 252 * PX);
    for (int pass = 0; pass < 2; pass++) {
        fx32 ox = RETRY_X + (pass ? 0 : MSG_SHADOW), oy = RETRY_Y + (pass ? 0 : MSG_SHADOW);
        u8 bank = pass ? HUD_BANK_TEXT : HUD_BANK_TEXT_SHADOW;
        for (const dsGlyph &e : g)
            retrySprites.push_back({bank, true, (s16)e.cell, ox + e.x, oy + e.y, 1});
    }
}

// @port ov003:02109610 done / ov003:021097d4 done (in Update)
void dsHud::HideMessage()
{
    sprites.resize(m_msgFirst);
    m_msgTimer = 0;
    message.clear();
}

// The backing object (ctor 0x02131068) keeps +0x14 message panel, +0x15 laps (the counter's flag +0x28), +0x16
// splits shown (+0x124), +0x17 one split line only, +0x18 the lap panel's x (0x02130e04: -21 below 10 laps, else
// -6). Its draw sends four-vertex quads through 0x01fffd4c with POLYGON_ATTR 0x021f00c0 and colour 0x3def.
// @port ov003:02130e1c partial (the message panel +0x14 is left out with the messages) / ov003:02130e04 done
int dsHud::BackingQuads(dsHudQuad *q) const
{
    int n = 0;
    q[n++] = {0xc5, 6, 0x40, 0x20, 0, 0, 64, 32, 1};
    if (m_lapsOn)
        q[n++] = {(s16)(m_lapTotal < 10 ? -0x15 : -6), 6, 0x4f, 0x20, 64, 0, 0, 32, -1};
    // +0x17 is set in the solo mode only for mode 6 (0x021048a0); the port hides the difference line without a
    // record (see ShowSplit), so one line then.
    if (m_splitOn) {
        if (m_line[1].shown)
            q[n++] = {0x30, 0x84, 0x84, 0x3c, 0, 0, 64, 32, 0};
        else
            q[n++] = {0x30, 0x9a, 0x84, 0x23, 0, 0, 64, 32, 0};
    }
    return n;
}
