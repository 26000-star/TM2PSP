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

// Rotation of angle (s = sin, c = cos) about a unit axis, translation cleared. Products of three fx32 are
// shifted by 24 and those of two by 12, all truncated.
// @port itcm:01ffa730 done
void dsMtx43_RotateAxis(MtxFx43 *m, const VecFx32 *axis, fx32 s, fx32 c)
{
    s64 x = axis->x, y = axis->y, z = axis->z, t = FX32_ONE - c;
    fx32 sx = (fx32)((s * x) >> 12), sy = (fx32)((s * y) >> 12), sz = (fx32)((s * z) >> 12);
    fx32 txy = (fx32)((t * x * y) >> 24), txz = (fx32)((t * x * z) >> 24), tyz = (fx32)((t * y * z) >> 24);
    m->m[0][0] = (fx32)((x * x * t) >> 24) + c;
    m->m[0][1] = txy + sz;
    m->m[0][2] = txz - sy;
    m->m[1][0] = txy - sz;
    m->m[1][1] = (fx32)((y * y * t) >> 24) + c;
    m->m[1][2] = tyz + sx;
    m->m[2][0] = txz + sy;
    m->m[2][1] = tyz - sx;
    m->m[2][2] = (fx32)((z * z * t) >> 24) + c;
    m->m[3][0] = m->m[3][1] = m->m[3][2] = 0;
}

// acos in fx32 radians: table index from FX_AcosIdx, wrapped above pi, times 2 pi / 65536.
// @port main:020b29b4 done
fx32 dsAcosFx(fx32 x)
{
    s32 idx = FX_AcosIdx(x);
    if (0x7fff < idx)
        idx += -65535;
    return (idx * 12868) >> 15;
}

// Orthonormal frame from a forward and an up hint: z = forward, x = up x z, y = z x x (crosses rounded, each
// axis normalised with the (0, 0, 1) fallback), translation pos.
// @port main:020584a4 done
void dsMtx43_FromAxes(MtxFx43 *m, const VecFx32 *pos, const VecFx32 *fwd, const VecFx32 *up)
{
    auto norm = [](VecFx32 &v) {
        if (v.x == 0 && v.y == 0 && v.z == 0)
            v = {0, 0, FX32_ONE};
        else
            dsVec_Normalize(&v, &v);
    };
    auto cross = [](const VecFx32 &a, const VecFx32 &b) {
        return VecFx32{(fx32)(((s64)a.y * b.z - (s64)a.z * b.y + 0x800) >> 12),
                       (fx32)(((s64)a.z * b.x - (s64)a.x * b.z + 0x800) >> 12),
                       (fx32)(((s64)a.x * b.y - (s64)a.y * b.x + 0x800) >> 12)};
    };
    VecFx32 z = *fwd;
    norm(z);
    VecFx32 x = cross(*up, z);
    norm(x);
    VecFx32 y = cross(z, x);
    m->m[0][0] = x.x; m->m[0][1] = x.y; m->m[0][2] = x.z;
    m->m[1][0] = y.x; m->m[1][1] = y.y; m->m[1][2] = y.z;
    m->m[2][0] = z.x; m->m[2][1] = z.y; m->m[2][2] = z.z;
    m->m[3][0] = pos->x; m->m[3][1] = pos->y; m->m[3][2] = pos->z;
}

// |a - b| with the square-root unit: (sqrt(4 d^2) + 1) >> 1, the differences on 32 bits.
// @port itcm:01ffa9d4 done
fx32 dsVec_Distance(const VecFx32 *a, const VecFx32 *b)
{
    VecFx32 d = {(fx32)((u32)a->x - (u32)b->x), (fx32)((u32)a->y - (u32)b->y), (fx32)((u32)a->z - (u32)b->z)};
    return dsVec_Mag(&d);
}

// The rotation rows are first divided by the largest row length s (FX_Inv of FX_Sqrt of the largest squared
// length), the adjugate of that matrix is divided by its determinant and multiplied by 1/s, and the translation
// is -t * inverse. Every product is rounded (+0x800 >> 12); both thresholds (s and |det| > 3) give the identity.
// @port main:02057db4 done
bool dsMtx43_InverseScaled(MtxFx43 *mtx)
{
    fx32 *m = &mtx->m[0][0];
    auto mul = [](fx32 a, fx32 b) { return (fx32)(((s64)a * b + 0x800) >> 12); };
    auto len2 = [&](int r) {
        return (fx32)(((s64)m[r + 2] * m[r + 2] + (s64)m[r] * m[r] + (s64)m[r + 1] * m[r + 1] + 0x800) >> 12);
    };
    fx32 l = len2(0);
    if (len2(3) > l)
        l = len2(3);
    if (len2(6) > l)
        l = len2(6);
    fx32 s = FX_Sqrt(l);
    if (s <= 3) {
        dsMtx43_Identity(mtx);
        return false;
    }
    fx32 inv = FX_Inv(s);
    for (int i = 0; i < 9; i++)
        m[i] = mul(m[i], inv);
    fx32 c[12];
    c[0] = mul(m[4], m[8]) - mul(m[7], m[5]);
    c[3] = mul(m[5], m[6]) - mul(m[3], m[8]);
    c[6] = mul(m[3], m[7]) - mul(m[6], m[4]);
    fx32 det = mul(m[2], c[6]) + mul(m[0], c[0]) + mul(m[1], c[3]);
    if ((det < 0 ? -det : det) <= 3) {
        dsMtx43_Identity(mtx);
        return false;
    }
    c[1] = mul(m[7], m[2]) - mul(m[1], m[8]);
    c[4] = mul(m[0], m[8]) - mul(m[6], m[2]);
    c[7] = mul(m[6], m[1]) - mul(m[0], m[7]);
    c[2] = mul(m[1], m[5]) - mul(m[4], m[2]);
    c[5] = mul(m[3], m[2]) - mul(m[0], m[5]);
    c[8] = mul(m[0], m[4]) - mul(m[3], m[1]);
    fx32 k = det != 0 ? FX_Div(inv, det) : 0;
    for (int i = 0; i < 9; i++)
        c[i] = mul(c[i], k);
    for (int j = 0; j < 3; j++)
        c[9 + j] = -(mul(c[j], m[9]) + mul(c[3 + j], m[10]) + mul(c[6 + j], m[11]));
    for (int i = 0; i < 12; i++)
        m[i] = c[i];
    return true;
}
