// G2D stand-in: cell banks decoded into an atlas (see g2d_cells.h). Same reading as tools/assets/nitro2d.py.
#include "g2d_cells.h"

#include <string.h>

#include <algorithm>

namespace {

u16 Rd16(const std::vector<u8> &d, size_t o) { return o + 2 <= d.size() ? (u16)(d[o] | d[o + 1] << 8) : 0; }
u32 Rd32(const std::vector<u8> &d, size_t o) { return o + 4 <= d.size() ? (u32)(Rd16(d, o) | Rd16(d, o + 2) << 16) : 0; }

// Payload of block `tag` (written as in GBATEK, e.g. "CHAR"; runtime files store it reversed) or false.
bool Block(const std::vector<u8> &d, const char *tag, size_t *off, size_t *size)
{
    if (d.size() < 16)
        return false;
    size_t o = Rd16(d, 0x0c);
    int n = Rd16(d, 0x0e);
    for (int i = 0; i < n && o + 8 <= d.size(); i++) {
        u32 sz = Rd32(d, o + 4);
        bool match = true;
        for (int k = 0; k < 4; k++)
            match = match && (d[o + k] == (u8)tag[3 - k] || d[o + k] == (u8)tag[k]);
        if (match && o + sz <= d.size() && sz >= 8) {
            *off = o + 8;
            *size = sz - 8;
            return true;
        }
        if (sz < 8)
            return false;
        o += sz;
    }
    return false;
}

// OBJ sizes by shape (attr0 >> 14) and size (attr1 >> 14).
const u8 kObjSize[3][4][2] = {
    {{8, 8}, {16, 16}, {32, 32}, {64, 64}},
    {{16, 8}, {32, 8}, {32, 16}, {64, 32}},
    {{8, 16}, {8, 32}, {16, 32}, {32, 64}},
};

int Pow2(int v)
{
    int p = 8;
    while (p < v)
        p <<= 1;
    return p;
}

struct Oam {
    int x, y, w, h, tile, pal;
    bool hf, vf, bpp8;
};

} // namespace

bool G2CellBank::Load(const std::vector<u8> &ncgr, const std::vector<u8> &ncer, const std::vector<u8> &nclr,
                      int maxCells)
{
    cells.clear();
    pixels.clear();
    width = height = 0;
    size_t co, cs, eo, es, po, ps;
    if (!Block(ncgr, "CHAR", &co, &cs) || !Block(ncer, "CEBK", &eo, &es) || !Block(nclr, "PLTT", &po, &ps))
        return false;
    // Characters: u16 height, u16 width (tiles), u32 depth (3: 4 bpp, 4: 8 bpp), u32 mapping, u32 format,
    // u32 size, u32 offset (from the payload).
    bool bpp8 = Rd32(ncgr, co + 4) == 4;
    size_t chrSize = Rd32(ncgr, co + 0x10), chrOff = co + Rd32(ncgr, co + 0x14);
    if (chrOff + chrSize > ncgr.size())
        return false;
    const u8 *chr = &ncgr[chrOff];
    // Palette: u32 depth, u32 ext, u32 size, u32 offset.
    size_t palSize = Rd32(nclr, po + 8), palOff = po + Rd32(nclr, po + 12);
    if (palOff + palSize > nclr.size())
        return false;
    std::vector<u16> pal(palSize / 2);
    for (size_t i = 0; i < pal.size(); i++)
        pal[i] = Rd16(nclr, palOff + 2 * i);

    int nCells = Rd16(ncer, eo), attr = Rd16(ncer, eo + 2);
    u32 cellOff = Rd32(ncer, eo + 4), mapping = Rd32(ncer, eo + 8), vramOff = Rd32(ncer, eo + 12);
    u32 boundary = mapping < 4 ? 32u << mapping : 32u;
    size_t entry = attr == 1 ? 16 : 8, cellBase = eo + cellOff, oamBase = cellBase + nCells * entry;
    std::vector<u32> src(nCells, 0);
    if (vramOff) {
        size_t tab = eo + vramOff + Rd32(ncer, eo + vramOff + 4);
        for (int i = 0; i < nCells; i++)
            src[i] = Rd32(ncer, tab + 8 * i);
    }
    if (maxCells > 0 && maxCells < nCells)
        nCells = maxCells; // after the layout offsets, which depend on the full count

    // Decode the OAM entries and bounding boxes.
    std::vector<std::vector<Oam>> oams(nCells);
    cells.resize(nCells);
    for (int i = 0; i < nCells; i++) {
        int n = Rd16(ncer, cellBase + i * entry);
        u32 first = Rd32(ncer, cellBase + i * entry + 4);
        int x0 = 0x7fff, y0 = 0x7fff, x1 = -0x7fff, y1 = -0x7fff;
        for (int k = 0; k < n; k++) {
            size_t o = oamBase + first + 6 * k;
            u16 a0 = Rd16(ncer, o), a1 = Rd16(ncer, o + 2), a2 = Rd16(ncer, o + 4);
            int shape = a0 >> 14, size = a1 >> 14;
            if (shape > 2)
                continue;
            Oam e;
            e.y = a0 & 0xff;
            if (e.y >= 128)
                e.y -= 256;
            e.x = a1 & 0x1ff;
            if (e.x >= 256)
                e.x -= 512;
            e.w = kObjSize[shape][size][0];
            e.h = kObjSize[shape][size][1];
            e.tile = a2 & 0x3ff;
            e.pal = a2 >> 12;
            bool affine = a0 & 0x100;
            e.hf = !affine && (a1 & 0x1000);
            e.vf = !affine && (a1 & 0x2000);
            e.bpp8 = (a0 & 0x2000) || bpp8;
            oams[i].push_back(e);
            x0 = std::min(x0, e.x);
            y0 = std::min(y0, e.y);
            x1 = std::max(x1, e.x + e.w);
            y1 = std::max(y1, e.y + e.h);
        }
        if (oams[i].empty())
            continue;
        cells[i].x0 = (s16)x0;
        cells[i].y0 = (s16)y0;
        cells[i].w = (u16)(x1 - x0);
        cells[i].h = (u16)(y1 - y0);
    }

    // Shelf packing with a transparent pixel around each cell (bilinear filtering on the PSP).
    int maxW = 0;
    for (const G2Cell &c : cells)
        maxW = std::max(maxW, (int)c.w + 2);
    width = std::max(Pow2(maxW), 256);
    int x = 0, y = 0, rowH = 0;
    for (G2Cell &c : cells) {
        if (!c.w)
            continue;
        if (x + c.w + 2 > width) {
            x = 0;
            y += rowH;
            rowH = 0;
        }
        c.u = (u16)(x + 1);
        c.v = (u16)(y + 1);
        x += c.w + 2;
        rowH = std::max(rowH, (int)c.h + 2);
    }
    height = Pow2(y + rowH);
    pixels.assign((size_t)width * height, 0);

    for (int i = 0; i < nCells; i++) {
        const G2Cell &c = cells[i];
        for (const Oam &e : oams[i]) {
            size_t tileBytes = e.bpp8 ? 64 : 32, base = src[i] + (size_t)e.tile * boundary;
            int tw = e.w / 8;
            for (int py = 0; py < e.h; py++)
                for (int px = 0; px < e.w; px++) {
                    size_t t = (size_t)(py / 8) * tw + px / 8, pix = (py & 7) * 8 + (px & 7);
                    size_t o = base + t * tileBytes + (e.bpp8 ? pix : pix / 2);
                    if (o >= chrSize)
                        continue;
                    int ci = e.bpp8 ? chr[o] : (chr[o] >> ((pix & 1) * 4)) & 15;
                    if (!ci)
                        continue;
                    size_t idx = e.bpp8 ? ci : e.pal * 16 + ci;
                    u16 col = idx < pal.size() ? pal[idx] : 0;
                    int dx = e.x - c.x0 + (e.hf ? e.w - 1 - px : px);
                    int dy = e.y - c.y0 + (e.vf ? e.h - 1 - py : py);
                    pixels[(size_t)(c.v + dy) * width + c.u + dx] = (u16)((col & 0x7fff) | 0x8000);
                }
        }
    }
    return true;
}
