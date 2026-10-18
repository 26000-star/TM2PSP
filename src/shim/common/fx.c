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

fx32 FX_Div(fx32 numer, fx32 denom)
{
    if (denom == 0)
        return numer < 0 ? (fx32)0x80000000 : 0x7fffffff;
    return (fx32)(((s64)numer << FX32_SHIFT) / denom);
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

fx32 FX_Sqrt(fx32 x)
{
    if (x <= 0)
        return 0;
    /* hardware SQRT of the 64-bit value x << 12, truncated */
    u64 v = (u64)x << FX32_SHIFT;
    u64 r = (u64)sqrt((double)v);
    while (r * r > v)
        r--;
    while ((r + 1) * (r + 1) <= v)
        r++;
    return (fx32)r;
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
