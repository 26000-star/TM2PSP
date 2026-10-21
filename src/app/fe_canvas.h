// The front end's drawing layer, shared by the race drivers (host and PSP): pictures converted offline
// (tools/fe/convert.py, kept in RAM), rounded panels, the DS sprite fonts (Fonts/Cell/Font20/28/32/40_White with the
// metrics of dsFontManager) and the DS front end's sprite cells. The menus are laid out on the PSP's 480 x 272 screen;
// the canvas maps that to the driver's screen (1:1 on the PSP, scaled and letterboxed on the host's window).
// A port addition: the DS front end draws its menus on two 256 x 192 screens with tiled backgrounds.
#pragma once

#include <string>
#include <vector>

#include "g2d_cells.h"
#include "race_session.h"

// Colours: red in the low byte, alpha in the high byte (the GE's ABGR order).
inline u32 FeRgba(int r, int g, int b, int a = 255) { return (u32)r | (u32)g << 8 | (u32)b << 16 | (u32)a << 24; }

struct FeImage {
    enum Format { F5650 = 0, F4444 = 2, F8888 = 3 }; // the GE's pixel formats
    int w = 0, h = 0;          // picture size
    int stride = 0, texH = 0;  // texture size (powers of two); `stride` x (h + 1) pixels are stored
    int format = F5650;
    u8 *pixels = nullptr;      // 16-byte aligned, inside `store`
    std::vector<u8> store;
    unsigned gl = 0;           // host: GL texture name
    void *gu = nullptr;        // PSP: set once the pixels are written back from the data cache
    bool Load(const std::string &path); // a .fet file (tools/fe/convert.py)
    void MakeDisc(int size);            // a white disc with an antialiased edge (rounded corners), 8888
    size_t Bytes() const { return (size_t)stride * (h + 1) * (format == F8888 ? 4 : 2); }
};

// Driver callbacks, in screen pixels. `image` draws texels (u0, v0)-(u1, v1) of the picture in the rectangle,
// modulated by the colour; `cell` draws a cell of a bank modulated by the colour (bilinear filtering for both).
typedef void (*FeImageFn)(const FeImage &img, float x, float y, float w, float h, float u0, float v0, float u1,
                          float v1, u32 rgba, void *user);
typedef void (*FeCellFn)(const G2CellBank &bank, const G2Cell &cell, float x, float y, float w, float h, u32 rgba,
                         void *user);

class FeCanvas {
public:
    enum Font { FONT20, FONT28, FONT32, FONT40, FONT_COUNT }; // dsFontManager slots 0..3
    enum Bank { BANK_COPPERS, BANK_ENV, BANK_DIFFICULTY, BANK_COUNT }; // FE/Cell sprite banks
    static const int W = 480, H = 272;

    FeImageFn image = nullptr;
    FeCellFn cell = nullptr;
    RaceRectFn rect = nullptr;
    void *user = nullptr;
    float scale = 1, ox = 0, oy = 0; // 480 x 272 -> screen

    // Fits 480 x 272 in a w x h screen (centred, whole aspect).
    void Fit(int w, int h);

    void Rect(float x, float y, float w, float h, u32 rgba) const;
    void Image(const FeImage &img, float x, float y, float w, float h, u32 rgba = 0xffffffff) const;
    // The picture covering the rectangle (centred crop keeping its aspect).
    void ImageCover(const FeImage &img, float x, float y, float w, float h, u32 rgba = 0xffffffff) const;
    void RoundRect(float x, float y, float w, float h, float r, u32 rgba) const;
    // A rounded panel with an outline of `width` pixels (none when 0).
    void Panel(float x, float y, float w, float h, float r, u32 fill, u32 outline, float width) const;
    // Text in a DS font (Latin-1) with the top of its capitals at y; align 0 left, 1 centred, 2 right of x. `wrap`:
    // line width (0: one line). Returns the width drawn.
    float Text(int font, const char *s, float x, float y, u32 rgba, int align = 0, float wrap = 0) const;
    float TextWidth(int font, const char *s) const;
    float TextHeight(int font, const char *s, float wrap) const; // the wrapped text's height
    float CapHeight(int font) const;
    float LineHeight(int font) const;
    // Cell `index` of a sprite bank with the top-left of its bounding box at (x, y), scaled by `k`.
    void Cell(int bank, int index, float x, float y, float k = 1, u32 rgba = 0xffffffff) const;
    // Width / height of a cell's bounding box.
    float CellWidth(int bank, int index) const;
};

// The front end's resources, loaded once (fonts and banks from the game files, pictures from FeSetDataDir).
void FeSetDataDir(const std::string &dir);
const FeImage *FeImageNamed(const char *name); // "background", "solo", "stadiumtopbg"...; null when missing
const FeImage &FeDisc();
