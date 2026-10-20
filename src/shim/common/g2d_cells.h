// NitroSystem G2D stand-in: OBJ cell banks (NCER cells + NCGR characters + NCLR palette) decoded for drawing.
//
// On the DS a cell is a group of OAM entries placed relative to the cell's origin; the game's 2D sprites
// (main 0x02069584 / 0x020696b0, fields +0x28/+0x2c position, +0x3e cell, +0x10 visible) draw one cell of a bank at
// their position. Here every cell of a bank is decoded once into a 16-bit atlas (the DS BGR555 colours with the
// alpha bit set, colour index 0 transparent: exact for OBJs, whose pixels are opaque or transparent), with its
// bounding box relative to the origin. Back-ends upload the atlas and draw a cell as one textured quad.
//
// Formats (docs/ASSETS.md "2D"): block offsets count from the block payload; NCER's CEBK header is 24 bytes
// {u16 cells, u16 attr (1: 16-byte cell entries with a bounding box), u32 cell offset, u32 mapping (OBJ 1D boundary
// 32 << mapping bytes, the unit of the OAM tile number), u32 VRAM transfer table offset, ...}. With a transfer table
// each cell's characters are DMA'd to their own slot: its OAM tile numbers count from the cell's source offset.
#pragma once

#include <vector>

#include "nitro.h"

struct G2Cell {
    s16 x0 = 0, y0 = 0; // top-left of the bounding box, relative to the cell origin (DS pixels)
    u16 w = 0, h = 0;   // 0 x 0: empty cell
    u16 u = 0, v = 0;   // place in the atlas
};

struct G2CellBank {
    // Decodes the first `maxCells` cells (all when 0), in each OAM entry's palette or, when `palette` >= 0, in
    // that one (the game's text sprites set it, sprite +0x45). False when a file is malformed.
    bool Load(const std::vector<u8> &ncgr, const std::vector<u8> &ncer, const std::vector<u8> &nclr,
              int maxCells = 0, int palette = -1);

    std::vector<G2Cell> cells;
    int width = 0, height = 0; // atlas size (powers of two)
    std::vector<u16> pixels;   // ABGR1555: DS colour | 0x8000, 0 = transparent
    unsigned gl = 0;           // host: GL texture name
    void *gu = nullptr;        // PSP: back-end data
};
