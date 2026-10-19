// A track: its header (laps, environment, time of day, medal times) and its placed pieces.
// Race tracks are Lua scenes, Scenes/<env>/<difficulty>/<n>.dcsc, whose global Track is a list of entries
// { key, value... }: "Environment", "TimeOfDay", "AuthorTime", "AuthorName", "LapCount", then one entry per piece
// { name, rotation, x, y, z } in grid cells (fx32 integers). Read by ov003 0x0211b404 (the editor's bit-packed
// format, 0x0211a1f0, places pieces the same way).
#pragma once

#include <string>
#include <vector>

#include "nitro.h"

struct dsPieceInfo;

struct dsTrackInfo {
    u8 lapCount = 0;           // +0x00
    u8 pieceCount = 0;         // +0x03 (bit-packed tracks only)
    s32 environment = 0;       // +0x04
    s32 timeOfDay = 0;         // +0x08
    fx32 authorTime = 0;       // +0x10 seconds
    char authorName[12] = {};  // +0x20 (10 characters max)
    fx32 goldTime = 0;         // +0x44 author + 2 s
    fx32 silverTime = 0;       // +0x48 gold + 3 s
    fx32 bronzeTime = 0;       // +0x4c silver + 5 s

    void SetAuthorTime(fx32 t);                        // 0x0209fa5c
};

// One placed piece (0xa4 bytes, constructor ov003 0x0212090c). The model, collision shape, helpers and triggers
// it owns in the original are created by later slices; this keeps the placement.
class dsTrackPiece {
public:
    dsTrackPiece(s32 id, const VecFx32 &pos, u8 rotation); // ov003 0x0212090c (partial)

    void SetPosition(const VecFx32 &pos);              // ov003 0x0211fe08 (partial: cells + model choice)
    void UpdateTransform();                            // ov003 0x0211f088 + the rotation of 0x0211f2f0
    // Loads graphics/<modelPath>.h as a mesh shape placed with the piece matrix and adds it to the collision world
    // (the shape creation of ov003 0x0211fe08: world vtable +0x14 then shape vtable +0x08).
    void CreateCollision();

    s32 id;                     // +0x04 piece id (-1 = test arrow)
    s32 type = 0;               // +0x08 dsPieceType
    VecFx32 pos = {};           // +0x2c world position of the origin cell
    s32 cell[3] = {};           // +0x38 grid cell of pos
    dsPieceInfo *info = nullptr; // +0x64
    bool cullable = true;       // +0x60
    u8 rotation;                // +0x74 quarter turns around Y
    bool air = false;           // +0x76 uses the "air" model (above the ground level)
    std::string modelPath;      // +0x0c "Tracks/<env>/<model name>" (collision: graphics/<path>.h)
    MtxFx43 matrix;             // model +0x20: rotation, and translation = pos moved to the footprint corner
    class dsCollisionMeshShape *collision = nullptr; // +0x7c
    ~dsTrackPiece();
};

class dsTerrainFilter;

class dsTrack {
public:
    ~dsTrack();
    // Runs scene <name> and fills the header; with pieces != NULL also places its pieces. ov003 0x0211b404.
    bool LoadScene(const char *name, bool withPieces);

    dsTrackInfo info;
    std::vector<dsTrackPiece *> pieces;
};

// Coast and Island tracks list their cliff/beach terrain pieces, but the game rebuilds the terrain itself
// (ov003 0x02100b2c), so the loader skips those six piece ids. ov003 0x02100d7c (partial).
class dsTerrainFilter {
public:
    explicit dsTerrainFilter(s32 env);                 // ov003 0x02100d7c
    bool IsTerrainPiece(u32 id) const;                 // ov003 0x02100438
    bool Active() const { return m_active; }

private:
    bool m_active = false;      // +0x04: Coast or Island
    bool m_editor = false;      // +0x05: game mode 6 or 7
    s32 m_groundLevel = 0;      // +0x08
    u32 m_grid[3] = {20, 20, 20}; // +0x0c
    s32 m_cell[2] = {};         // +0x18
    u32 m_ids[6] = {};          // +0x20: corner, dead end, base1 (twice), grass/cross, base5; [2] | 0x30000
};
