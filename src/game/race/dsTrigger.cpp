#include "race/dsTrigger.h"

#include "collision/dsCollisionWorld.h"
#include "math/dsMath.h"
#include "track/dsPieceData.h"
#include "track/dsRaceSetup.h"
#include "track/dsTrack.h"

static inline fx32 MulR(fx32 a, fx32 b) { return (fx32)(((s64)a * b + 0x800) >> 12); }

static fx32 Sign(fx32 v) { return v < 0 ? -4095 : (v > 0 ? FX32_ONE : 0); }

// @port ov003:02130358 done
void dsTriggerGate::Set(const MtxFx43 &m)
{
    centre = {m.m[3][0], m.m[3][1], m.m[3][2]};
    VecFx32 n = {m.m[2][0], m.m[2][1], m.m[2][2]};
    if (n.x == 0 && n.y == 0 && n.z == 0)
        n = {0, 0, FX32_ONE};
    else
        dsVec_Normalize(&n, &n);
    normal = n;
    d = -(fx32)(((s64)centre.z * n.z + (s64)centre.x * n.x + (s64)n.y * centre.y + 0x800) >> 12);
    VecFx32 r0 = {m.m[0][0], m.m[0][1], m.m[0][2]}, r1 = {m.m[1][0], m.m[1][1], m.m[1][2]};
    VecFx32 half = {MulR(dsVec_Mag(&r0), 0x800), MulR(dsVec_Mag(&r1), 0x800), 0};
    radius = dsVec_Mag(&half);
    inverse = m;
    dsMtx43_InverseScaled(&inverse);
}

fx32 dsTriggerGate::SideOf(const VecFx32 &p) const
{
    return Sign(d + (fx32)(((s64)p.z * normal.z + (s64)p.x * normal.x + (s64)normal.y * p.y + 0x800) >> 12));
}

// @port ov003:021304dc done
bool dsTriggerGate::Test(const VecFx32 &p, int player)
{
    fx32 s = SideOf(p);
    if (s == side[player])
        return false;
    side[player] = s;
    if (dsVec_Distance(&centre, &p) > radius)
        return false;
    VecFx32 l;
    dsVec_MulMtx43(&p, &inverse, &l);
    const fx32 lo = -2253, hi = 2253; // 0.55
    return lo < l.x && l.x < hi && lo < l.y && l.y < hi;
}

// @port ov003:02130620 done
void dsTriggerGate::ResetSide(int player, const VecFx32 &p) { side[player] = SideOf(p); }

// @port ov003:0212f4a4 partial (the name string, matrices, mask and flags; the scene-node part 0x02046358 is not
// kept) / ov003:02130690 done
dsTrigger::dsTrigger(Kind k, const std::string &n, const MtxFx43 &m, int piece)
    : kind(k), name(n), matrix(m), pieceIndex(piece)
{
    gate.Set(m);
}

// @port ov003:02113f50 done (finish: enter -> race OnFinish through 0x021222dc)
// @port ov003:02114168 done (checkpoint: enter -> 0x0213018c, race OnCheckpoint while pending)
bool dsTrigger::Update(int player, const VecFx32 &p)
{
    if (!active) {
        inside = 0;
        return false;
    }
    u32 bit = 1u << player;
    if (!gate.Test(p, player)) {
        inside &= ~bit; // leave (listener +0x184 / 0x021222d4: nothing)
        return false;
    }
    if (inside & bit)
        return false;   // stay (0x02130188 / 0x021222d8: nothing)
    inside |= bit;
    if (kind == FINISH)
        return true;
    bool fire = pending;
    pending = false;
    return fire;
}

// @port ov003:02113f1c done / ov003:02114134 done
void dsTrigger::ResetPlayer(int player, const VecFx32 &p)
{
    gate.ResetSide(player, p);
    inside &= ~(1u << player);
}

// Piece matrix (model +0x20) with rows 0 and 1 scaled by TriggerSize.x / .y, translation + InitPos (ground or air)
// rotated by the piece, + TriggerSize.z on y. The checkpoint loop (ov003 0x021114e4) also turns Stadium's
// horizontal ring (piece 0x10, cpringh) a quarter about its scaled row 0 (angle 6432: MTX_RotAxis then
// MTX_Concat33 rows x rotation), so that the gate lies in the ring.
// @port ov003:02111280 partial (matrix part; the loop is dsRaceProgress::CreateTriggers)
MtxFx43 dsTrigger::PieceMatrix(const dsTrackPiece &piece, const dsPieceInfo &info)
{
    MtxFx43 m = piece.matrix;
    VecFx32 off = piece.air ? info.initPosAir : info.initPosGround;
    dsVec_MulMtx33(&off, &m, &off);
    for (int c = 0; c < 3; c++) {
        m.m[0][c] = MulR(m.m[0][c], info.triggerSize.x);
        m.m[1][c] = MulR(m.m[1][c], info.triggerSize.y);
    }
    if (dsRaceSetup::Get()->environment == ENV_STADIUM && piece.id == 0x10) {
        VecFx32 axis = {m.m[0][0], m.m[0][1], m.m[0][2]};
        if (axis.x == 0 && axis.y == 0 && axis.z == 0)
            axis = {0, 0, FX32_ONE};
        else
            dsVec_Normalize(&axis, &axis);
        MtxFx43 r;
        dsMtx43_RotateAxis(&r, &axis, dsSin(6432), dsCos(6432));
        MtxFx43 rows = m;
        for (int i = 0; i < 3; i++)   // MTX_Concat33: 64-bit sums truncated
            for (int j = 0; j < 3; j++)
                m.m[i][j] = (fx32)(((s64)rows.m[i][0] * r.m[0][j] + (s64)rows.m[i][1] * r.m[1][j] +
                                    (s64)rows.m[i][2] * r.m[2][j]) >> 12);
    }
    off.y += info.triggerSize.z;
    m.m[3][0] += off.x;
    m.m[3][1] += off.y;
    m.m[3][2] += off.z;
    return m;
}
