/* FX fixed-point math. On the DS these use the hardware divider and square-root units; the results are the
 * integer quotient/remainder/root of the same operands, so plain C reproduces them exactly. */
#include <math.h>

#include "nitro.h"

s16 FX_SinCosTable_[4096 * 2];

void FX_Init(void)
{
    /* Same values as the SDK table: round(sin/cos(2*pi*i/4096) * 4096). */
    for (int i = 0; i < 4096; i++) {
        double a = 2.0 * 3.14159265358979323846 * i / 4096.0;
        FX_SinCosTable_[i * 2] = (s16)lround(sin(a) * 4096.0);
        FX_SinCosTable_[i * 2 + 1] = (s16)lround(cos(a) * 4096.0);
    }
}

/* FX_Div 0x0202c4b4 = FX_DivAsync (64/32 mode, numerator numer << 32) + FX_GetDivResult, which rounds the 32.32
 * quotient to 20.12: (q + 2^19) >> 20. FX_Inv 0x0202c4f0 is the same with numer = 1.0. */
static fx32 RoundDivResult(fx64c q)
{
    return (fx32)(u32)((u64)(q + (1 << 19)) >> 20);
}

fx32 FX_Div(fx32 numer, fx32 denom)
{
    if (denom == 0)
        return numer < 0 ? (fx32)0x80000000 : 0x7fffffff;
    return RoundDivResult(FX_DivFx64c(numer, denom));
}

fx32 FX_Inv(fx32 denom)
{
    return FX_Div(FX32_ONE, denom);
}

fx64c FX_DivFx64c(fx32 numer, fx32 denom)
{
    if (denom == 0)
        return numer < 0 ? (fx64c)0x8000000000000000ULL : 0x7fffffffffffffffLL;
    return ((fx64c)numer * 4294967296LL) / denom;
}

fx64c FX_InvFx64c(fx32 denom)
{
    return FX_DivFx64c(FX32_ONE, denom);
}

s32 FX_ModS32(s32 a, s32 b)
{
    return b ? a % b : a;
}

/* FX_Sqrt 0x0202c52c: 64-bit SQRT of x << 32, then FX_GetSqrtResult rounds: (r + 0x200) >> 10. */
fx32 FX_Sqrt(fx32 x)
{
    if (x <= 0)
        return 0;
    u64 v = (u64)(u32)x << 32;
    u64 r = (u64)sqrt((double)v);
    while (r * r > v)
        r--;
    while ((r + 1) * (r + 1) <= v)
        r++;
    return (fx32)(((u32)r + 0x200) >> 10);
}

/* VEC_DotProduct 0x0202c6b8 and VEC_CrossProduct 0x0202c6f4: 64-bit products, rounded (+0x800) >> 12. */
fx32 VEC_DotProduct(const VecFx32 *a, const VecFx32 *b)
{
    return (fx32)(((s64)a->x * b->x + (s64)a->y * b->y + (s64)a->z * b->z + 0x800) >> FX32_SHIFT);
}

void VEC_CrossProduct(const VecFx32 *a, const VecFx32 *b, VecFx32 *out)
{
    fx32 x = (fx32)(((s64)a->y * b->z - (s64)a->z * b->y + 0x800) >> FX32_SHIFT);
    fx32 y = (fx32)(((s64)a->z * b->x - (s64)a->x * b->z + 0x800) >> FX32_SHIFT);
    fx32 z = (fx32)(((s64)a->x * b->y - (s64)a->y * b->x + 0x800) >> FX32_SHIFT);
    out->x = x;
    out->y = y;
    out->z = z;
}

/* MTX_RotY43_ 0x0202b278 */
void MTX_RotY43_(MtxFx43 *mtx, fx32 sinVal, fx32 cosVal)
{
    mtx->m[0][0] = cosVal; mtx->m[0][1] = 0;        mtx->m[0][2] = -sinVal;
    mtx->m[1][0] = 0;      mtx->m[1][1] = FX32_ONE; mtx->m[1][2] = 0;
    mtx->m[2][0] = sinVal; mtx->m[2][1] = 0;        mtx->m[2][2] = cosVal;
    mtx->m[3][0] = 0;      mtx->m[3][1] = 0;        mtx->m[3][2] = 0;
}

/* MTX_Inverse43 0x0202b294: cofactors c0..c2 of the first row are rounded, the others truncated, as in the SDK. */
static fx32 Sh12(s64 v) { return (fx32)(v >> FX32_SHIFT); }
static fx32 Rn12(s64 v) { return (fx32)((v + 0x800) >> FX32_SHIFT); }

int MTX_Inverse43(const MtxFx43 *src, MtxFx43 *dst)
{
    const fx32 *a = &src->m[0][0];
    fx32 c1 = Rn12((s64)a[3] * a[8] - (s64)a[5] * a[6]);
    fx32 c0 = Rn12((s64)a[4] * a[8] - (s64)a[5] * a[7]);
    fx32 c2 = Rn12((s64)a[3] * a[7] - (s64)a[4] * a[6]);
    fx32 det = Rn12((s64)a[0] * c0 - (s64)a[1] * c1 + (s64)c2 * a[2]);
    if (det == 0)
        return -1;
    fx32 inv = FX_Inv(det);
    fx32 o[12];
    o[0] = Sh12((s64)c0 * inv);
    o[1] = -Sh12((s64)Sh12((s64)a[1] * a[8] - (s64)a[7] * a[2]) * inv);
    o[2] = Sh12((s64)Sh12((s64)a[1] * a[5] - (s64)a[4] * a[2]) * inv);
    o[3] = -Sh12((s64)c1 * inv);
    o[4] = Sh12((s64)Sh12((s64)a[0] * a[8] - (s64)a[6] * a[2]) * inv);
    o[5] = -Sh12((s64)Sh12((s64)a[0] * a[5] - (s64)a[3] * a[2]) * inv);
    o[6] = Sh12((s64)c2 * inv);
    o[7] = -Sh12((s64)Sh12((s64)a[0] * a[7] - (s64)a[6] * a[1]) * inv);
    o[8] = Sh12((s64)Sh12((s64)a[0] * a[4] - (s64)a[3] * a[1]) * inv);
    o[9] = -Sh12((s64)a[11] * o[6] + (s64)a[9] * o[0] + (s64)o[3] * a[10]);
    o[10] = -Sh12((s64)a[11] * o[7] + (s64)a[9] * o[1] + (s64)o[4] * a[10]);
    o[11] = -Sh12((s64)a[11] * o[8] + (s64)a[9] * o[2] + (s64)o[5] * a[10]);
    for (int i = 0; i < 12; i++)
        (&dst->m[0][0])[i] = o[i];
    return 0;
}

/* MTX_Concat43 0x0202b620: ab = a * b (row vectors), sums of 64-bit products truncated, translation
 * a.t * b + b.t. ab may alias a or b. */
void MTX_Concat43(const MtxFx43 *a, const MtxFx43 *b, MtxFx43 *ab)
{
    MtxFx43 r;
    int i, j;
    for (i = 0; i < 4; i++)
        for (j = 0; j < 3; j++) {
            s64 s = (s64)a->m[i][0] * b->m[0][j] + (s64)a->m[i][1] * b->m[1][j] + (s64)a->m[i][2] * b->m[2][j];
            r.m[i][j] = (fx32)(s >> FX32_SHIFT) + (i == 3 ? b->m[3][j] : 0);
        }
    *ab = r;
}

/* FX_AcosIdx 0x0202ca9c: binary search of x among the cosines of the table (first or second quarter by the sign
 * of x, compared on 16 bits), index of the last probed entry * 16 + 8. */
u16 FX_AcosIdx(fx32 x)
{
    int lo, hi, mid = 0;
    if (x < 0) {
        lo = 0x400;
        hi = 0x800;
    } else {
        lo = 0;
        hi = 0x400;
    }
    while (lo <= hi) {
        s16 c;
        mid = (lo + hi) / 2;
        c = FX_SinCosTable_[mid * 2 + 1];
        if ((s16)x == c)
            break;
        if (c < (s16)x)
            hi = mid - 1;
        else
            lo = mid + 1;
    }
    return (u16)((mid * 2 + 1) << 3);
}

/* FX_AtanIdxTable_ (main 0x020b828c): atan(i / 128) as an angle index, i = 0..128. */
static const s16 FX_AtanIdxTable_[129] = {
    0, 81, 163, 244, 326, 407, 489, 570, 651, 732, 813, 894, 975, 1056, 1136, 1217,
    1297, 1377, 1457, 1537, 1617, 1696, 1775, 1854, 1933, 2012, 2090, 2168, 2246, 2324, 2401, 2478,
    2555, 2632, 2708, 2784, 2860, 2935, 3010, 3085, 3159, 3233, 3307, 3380, 3453, 3526, 3599, 3670,
    3742, 3813, 3884, 3955, 4025, 4095, 4164, 4233, 4302, 4370, 4438, 4505, 4572, 4639, 4705, 4771,
    4836, 4901, 4966, 5030, 5094, 5157, 5220, 5282, 5344, 5406, 5467, 5528, 5589, 5649, 5708, 5768,
    5826, 5885, 5943, 6000, 6058, 6114, 6171, 6227, 6282, 6337, 6392, 6446, 6500, 6554, 6607, 6660,
    6712, 6764, 6815, 6867, 6917, 6968, 7018, 7068, 7117, 7166, 7214, 7262, 7310, 7358, 7405, 7451,
    7498, 7544, 7589, 7635, 7679, 7724, 7768, 7812, 7856, 7899, 7942, 7984, 8026, 8068, 8110, 8151,
    8192};

/* FX_Atan2Idx 0x0202c8f0: the angle of (x, y) as a 16-bit index. The octant gives a base angle; the smaller
 * coordinate over the larger (hardware divider) indexes the table, added or subtracted (s16 arithmetic). */
u16 FX_Atan2Idx(fx32 y, fx32 x)
{
    s32 a, b;
    s16 base;
    int add;
    if (y > 0) {
        if (x > 0) {
            if (y < x) {
                base = 0, add = 1, a = y, b = x;
            } else if (y == x) {
                return 0x2000;
            } else {
                base = 0x4000, add = 0, a = x, b = y;
            }
        } else if (x == 0) {
            return 0x4000;
        } else if (-x < y) {
            base = 0x4000, add = 1, a = -x, b = y;
        } else if (-x == y) {
            return 0x6000;
        } else {
            base = (s16)-0x8000, add = 0, a = y, b = -x;
        }
    } else if (y == 0) {
        return x < 0 ? 0x8000 : 0;
    } else if (x < 0) {
        if (-x < -y) {
            base = -0x4000, add = 0, a = -x, b = -y;
        } else if (x == y) {
            return (u16)-0x6000;
        } else {
            base = (s16)-0x8000, add = 1, a = -y, b = -x;
        }
    } else if (x == 0) {
        return (u16)-0x4000;
    } else if (x < -y) {
        base = -0x4000, add = 1, a = x, b = -y;
    } else if (x == -y) {
        return (u16)-0x2000;
    } else {
        base = 0, add = 0, a = -y, b = x;
    }
    {
        s16 t = FX_AtanIdxTable_[FX_Div(a, b) >> 5];
        return (u16)(s16)(add ? base + t : base - t);
    }
}

/* MTX_RotX43_ 0x0202b258: rotation about x by (sin, cos), no translation. */
void MTX_RotX43_(MtxFx43 *mtx, fx32 sinVal, fx32 cosVal)
{
    mtx->m[0][0] = FX32_ONE; mtx->m[0][1] = 0;       mtx->m[0][2] = 0;
    mtx->m[1][0] = 0;        mtx->m[1][1] = cosVal;  mtx->m[1][2] = sinVal;
    mtx->m[2][0] = 0;        mtx->m[2][1] = -sinVal; mtx->m[2][2] = cosVal;
    mtx->m[3][0] = 0;        mtx->m[3][1] = 0;       mtx->m[3][2] = 0;
}
