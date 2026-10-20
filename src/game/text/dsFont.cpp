// Sprite fonts and text layout (see dsFont.h).
#include "text/dsFont.h"

#include <string.h>

#include "script/dsLua.h"

namespace {

// Font manager slots (main 0x020bfa08).
const char *const kSlotFonts[dsFontManager::SLOTS] = {
    "font20_White", "font28_White", "font32_White", "font40_White", "font20_White", "font28_White",
    "font32_White", "font40_White", "font20_White", "font28_White", "font32_White", "font40_White"};

// The substitute of characters without a glyph (0x0207e9f8 reads the font at +0x145 = cell table + '1').
const u8 kMissing = 0x31;

dsScriptManager s_scripts; // the manager's own instance (*0x020c8e80)

} // namespace

// @port main:0207f01c partial (upperCaseOnly: a-z only)
void dsFont::Set(const char *res, fx32 h, fx32 space, bool upperCaseOnly, const u8 *widths, const u8 *cells)
{
    resource = res ? res : "";
    height = h;
    for (int c = 0; c < 256; c++)
        advance[c] = (u8)(widths[c] + 1);
    memcpy(cell, cells, sizeof cell);
    spaceWidth = (u8)(space >> 12);
    if (upperCaseOnly) {
        // Lower-case letters take the upper-case glyphs. The game also copies a list of accented letters; no font
        // of config/ui sets upperCaseOnly, so that list is not ported.
        for (int c = 'a'; c <= 'z'; c++) {
            cell[c] = cell[c - 0x20];
            advance[c] = advance[c - 0x20];
        }
    }
}

dsFontManager *dsFontManager::Get()
{
    static dsFontManager manager;
    if (!manager.m_loaded)
        manager.Load();
    return &manager;
}

// @port main:02085528 done (the cell and palette resources stay names: the drivers load the banks)
bool dsFontManager::Load()
{
    m_loaded = true;
    dsLuaStateRef state = s_scripts.Run("config/ui");
    if (!state.get())
        return false;
    bool any = false;
    for (int slot = 0; slot < SLOTS; slot++) {
        m_valid[slot] = false;
        dsLuaObject t = state->Global(kSlotFonts[slot]);
        if (t.GetType() != dsLuaObject::TABLE)
            continue;
        const char *res = t.FieldString("resource");
        fx32 height = t.FieldNumber("height"), space = t.FieldNumber("spaceWidth");
        bool upperCaseOnly = t.FieldBool("upperCaseOnly");
        u8 widths[256], cells[256];
        memset(widths, 0, sizeof widths);
        memset(cells, 0xff, sizeof cells);
        dsLuaObject glyphs = t.Field("glyphs");
        int n = 0;
        for (dsLuaObject k = glyphs.Begin(); !k.IsNil(); k = glyphs.Next(k), n++) {
            dsLuaObject g = glyphs.Value(k);
            const char *ch = g.IndexString(0);
            u8 c = ch ? (u8)ch[0] : 0;
            widths[c] = (u8)(g.IndexNumber(1) >> 12);
            cells[c] = (u8)n;
        }
        m_fonts[slot].Set(res, height, space, upperCaseOnly, widths, cells);
        m_valid[slot] = true;
        any = true;
    }
    return any;
}

const dsFont *dsFontManager::Font(int slot) const
{
    return slot >= 0 && slot < SLOTS && m_valid[slot] ? &m_fonts[slot] : nullptr;
}

// @port main:0207e9f8 done (advances are d2i(i2d(w << 12) + 0.5) = w << 12; the glyph sprites are the drivers')
std::vector<dsGlyph> dsTextLayout(const dsFont &font, const char *text, fx32 x0, fx32 y0, int hAlign, int vAlign,
                                  fx32 wrap, fx32 *outW, fx32 *outH)
{
    const u8 *s = (const u8 *)text;
    int n = (int)strlen(text);
    std::vector<dsGlyph> g((size_t)n + 1);
    auto adv = [&](u8 c) { return (fx32)font.advance[c] << 12; };
    auto at = [&](int k) -> dsGlyph & {
        if (k >= (int)g.size())
            g.resize((size_t)k + 1);
        return g[(size_t)k];
    };
    auto shiftLine = [&](int from, int to, fx32 w) {
        fx32 d = hAlign == 1 ? -w / 2 : hAlign == 2 ? -w : 0;
        if (d)
            for (int k = from; k < to; k++)
                at(k).x += d;
    };
    const fx32 space = (fx32)font.spaceWidth << 12, right = x0 + wrap;
    fx32 x = x0, y = y0, width = 0;
    int count = 0, lineStart = 0, lineFirst = 0;
    for (int i = 0; i < n; i++) {
        u8 c = s[i];
        bool draw = true, brk = false;
        if (font.cell[c] == 0xff) {
            if (c == '\n') {
                brk = true;
                draw = false;
            } else if (c == ' ') {
                x += space;
                draw = false;
            } else {
                c = kMissing;
                draw = font.cell[kMissing] != 0xff;
            }
        }
        if (draw) {
            dsGlyph &e = at(count);
            e.x = x;
            e.y = y;
            e.cell = font.cell[c];
            x += adv(c);
        }
        int last = count, j = i;
        fx32 tail = 0;
        if (right < x) {
            // Overflow: break at the last space of the line, else before this character.
            brk = true;
            for (; lineStart <= j; j--) {
                u8 cj = s[j];
                if (cj == ' ' || cj == '\n')
                    break;
                tail += font.cell[cj] == 0xff ? adv(kMissing) : adv(cj);
            }
            if (j < lineStart) {
                tail = adv(c);
                last = count;
                j = i;
            } else {
                int k = count - (i - j);
                last = s[i] == ' ' || s[i] == '\n' ? k : k + 1;
            }
            if (s[j] == ' ') {
                do {
                    j++;
                    tail += space;
                } while (s[j] == ' ');
            }
        }
        if (brk) {
            fx32 w = (x - x0) - tail;
            if (width < w)
                width = w;
            shiftLine(lineFirst, last, w);
            lineStart = i;
            x = x0;
            y += font.height;
            lineFirst = last;
            // The rest of the line moves down (from the break to this character, one glyph slot each).
            for (; j <= i; j++, last++) {
                dsGlyph &e = at(last);
                e.x = x;
                e.y = y;
                x += adv(s[j]);
            }
        }
        if (draw)
            count++;
    }
    g.resize((size_t)count);
    fx32 w = x - x0;
    if (width < w)
        width = w;
    shiftLine(lineFirst, count, w);
    fx32 h = font.height + (y - y0);
    fx32 dy = vAlign == 1 ? -h / 2 + font.height : vAlign == 2 ? font.height - h : 0;
    if (dy)
        for (dsGlyph &e : g)
            e.y += dy;
    if (outW)
        *outW = width;
    if (outH)
        *outH = h;
    return g;
}
