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
    // The editor's direction markers of a start, finish, checkpoint or multilap piece (models the DS creates only
    // in the editor modes, flag *0x021378dc): the piece's Helper model (Misc/helpers/<env>/<Helper>, Island, Snow,
    // Coast), or Stadium's arch and arrow (Tracks/sta/Arch<type>, Arrow<type>). Returns how many (0 to 2).
    struct Marker {
        std::string model;
        MtxFx43 matrix;
    };
    int EditorMarkers(Marker out[2]) const;         // ov003 0x0211eac0 + 0x0211e74c (placement only)
    // Loads graphics/<modelPath>.h as a mesh shape placed with the piece matrix and adds it to the collision world
    // (the shape creation of ov003 0x0211fe08: world vtable +0x14 then shape vtable +0x08), then the pillars'.
    void CreateCollision();

    s32 id;                     // +0x04 piece id (-1 = test arrow)
    s32 type = 0;               // +0x08 dsPieceType
    VecFx32 pos = {};           // +0x2c world position of the origin cell
    s32 cell[3] = {};           // +0x38 grid cell of pos
    dsPieceInfo *info = nullptr; // +0x64
    bool cullable = true;       // +0x60
    u8 rotation;                // +0x74 quarter turns around Y
    bool air = false;           // +0x76 uses the "air" model (above the ground level)
    bool autoTerrain = false;   // +0x7a placed by the terrain builder
    std::string modelPath;      // +0x0c "Tracks/<env>/<model name>" (collision: graphics/<path>.h)
    MtxFx43 matrix;             // model +0x20: rotation, and translation = pos moved to the footprint corner
    class dsCollisionMeshShape *collision = nullptr; // +0x7c
    // Snow: the pillar pieces under a piece above the ground level, one per set bit of its cell height
    // (<name>pillar1/2/4/8 stacked from the ground). Owned; not in the track's piece list.
    dsTrackPiece *pillars[4] = {}; // +0x94
    ~dsTrackPiece();

private:
    void UpdatePillars(const VecFx32 &oldPos, bool force); // ov003 0x0211ef00
    void CreatePillars();                                  // ov003 0x0211ee60
    void DeletePillars();
};

class dsTerrainFilter;

class dsTrack {
public:
    ~dsTrack();
    // Runs scene <name> and fills the header; with pieces != NULL also places its pieces. ov003 0x0211b404.
    bool LoadScene(const char *name, bool withPieces);

    // Rotation of the piece whose footprint holds the cell of pos, -1 when none. Stand-in for the race grid
    // lookup main 0x0208d404 (0x0208cb80 maps the position to a cell, then a byte per cell names the piece);
    // the port walks the footprints instead of keeping the grid (W x D cells towards -x and +z of the origin
    // cell, swapped for odd rotations, H cells up).
    int PieceRotationAt(const VecFx32 &pos) const;
    // Piece covering grid cell (x, y, z), NULL when none (same footprint walk; grid lookup main 0x0208d3c4).
    dsTrackPiece *PieceAtCell(s32 x, s32 y, s32 z) const;
    // Coast/Island terrain: cliff and beach pieces around the ground-level pieces (dsTerrain_Build with no
    // piece, ov003 0x02100998). New pieces are appended with autoTerrain set; returns how many.
    int BuildTerrain();

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
    // ov003 0x02100998 / 0x02100480 / 0x02100750 / 0x02100554, see dsTrack::BuildTerrain.
    int Build(dsTrack &t) const;

private:
    bool HasSolidNeighbour(const dsTrack &t, s32 x, s32 z) const;
    bool IsSolid(const dsTrackPiece *p) const;
    bool m_active = false;      // +0x04: Coast or Island
    bool m_editor = false;      // +0x05: game mode 6 or 7
    s32 m_groundLevel = 0;      // +0x08
    u32 m_grid[3] = {20, 20, 20}; // +0x0c
    s32 m_cell[2] = {};         // +0x18
    u32 m_ids[6] = {};          // +0x20: corner, dead end, base1 (twice), grass/cross, base5; [2] | 0x30000
};
