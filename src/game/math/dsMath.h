// Firebrand's math helpers. The hot ones live in ITCM (0x01ffa6ec..0x01ffab20) and use the hardware divider and
// square-root units directly; the shim reproduces those results with 64-bit integer C.
#pragma once

#include "nitro.h"

void dsMtx43_Copy(const MtxFx43 *src, MtxFx43 *dst);                       // itcm 0x01ffa6ec
void dsMtx43_Identity(MtxFx43 *m);                                         // itcm 0x01ffa708
void dsVec_MulMtx33(const VecFx32 *v, const MtxFx43 *m, VecFx32 *out);     // itcm 0x01ffa8a4 (rotation only, truncating)
fx32 dsVec_Mag(const VecFx32 *v);                                          // itcm 0x01ffaa58
void dsVec_Normalize(const VecFx32 *v, VecFx32 *out);                      // itcm 0x01ffaac0

// Sine/cosine of an angle in fx32 radians through the SDK table, as the game inlines it everywhere:
// idx = 65535 * (angle mod 2pi) / 2pi, entry idx >> 4. 2pi = 25736 (6.2832).
static const fx32 DS_FX_TWO_PI = 25736;
static inline int dsAngleIndex(fx32 angle)
{
    s32 a = angle < 0 ? -angle : angle;
    a = FX_ModS32(a, DS_FX_TWO_PI);
    if (angle < 0)
        a = DS_FX_TWO_PI - a;
    return (0xffff * a / DS_FX_TWO_PI) >> 4;
}
static inline fx32 dsSin(fx32 angle) { return FX_SinCosTable_[dsAngleIndex(angle) * 2]; }
static inline fx32 dsCos(fx32 angle) { return FX_SinCosTable_[dsAngleIndex(angle) * 2 + 1]; }

// The game converts a few fx32 values through software doubles: d2i(i2d(x) + 0.5), which rounds toward zero
// after adding a half (MSL _dadd 0x0201201c, _dflt 0x020125a4, _dfix 0x02012438).
static inline s32 dsRoundD(s32 x) { return (s32)((double)x + 0.5); }
