#include "dsMath.h"

#include <string.h>

// @port itcm:01ffa6ec done
void dsMtx43_Copy(const MtxFx43 *src, MtxFx43 *dst)
{
    memcpy(dst, src, sizeof *dst);
}

// @port itcm:01ffa708 done
void dsMtx43_Identity(MtxFx43 *m)
{
    memset(m, 0, sizeof *m);
    m->m[0][0] = m->m[1][1] = m->m[2][2] = FX32_ONE;
}

// v * M (rows 0..2), each sum of 64-bit products shifted right by 12 without rounding.
// @port itcm:01ffa8a4 done
void dsVec_MulMtx33(const VecFx32 *v, const MtxFx43 *m, VecFx32 *out)
{
    s64 x = v->x, y = v->y, z = v->z;
    fx32 rx = (fx32)((x * m->m[0][0] + y * m->m[1][0] + z * m->m[2][0]) >> FX32_SHIFT);
    fx32 ry = (fx32)((x * m->m[0][1] + y * m->m[1][1] + z * m->m[2][1]) >> FX32_SHIFT);
    fx32 rz = (fx32)((x * m->m[0][2] + y * m->m[1][2] + z * m->m[2][2]) >> FX32_SHIFT);
    out->x = rx;
    out->y = ry;
    out->z = rz;
}

// Hardware SQRT in 64-bit mode: floor of the square root.
static u32 Sqrt64(u64 v)
{
    if (v == 0)
        return 0;
    u64 r = (u64)__builtin_sqrt((double)v);
    while (r * r > v)
        r--;
    while ((r + 1) * (r + 1) <= v)
        r++;
    return (u32)r;
}

// sqrt(4 * |v|^2) rounded to the nearest half: (sqrt + 1) >> 1.
// @port itcm:01ffaa58 done
fx32 dsVec_Mag(const VecFx32 *v)
{
    s64 t = (s64)v->x * v->x + (s64)v->y * v->y + (s64)v->z * v->z;
    return (fx32)((Sqrt64((u64)(t * 4)) + 1) >> 1);
}

// k = sqrt(4t) * (2^56 / t) ~ 2^57 / |v| (64/64 divider and 64-bit sqrt started together), then
// out = ((v * k) >> 32 + 0x1000) >> 13, the product taken modulo 2^64 like the ARM code.
// @port itcm:01ffaac0 done
void dsVec_Normalize(const VecFx32 *v, VecFx32 *out)
{
    s64 t = (s64)v->x * v->x + (s64)v->y * v->y + (s64)v->z * v->z;
    if (t == 0) { // the divider returns all ones on a zero denominator; callers never pass a null vector
        out->x = out->y = out->z = 0;
        return;
    }
    s64 q = (s64)(((u64)1 << 56) / (u64)t);
    s64 k = (s64)((u64)(s64)(s32)Sqrt64((u64)(t * 4)) * (u64)q);
    s32 c[3] = {v->x, v->y, v->z};
    s32 r[3];
    for (int i = 0; i < 3; i++) {
        s32 hi = (s32)(((u64)(s64)c[i] * (u64)k) >> 32);
        r[i] = (hi + 0x1000) >> 13;
    }
    out->x = r[0];
    out->y = r[1];
    out->z = r[2];
}
