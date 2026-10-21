// The front end's drawing layer (see fe_canvas.h).
#include "fe_canvas.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include <map>
#include <memory>

#include "text/dsFont.h"

namespace {

std::string g_dataDir = "build/fe/data";

// A DS sprite font drawn in one colour: the font's cells with every opaque pixel made white, so that the driver's
// colour modulation gives the text colour (the DS picks palette 0 white or 1 dark grey; the menus here use more
// colours).
struct FontBank {
    G2CellBank bank;
    const dsFont *font = nullptr;
    float top = 0, cap = 0; // top of the capitals from the glyph origin, and their height
    bool ok = false;
};

FontBank *Fonts()
{
    static FontBank fonts[FeCanvas::FONT_COUNT];
    static bool loaded = false;
    if (loaded)
        return fonts;
    loaded = true;
    static const char *const kNames[FeCanvas::FONT_COUNT] = {"font20_white", "font28_white", "font32_white",
                                                               "font40_white"};
    for (int i = 0; i < FeCanvas::FONT_COUNT; i++) {
        FontBank &f = fonts[i];
        f.font = dsFontManager::Get()->Font(i);
        std::string base = std::string("graphics/2dsprites/common/fonts/cell/") + kNames[i];
        std::vector<u8> e, g, c;
        if (!f.font || !RaceReadFile(base + ".ncer", &e) || !RaceReadFile(base + ".ncgr", &g) ||
            !RaceReadFile("graphics/2dsprites/common/fonts/cell/font.nclr", &c) || !f.bank.Load(g, e, c, 0, 0))
            continue;
        for (u16 &p : f.bank.pixels)
            if (p & 0x8000)
                p = 0xffff;
        // The cells' boxes are their OAM squares: the capitals' top and bottom come from their opaque pixels.
        float top = 1e9f, bottom = 0;
        for (const char *p = "AMH"; *p; p++) {
            u8 k = f.font->cell[(u8)*p];
            if (k >= f.bank.cells.size() || !f.bank.cells[k].w)
                continue;
            const G2Cell &cc = f.bank.cells[k];
            for (int y = 0; y < cc.h; y++)
                for (int x = 0; x < cc.w; x++)
                    if (f.bank.pixels[(size_t)(cc.v + y) * f.bank.width + cc.u + x] & 0x8000) {
                        top = fminf(top, (float)(cc.y0 + y));
                        bottom = fmaxf(bottom, (float)(cc.y0 + y + 1));
                    }
        }
        f.top = top < 1e9f ? top : 0;
        f.cap = bottom - f.top;
        f.ok = true;
    }
    return fonts;
}

G2CellBank **Banks()
{
    static G2CellBank *banks[FeCanvas::BANK_COUNT];
    static bool loaded = false;
    if (loaded)
        return banks;
    loaded = true;
    static const char *const kNames[FeCanvas::BANK_COUNT] = {"Coppers", "EnvIcons", "DifficultyIcons"};
    for (int i = 0; i < FeCanvas::BANK_COUNT; i++) {
        std::string base = std::string("graphics/2dsprites/common/fe/cell/") + kNames[i];
        std::vector<u8> e, g, c;
        G2CellBank *b = new G2CellBank;
        if (RaceReadFile(base + ".ncer", &e) && RaceReadFile(base + ".ncgr", &g) &&
            RaceReadFile("graphics/2dsprites/common/fe/cell/frontend.nclr", &c) && b->Load(g, e, c))
            banks[i] = b;
        else
            delete b;
    }
    return banks;
}

u16 Rd16(const u8 *p) { return (u16)(p[0] | p[1] << 8); }

} // namespace

bool FeImage::Load(const std::string &path)
{
    FILE *f = fopen(path.c_str(), "rb");
    if (!f)
        return false;
    u8 hd[16];
    bool ok = fread(hd, 1, 16, f) == 16 && !memcmp(hd, "FET1", 4);
    if (ok) {
        w = Rd16(hd + 4);
        h = Rd16(hd + 6);
        stride = Rd16(hd + 8);
        texH = Rd16(hd + 10);
        format = hd[12];
        ok = w > 0 && h > 0 && stride >= w && stride <= 512 && texH > h && texH <= 512 &&
             (format == F5650 || format == F4444 || format == F8888);
    }
    if (ok) {
        size_t n = Bytes();
        store.assign(n + 16, 0);
        pixels = store.data() + ((16 - ((uintptr_t)store.data() & 15)) & 15);
        ok = fread(pixels, 1, n, f) == n;
    }
    fclose(f);
    return ok;
}

void FeImage::MakeDisc(int size)
{
    w = h = stride = size;
    texH = size * 2;
    format = F8888;
    store.assign(Bytes() + 16, 0);
    pixels = store.data() + ((16 - ((uintptr_t)store.data() & 15)) & 15);
    float r = size * 0.5f;
    for (int y = 0; y <= size; y++)
        for (int x = 0; x < size; x++) {
            float dx = x + 0.5f - r, dy = (y < size ? y : size - 1) + 0.5f - r;
            float a = r - sqrtf(dx * dx + dy * dy) + 0.5f;
            u8 *p = pixels + ((size_t)y * stride + x) * 4;
            p[0] = p[1] = p[2] = 255;
            p[3] = (u8)(a <= 0 ? 0 : a >= 1 ? 255 : a * 255);
        }
}

void FeSetDataDir(const std::string &dir) { g_dataDir = dir; }

const FeImage *FeImageNamed(const char *name)
{
    static std::map<std::string, std::unique_ptr<FeImage>> cache;
    auto it = cache.find(name);
    if (it == cache.end()) {
        std::unique_ptr<FeImage> img(new FeImage);
        if (!img->Load(g_dataDir + "/" + name + ".fet")) {
            printf("fe: %s/%s.fet missing\n", g_dataDir.c_str(), name);
            img.reset();
        }
        it = cache.emplace(name, std::move(img)).first;
    }
    return it->second.get();
}

const FeImage &FeDisc()
{
    static FeImage disc;
    if (!disc.pixels)
        disc.MakeDisc(32);
    return disc;
}

void FeCanvas::Fit(int w, int h)
{
    scale = fminf((float)w / W, (float)h / H);
    ox = (w - W * scale) * 0.5f;
    oy = (h - H * scale) * 0.5f;
}

void FeCanvas::Rect(float x, float y, float w, float h, u32 c) const
{
    if (w <= 0 || h <= 0)
        return;
    rect(ox + x * scale, oy + y * scale, w * scale, h * scale, (c & 255) / 255.0f, (c >> 8 & 255) / 255.0f,
         (c >> 16 & 255) / 255.0f, (c >> 24) / 255.0f, user);
}

void FeCanvas::Image(const FeImage &img, float x, float y, float w, float h, u32 rgba) const
{
    image(img, ox + x * scale, oy + y * scale, w * scale, h * scale, 0, 0, (float)img.w, (float)img.h, rgba, user);
}

void FeCanvas::ImageCover(const FeImage &img, float x, float y, float w, float h, u32 rgba) const
{
    float s = fmaxf(w / img.w, h / img.h), cw = w / s, ch = h / s;
    float u0 = (img.w - cw) * 0.5f, v0 = (img.h - ch) * 0.5f;
    image(img, ox + x * scale, oy + y * scale, w * scale, h * scale, u0, v0, u0 + cw, v0 + ch, rgba, user);
}

// Nine parts: the disc's quarters in the corners, rectangles elsewhere.
void FeCanvas::RoundRect(float x, float y, float w, float h, float r, u32 c) const
{
    r = fminf(r, fminf(w, h) * 0.5f);
    const FeImage &d = FeDisc();
    float half = d.w * 0.5f;
    float X = ox + x * scale, Y = oy + y * scale, Wd = w * scale, Hd = h * scale, R = r * scale;
    image(d, X, Y, R, R, 0, 0, half, half, c, user);
    image(d, X + Wd - R, Y, R, R, half, 0, (float)d.w, half, c, user);
    image(d, X, Y + Hd - R, R, R, 0, half, half, (float)d.h, c, user);
    image(d, X + Wd - R, Y + Hd - R, R, R, half, half, (float)d.w, (float)d.h, c, user);
    Rect(x + r, y, w - 2 * r, h, c);
    Rect(x, y + r, r, h - 2 * r, c);
    Rect(x + w - r, y + r, r, h - 2 * r, c);
}

void FeCanvas::Panel(float x, float y, float w, float h, float r, u32 fill, u32 outline, float width) const
{
    if (width > 0) {
        RoundRect(x, y, w, h, r, outline);
        RoundRect(x + width, y + width, w - 2 * width, h - 2 * width, fmaxf(r - width, 1), fill);
    } else {
        RoundRect(x, y, w, h, r, fill);
    }
}

float FeCanvas::Text(int font, const char *s, float x, float y, u32 rgba, int align, float wrap) const
{
    const FontBank &f = Fonts()[font];
    if (!f.ok || !s || !*s)
        return 0;
    fx32 tw = 0;
    std::vector<dsGlyph> g = dsTextLayout(*f.font, s, 0, 0, wrap > 0 ? align : 0, 0,
                                          (fx32)((wrap > 0 ? wrap : 4000) * 4096), &tw);
    float width = tw / 4096.0f, dx = 0;
    if (wrap <= 0)
        dx = align == 1 ? -width * 0.5f : align == 2 ? -width : 0;
    else
        dx = align == 1 ? wrap * 0.5f : align == 2 ? wrap : 0;
    for (const dsGlyph &e : g) {
        if (e.cell >= f.bank.cells.size())
            continue;
        const G2Cell &c = f.bank.cells[e.cell];
        if (!c.w)
            continue;
        float gx = x + dx + e.x / 4096.0f + c.x0, gy = y - f.top + e.y / 4096.0f + c.y0;
        cell(f.bank, c, ox + gx * scale, oy + gy * scale, c.w * scale, c.h * scale, rgba, user);
    }
    return width;
}

float FeCanvas::TextWidth(int font, const char *s) const
{
    const FontBank &f = Fonts()[font];
    if (!f.ok || !s || !*s)
        return 0;
    fx32 tw = 0;
    dsTextLayout(*f.font, s, 0, 0, 0, 0, 4000 * 4096, &tw);
    return tw / 4096.0f;
}

float FeCanvas::TextHeight(int font, const char *s, float wrap) const
{
    const FontBank &f = Fonts()[font];
    if (!f.ok || !s || !*s)
        return 0;
    fx32 tw = 0, th = 0;
    dsTextLayout(*f.font, s, 0, 0, 0, 0, (fx32)(wrap * 4096), &tw, &th);
    return th / 4096.0f;
}

float FeCanvas::CapHeight(int font) const { return Fonts()[font].cap; }

float FeCanvas::LineHeight(int font) const
{
    const FontBank &f = Fonts()[font];
    return f.ok ? f.font->height / 4096.0f : 0;
}

void FeCanvas::Cell(int bank, int index, float x, float y, float k, u32 rgba) const
{
    G2CellBank *b = Banks()[bank];
    if (!b || index < 0 || index >= (int)b->cells.size() || !b->cells[index].w)
        return;
    const G2Cell &c = b->cells[index];
    cell(*b, c, ox + x * scale, oy + y * scale, c.w * k * scale, c.h * k * scale, rgba,
         user);
}

float FeCanvas::CellWidth(int bank, int index) const
{
    G2CellBank *b = Banks()[bank];
    if (!b || index < 0 || index >= (int)b->cells.size())
        return 0;
    return b->cells[index].w;
}
