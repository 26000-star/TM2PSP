#include "dsSeaBackdrop.h"

#include "track/dsRaceSetup.h"

// @port ov003:0213079c done (with the fields set by ov003 0x0211de20: speed for Island and Coast, height and scale)
void dsSeaBackdrop::Init(s32 environment, const VecFx32 &color4)
{
    bool sea = environment == ENV_ISLAND || environment == ENV_COAST;
    speed = sea ? 0x11f : 0;     // ov003 0x0211e170
    phase = 0;
    height = environment == ENV_COAST ? 0x2800 : environment == ENV_ISLAND ? -16219 : 0; // 0x0211e17c, 0x0211e180
    scale = sea ? 2 : 1;
    color = color4;              // race environment +0x44..+0x4c (ov003 0x02130894)
}

// @port itcm:01fff8cc done (the geometry engine commands become vertices; MTX_POP/PUSH: world space)
void dsSeaBackdrop::Quads(fx32 eyeY, Vertex *out) const
{
    // The sea goes down as the camera rises: by a quarter of the height above 2.4, at most 2 units.
    s32 lift = (eyeY - 0x2666) >> 2;
    if (lift < 0)
        lift = 0;
    if (lift > 0x2000)
        lift = 0x2000;
    fx32 ty = 0xf9a - lift + height;
    s32 sxz = scale * 0x10002;   // MTX_SCALE(sxz, 1, sxz), then MTX_TRANS(tx, ty, tz): world = (v + t) x scale
    // TEXCOORD keeps 16 bits: a = (phase + 0x900000) >> 8, b = phase >> 8; across a quad the coordinates run
    // from a to b, 14 repeats of the texture (18 the other way once only one of them has wrapped).
    s16 a = (s16)(u16)((u32)(phase + 0x900000) >> 8), b = (s16)(u16)((u32)phase >> 8);
    // VTX_16 words 0xf0ce8002 / 0x7ffe, 0xf0ce8002 / 0x8002, 0xf0ce7ffe / 0x8002, 0xf0ce7ffe / 0x7ffe.
    static const s16 vx[4] = {-0x7ffe, -0x7ffe, 0x7ffe, 0x7ffe}, vz[4] = {0x7ffe, -0x7ffe, -0x7ffe, 0x7ffe};
    const s16 vy = -0xf32;
    const s16 ts[4] = {a, a, b, b}, tt[4] = {b, a, a, b};
    const double k = 1.0 / (4096.0 * 4096.0);
    fx32 tx = -0xc000;
    for (int i = 0; i < 5; i++, tx += 0xfffc) {
        fx32 tz = -0xc000;
        for (int j = 0; j < 5; j++, tz += 0xfffc) {
            for (int c = 0; c < 4; c++, out++) {
                out->pos[0] = (float)((double)(vx[c] + tx) * sxz * k);
                out->pos[1] = (float)((vy + ty) / 4096.0);
                out->pos[2] = (float)((double)(vz[c] + tz) * sxz * k);
                out->s = ts[c];
                out->t = tt[c];
            }
        }
    }
}

u16 dsSeaBackdrop::Color() const
{
    // 0x3e0f83e1 x c >> 37, rounded towards zero: c / 132 (fx32 1.0 -> 31).
    return (u16)((color.x / 132 | (color.y / 132) << 5 | (color.z / 132) << 10) & 0x7fff);
}
