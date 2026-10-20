// The game's sprite fonts and text layout (main 0x0207f01c, 0x02085528, 0x0207e9f8).
//
// Fonts come from Scripts/config/ui: per font {resource = "Fonts/Cell/<name>", height, spaceWidth (fx32 pixels),
// upperCaseOnly, glyphs = {{character, width}, ...}}. Glyph i is cell i of the resource's cell bank; characters are
// Latin-1 bytes (font20_White: '!'..'~' then 54 accented letters and signs, 148 cells). The font keeps per byte the
// advance (width + 1 pixel) and the cell (0xff: none). The font manager (*0x020d35cc) loads twelve slots from the
// names at 0x020bfa08: font20_White, font28_White, font32_White, font40_White, three times; its palette resource is
// Fonts/Cell/Font (each text object picks one of its palettes: 0 white, 1 dark grey).
#pragma once

#include <string>
#include <vector>

#include "nitro.h"

class dsFont {
public:
    // 0x0207f01c from the font's script table (read by 0x02085528).
    void Set(const char *resource, fx32 height, fx32 spaceWidth, bool upperCaseOnly, const u8 *widths,
             const u8 *cells);

    std::string resource;   // +0x08: cell bank, e.g. "Fonts/Cell/Font28_White"
    fx32 height = 0;        // +0x10: line height
    u8 advance[256] = {};   // +0x14: glyph width + 1, in pixels
    u8 spaceWidth = 0;      // +0x34: in pixels
    u8 cell[256] = {};      // +0x114: cell of each character, 0xff = none
};

class dsFontManager {
public:
    enum { SLOTS = 12 };
    static dsFontManager *Get(); // *0x020d35cc, loaded on first use (false fonts when config/ui is missing)
    // 0x02085528: the twelve slots from config/ui.
    bool Load();
    const dsFont *Font(int slot) const; // null when not loaded

private:
    bool m_loaded = false;
    dsFont m_fonts[SLOTS];
    bool m_valid[SLOTS] = {};
};

// One placed character of a laid-out text: position relative to the text object, cell in the font's bank.
struct dsGlyph {
    fx32 x = 0, y = 0;
    u8 cell = 0;
};

// 0x0207e9f8: lays `text` out with `font` from (x, y): `wrap` is the line width (a line breaks at the last space
// before it overflows, or before the character that overflows), `hAlign` / `vAlign` 0 left / top, 1 centred,
// 2 right / bottom. Returns the glyphs; *width and *height get the text's size.
std::vector<dsGlyph> dsTextLayout(const dsFont &font, const char *text, fx32 x, fx32 y, int hAlign, int vAlign,
                                  fx32 wrap, fx32 *width = nullptr, fx32 *height = nullptr);
