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
