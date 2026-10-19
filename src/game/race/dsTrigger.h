// Race triggers: the "finish" gates of the finish and multilap pieces and the "cp%d" gates of the checkpoints
// (ov003 0x02111280, 0x021114e4). A trigger is a rectangle standing on the piece: its matrix is the piece matrix
// with the x row scaled by TriggerSize.x, the y row by TriggerSize.y, and the translation moved by the rotated
// InitPos and raised by TriggerSize.z. A player "enters" the trigger when the test point crosses the plane of the
// rectangle (either way) inside it.
#pragma once

#include <string>

#include "nitro.h"

struct dsPieceInfo;
class dsTrackPiece;

// Gate shape (object +0x80 of a trigger, vtable 0x021306d0 area; constructor ov003 0x02130690).
struct dsTriggerGate {
    fx32 radius = 0;           // +0x04 half the diagonal of the x/y rows: points further from the centre never hit
    VecFx32 centre = {};       // +0x08 matrix translation
    VecFx32 normal = {};       // +0x14 unit z row
    fx32 d = 0;                // +0x20 -normal . centre
    MtxFx43 inverse;           // +0x24 world -> unit rectangle (x, y in (-0.55, 0.55))
    fx32 side[8] = {};         // +0x54 per player: -4095 / 0 / 4096, sign of the last tested point

    void Set(const MtxFx43 &m);                        // ov003 0x02130358
    // True when the point changed side since the last test of this player and lies inside the rectangle.
    bool Test(const VecFx32 &p, int player);           // ov003 0x021304dc
    void ResetSide(int player, const VecFx32 &p);      // ov003 0x02130620
    fx32 SideOf(const VecFx32 &p) const;
};

// Base trigger (constructor ov003 0x0212f4a4) with the race listener at +0xf4. Finish triggers (0xf8 bytes,
// vtable 0x02136630) report every entry; checkpoint triggers (0xfc bytes, vtable 0x02136668) report the first
// one after a reset (pending flag +0xf8).
class dsTrigger {
public:
    enum Kind { FINISH, CHECKPOINT };

    dsTrigger(Kind kind, const std::string &name, const MtxFx43 &m, int pieceIndex);

    // vtable +0x18 (ov003 0x02113f50 finish, 0x02114168 checkpoint): returns true when the listener's "enter"
    // fires an event (finish: always on entry; checkpoint: only while pending, which it clears).
    bool Update(int player, const VecFx32 &p);
    // vtable +0x14 (ov003 0x02113f1c / 0x02114134): side of the player's current position, inside bit cleared.
    void ResetPlayer(int player, const VecFx32 &p);

    // Builds the trigger matrix of a piece (the shared part of ov003 0x02111280 and 0x021114e4).
    static MtxFx43 PieceMatrix(const dsTrackPiece &piece, const dsPieceInfo &info);

    Kind kind;
    std::string name;          // +0x04 "finish" / "cp<n>"
    MtxFx43 matrix;            // +0x10 (and its copy +0x40)
    u32 playerMask = 0xffffffff; // +0x70
    u32 inside = 0;            // +0x78 one bit per player
    bool active = true;        // +0x7e bit 0
    dsTriggerGate gate;        // +0x80
    int pieceIndex = -1;       // +0xf4 (checkpoints: index of the piece in the track's piece list)
    bool pending = true;       // +0xf8 (checkpoints: not passed yet)
};
