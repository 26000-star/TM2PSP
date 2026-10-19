// Fixed-point idioms of the car simulation (ov003 0x021220a0..0x0212bed0), as the compiler emitted them.
#pragma once

#include "math/dsMath.h"
#include "nitro.h"

namespace carmath {

// a * b, 64-bit product rounded (+0x800) then >> 12: the usual smull/adds/adc sequence.
static inline fx32 Mul(fx32 a, fx32 b)
{
    return (fx32)(((s64)a * b + 0x800) >> 12);
}

// a * b >> 12 on 32 bits (mul then asr: wraps, truncates). Used by a few lerps.
static inline fx32 Mul32(fx32 a, fx32 b)
{
    return (fx32)((s32)((u32)a * (u32)b) >> 12);
}

// a . b with the three 64-bit products summed before the rounding (smull + 2 smlal).
static inline fx32 Dot(const VecFx32 &a, const VecFx32 &b)
{
    return (fx32)(((s64)a.x * b.x + (s64)a.y * b.y + (s64)a.z * b.z + 0x800) >> 12);
}

static inline VecFx32 Add(const VecFx32 &a, const VecFx32 &b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
static inline VecFx32 Sub(const VecFx32 &a, const VecFx32 &b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
static inline VecFx32 Neg(const VecFx32 &a) { return {-a.x, -a.y, -a.z}; }
static inline VecFx32 Scale(const VecFx32 &v, fx32 s) { return {Mul(v.x, s), Mul(v.y, s), Mul(v.z, s)}; }
static inline bool IsZero(const VecFx32 &v) { return v.x == 0 && v.y == 0 && v.z == 0; }

// v * k with k an fx64c (32.32) from FX_InvFx64c: bits 32..63 of v * k + 2^31, computed modulo 2^64.
static inline fx32 MulFx64c(fx32 v, fx64c k)
{
    return (fx32)(((u64)(s64)v * (u64)k + 0x80000000u) >> 32);
}
static inline VecFx32 ScaleFx64c(const VecFx32 &v, fx64c k)
{
    return {MulFx64c(v.x, k), MulFx64c(v.y, k), MulFx64c(v.z, k)};
}

// Cross product, each component a 64-bit difference of products rounded once (as ov003 0x020fb040).
static inline VecFx32 Cross(const VecFx32 &a, const VecFx32 &b)
{
    return {(fx32)(((s64)a.y * b.z - (s64)a.z * b.y + 0x800) >> 12),
            (fx32)(((s64)a.z * b.x - (s64)a.x * b.z + 0x800) >> 12),
            (fx32)(((s64)a.x * b.y - (s64)a.y * b.x + 0x800) >> 12)};
}

// Normalise with the game's guard: a null vector becomes (0, 0, 1) (dsVec_NormalizeSafe 0x02057d70 inlined).
static inline VecFx32 NormalizeSafe(const VecFx32 &v)
{
    if (IsZero(v))
        return {0, 0, FX32_ONE};
    VecFx32 r;
    dsVec_Normalize(&v, &r);
    return r;
}

// Removes the component of v along n: v - n * (n . v), or v when n is null. The inlined sequence of
// dsVec_Dot 0x02057d18, dsVec_ScaleTo 0x0208f9a8 and dsVec_Sub 0x0208f968 (each rounded).
static inline VecFx32 Project(const VecFx32 &n, const VecFx32 &v)
{
    if (IsZero(n))
        return v;
    return Sub(v, Scale(n, Dot(n, v)));
}

static inline fx32 Clamp01(fx32 v)
{
    if (v < 0)
        return 0;
    if (0xfff < v)
        return FX32_ONE;
    return v;
}

static inline VecFx32 Row(const MtxFx43 &m, int i) { return {m.m[i][0], m.m[i][1], m.m[i][2]}; }
static inline void SetRow(MtxFx43 &m, int i, const VecFx32 &v)
{
    m.m[i][0] = v.x;
    m.m[i][1] = v.y;
    m.m[i][2] = v.z;
}

} // namespace carmath
