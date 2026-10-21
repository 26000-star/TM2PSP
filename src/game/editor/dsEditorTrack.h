// The track editor's model (ov002 + main's track grid): the block menu, the occupancy grid with the pieces' notch
// masks, placing and deleting blocks, the editor budget, the validation rules and the bit-packed track format of the
// custom tracks (the DS keeps them in the game card's save; the port writes the same bytes to a file).
// Platform independent: the editor screen (src/app/editor.cpp) drives it and draws its pieces.
#pragma once

#include <map>
#include <memory>
#include <string>
#include <vector>

#include "nitro.h"
#include "track/dsTrack.h"

struct dsPieceInfo;

// One entry of the editor's block menu: Scripts/screens/<Env>/TrackPieceLayout (editor) or Scenes/puz/<diff>/<n>-Layout
// (puzzle). Each global table is a folder, a list of { FOLDER or PIECE, name, "0x<icon>" [, count] }; the root is
// TopFolder. The icon is a cell of Editor/Cell/<Env>Folders or <Env>Blocks. Read by ov002 0x020e763c.
struct dsBlockMenuEntry {
    bool folder = false;
    std::string name;  // folder: the global's name; piece: the piece's name in PieceData (any case)
    int icon = 0;
    int count = -1;    // puzzle layouts: blocks available (-1: no limit)
    u32 id = 0;        // piece id (pieces only; 0 when the catalogue has no such piece)
};

class dsBlockMenu {
public:
    // Reads every folder reachable from TopFolder. False when the script is missing.
    bool Load(const char *script);
    const std::vector<dsBlockMenuEntry> *Folder(const std::string &name) const;
    std::map<std::string, std::vector<dsBlockMenuEntry>> folders;
    bool puzzle = false; // entries carry counts
};

// The editor's validation result (ov002 0x020e5808, the screen's +0xd0): what the Validate / Test buttons say.
enum dsEditorCheck {
    CHECK_OK = 1,
    CHECK_NO_START = 2,       // "Validate1StartPiece": no start or multilap block
    CHECK_MANY_STARTS = 3,    // "ValidateOnly1Start": more than one start / multilap
    CHECK_NO_FINISH = 4,      // "Validate1FinishPiece"
    CHECK_MULTILAP_MIX = 5,   // "Validate1MultilapNoStartFinish": a multilap with start or finish blocks
};

class dsEditorTrack {
public:
    // A new track of environment env (piece catalogue, race setup and grid for it), as tracks/editor_<env>:
    // lap count 1, time of day 2 (sunset), no author time.
    explicit dsEditorTrack(s32 env);
    ~dsEditorTrack();

    s32 env;
    dsTrack track;        // pieces (with the Coast / Island terrain, autoTerrain) and header
    bool puzzle = false;  // race setup mode 7: no budget limit, locked pieces
    u32 gridSize[3] = {}; // cells x, y, z (dsRaceSetup::GridSize)
    bool border = false;  // Island and Coast keep a one-cell border (grid +0x414)
    int maxPieces = 0;    // grid +0x418, per environment (main 0x020b8f5c): 250 127 240 127

    // Grid cell of a piece's origin, and the world position of a cell (as the packed loader ov003 0x0211a1f0).
    static VecFx32 CellPosition(s32 env, s32 x, s32 y, s32 z);

    // The cells a piece of `info` at rotation rot covers when its origin is (x, y, z): the notch mask turned by
    // main 0x0209e0a8, walked as main 0x0208d1d4 (H layers, D' rows towards +z, W' columns towards -x; W' and D'
    // swapped for odd rotations). With pillars the column below is included (0x0208d1d4 with piece +0x94).
    void Footprint(const dsPieceInfo &info, int rot, s32 x, s32 y, s32 z, bool pillars,
                   std::vector<s32> *cells) const;
    bool InBounds(const dsPieceInfo &info, int rot, s32 x, s32 y, s32 z) const; // main 0x0208cf94
    // The pieces a block placed there would replace (0x0208d1d4, repeated after each removal by 0x020ddf98);
    // `locked` is set when one of them is a puzzle's fixed piece (0x0208d098: the placement is refused).
    void Overlaps(const dsPieceInfo &info, int rot, s32 x, s32 y, s32 z, std::vector<dsTrackPiece *> *out,
                  bool *locked) const;
    dsTrackPiece *PieceAt(s32 x, s32 y, s32 z) const; // grid byte lookup (main 0x0208d3c4)

    enum PlaceResult { PLACED, OUT_OF_BOUNDS, LOCKED, FULL, NO_PIECE };
    // Places a block, deleting the pieces under it (ov002 0x020ddf98 / 0x020ec334), then rebuilds the terrain.
    PlaceResult Place(u32 id, int rot, s32 x, s32 y, s32 z, dsTrackPiece **placed = nullptr);
    PlaceResult CanPlace(u32 id, int rot, s32 x, s32 y, s32 z); // Place's checks only (the track is unchanged)
    bool Remove(dsTrackPiece *p);   // ov002 0x020ebfb4 (locked and terrain pieces stay)
    void Clear();                   // "Clear All": every piece but the locked ones
    // The terrain around the ground-level pieces (Coast, Island), rebuilt after each change (ov003 0x02100b2c).
    void RebuildTerrain();

    // Budget (ov002 0x020f091c): polygons (piece CostPoly, 190 at most) and memory (one per piece, plus CostMem
    // for the first piece of each id; limits per environment 612 630 612 642). Terrain pieces count their
    // polygons and one unit. Never full in puzzle mode.
    int polyUsed = 0, memUsed = 0;
    int memLimit() const;
    bool WouldOverflow(u32 id) const; // ov002 0x020ebe90
    float Usage() const;              // 0..1, the larger of the two, for the gauge
    int CountOf(u32 id) const;        // pieces of this id (main 0x0208ce20), terrain excluded

    // Validation (ov002 0x020e5808): also sets the lap count of a multilap track to at least 2 (1 otherwise).
    dsEditorCheck Validate();
    dsTrackPiece *StartPiece() const; // the first start or multilap piece (main 0x0208cdcc)

    // Bit-packed custom track (ov003 0x02119bec header + 0x0211a1f0 pieces; writer ov003 0x0211a7e0):
    // see the .cpp for the layout. Load replaces the track (its environment must be this one).
    struct Header {
        char name[13] = {};    // +0x14 track name (12 characters)
        char author[13] = {};  // +0x20 author (12)
        u8 lapCount = 1;       // +0x00
        u8 platform = 0;       // +0x01 challenge type: 0 race, 1 platform
        u8 unknown2 = 0;       // +0x02
        u8 environment = 0;    // +0x04 (3 bits)
        u8 timeOfDay = 2;      // +0x08 (2 bits)
        u8 difficulty = 1;     // +0x0c (4 bits)
        u8 date[6] = {};       // +0x2c..+0x40: year - 2000, month, day, hour, minute, second
        fx32 authorTime = 0, goldTime = 0, silverTime = 0, bronzeTime = 0; // +0x10 +0x44 +0x48 +0x4c
        u8 extra[3] = {};      // +0x50 +0x51 +0x52
    };
    Header header;
    std::vector<u8> SavePacked() const;
    bool LoadPacked(const std::vector<u8> &data);
    // Reads a packed track's header only (the load / save lists).
    static bool ReadHeader(const std::vector<u8> &data, Header *out);

    // Free draw (ov002 0x020e62cc, 0x020e5b48, 0x020e5f58): a stroke over the cells lays the environment's four
    // road blocks (ov002 0x020eff0c: straight, corner, T, cross; Island highway*, Snow woodroad*, Coast coastroad*),
    // each cell taking the block and rotation whose connections (table ov002 0x020ee7a0) match its neighbours and the
    // stroke's direction; the previous cell is then fitted again to its neighbours (a dead end: a straight along the
    // stroke). Begin starts a stroke, Step adds a cell (true when blocks changed). The stroke never goes below the
    // ground level (ov002 0x020df178).
    void FreeDrawBegin();
    bool FreeDrawStep(s32 x, s32 y, s32 z);
    u32 roadIds[4] = {};
    // A copy of the pieces for undo (port addition to the DS's own undo list of ov002 0x020eb750).
    struct Snapshot {
        struct P {
            u32 id;
            u8 rot;
            s32 cell[3];
            bool locked;
        };
        std::vector<P> pieces;
    };
    Snapshot Save() const;
    void Restore(const Snapshot &s);

    // The puzzle's own pieces (Scenes/puz/<diff>/<n>): piece +0x77, they cannot be moved, replaced or deleted.
    bool IsLocked(const dsTrackPiece *p) const;
    void SetLocked(dsTrackPiece *p, bool on);
    // Loads a scene as the starting pieces (puzzle: every piece locked).
    bool LoadScene(const char *scene, bool lockAll);

private:
    void Index();                   // rebuild the occupancy grid from the pieces
    void Mark(dsTrackPiece *p, bool on);
    void AddBudget(const dsTrackPiece *p);
    void SubBudget(const dsTrackPiece *p);
    dsTrackPiece *NewPiece(u32 id, int rot, s32 x, s32 y, s32 z);
    std::vector<u16> m_cells;       // piece index + 1 per cell, 0 = empty
    dsTrackPiece *FreeDrawFit(s32 x, s32 y, s32 z, u8 mask); // ov002 0x020e5b48 (null: no road block fits)
    bool FreeDrawPut(dsTrackPiece *p);                       // place it unless the same block is already there
    bool m_strokeFirst = true;
    bool m_strokeLoose = false; // the stroke's first cell got the default straight (ov002 +0x44)
    s32 m_strokePrev[3] = {};
    std::vector<const dsTrackPiece *> m_locked;
};
