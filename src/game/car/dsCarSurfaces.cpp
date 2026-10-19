// dsCarDynamics surfaces (ov003 0x02122b54): materials under the wheels (rough ground, grass, boosters, turbo
// pads), grip, sea level.
#include "car/dsCarDynamics.h"

#include "car/dsCarMath.h"
#include "core/dsTime.h"
#include "math/dsMath.h"

using namespace carmath;

static const fx32 ROUGH_JOLT = 410;                // ov003 0x021233b8 (0.1 per rough wheel)
static const fx32 SKIM_SPEED = 128170;             // ov003 0x0213792c (31.3, __sinit 0x021354dc)
static const fx32 SKIM_BOUNCE = 3277;              // ov003 0x021233c4 (0.8)

// Rough surfaces: grass (1), gravel (2) and 4 (main 0x020ad268).
static inline bool IsRough(s32 m)
{
    return (u32)(m - 1) <= 3 && ((1u << (m - 1)) & 0xb) != 0;
}

// @port ov003:02122b54 done (the booster sound flag of the owner vehicle, vtable +0x30, is left out)
void dsCarDynamics::Surfaces()
{
    surfaceCount[0] = surfaceCount[1] = 0;
    bool boosted = false;
    for (int i = 0; i < 4; i++) {
        const dsCarWheel *w = wheels[i];
        s32 m = w->surface;
        if (IsRough(m))
            surfaceCount[0]++;
        if (m == 1)
            surfaceCount[1]++;
        if (m == 8) {
            noThrottle = true;
        } else if ((u32)(m - 9) < 2) {
            turboTimer = FX32_ONE;
            boosterAlign = true;
            if (-0x199 < matrix.m[1][1])
                alignTime = 0x400;
        } else if (!boosted && m == 7 && !(warmingUp && *warmingUp) && pieceRotationAt) {
            int rot = pieceRotationAt(w->contactPoint);
            if (rot < 0) {
                VecFx32 q = {w->contactPoint.x, w->contactPoint.y - 0x2000, w->contactPoint.z};
                rot = pieceRotationAt(q);
            }
            if (0 <= rot) {
                VecFx32 d = {};
                switch (rot) {
                case 0: d = {0, 0, FX32_ONE}; break;
                case 1: d = {FX32_ONE, 0, 0}; break;
                case 2: d = {0, 0, (fx32)0xfffff001}; break;
                case 3: d = {(fx32)0xfffff001, 0, 0}; break;
                }
                fx32 k = Mul(p.SpeedBoost, dt);
                velocity.x += Mul(d.x, k);
                velocity.y += Mul(d.y, k);
                velocity.z += Mul(d.z, k);
                boosted = true;
            }
        }
    }
    if (turboTimer != 0) {
        turboTimer += dt * -5;
        if (turboTimer < 0)
            turboTimer = 0;
    }
    if (0 < groundedWheels && turboTimer == 0)
        boosterAlign = false;
    if (turboTimer == 0 && matrix.m[1][1] < -0x199)
        boosterAlign = false;

    // Rough ground: random jolts every unit of distance.
    if (surfaceCount[0] != 0) {
        fx32 afs = forwardSpeed < 0 ? -forwardSpeed : forwardSpeed;
        roughTime += Mul(afs, dt);
        if (FX32_ONE < roughTime) {
            s32 a = surfaceCount[0] * ROUGH_JOLT;
            angularVelocity.x += random.Range(-a, a);
            angularVelocity.z += random.Range(-a, a);
            roughTime = 0;
        }
    }
    grip = p.AngDampGroundY + Mul(p.d_AngDampGroundY - p.AngDampGroundY, (surfaceCount[0] << 12) / 4);
    grip += Mul(Mul(grip, FX32_ONE) - grip, s_donutBlend);

    // After a turbo pad, in the air: velocity and spin kept in the plane of the last ground.
    if (alignTime != 0) {
        if (groundedWheels < 1) {
            VecFx32 n = groundNormal;
            if ((n.x < 0 ? -n.x : n.x) < ROUGH_JOLT)
                n.x = 0;
            if ((n.y < 0 ? -n.y : n.y) < ROUGH_JOLT)
                n.y = 0;
            if ((n.z < 0 ? -n.z : n.z) < ROUGH_JOLT)
                n.z = 0;
            n = NormalizeSafe(n);
            velocity = Project(n, velocity);
            angularVelocity = Sub(angularVelocity, Project(n, angularVelocity));
        }
        alignTime -= dt;
        if (alignTime < 0)
            alignTime = 0;
    }

    // Below the sea level: skim when fast and falling, otherwise sink and slow down with the depth.
    if (minHeight <= matrix.m[3][1])
        return;
    VecFx32 h = {velocity.x, 0, velocity.z};
    fx32 hs = dsVec_Mag(&h);
    if (SKIM_SPEED < hs && velocity.y < 0) {
        velocity.x = Mul(velocity.x, SKIM_BOUNCE);
        velocity.y = -Mul(velocity.y, SKIM_BOUNCE);
        velocity.z = Mul(velocity.z, SKIM_BOUNCE);
        angularVelocity.x += Mul(matrix.m[1][2], 0x4000);
        angularVelocity.z += Mul(matrix.m[1][0], -0x4000);
        angularVelocity.x += random.Range(-FX32_ONE, FX32_ONE);
        angularVelocity.z += random.Range(-FX32_ONE, FX32_ONE);
        return;
    }
    fx32 d = Clamp01((minHeight - matrix.m[3][1]) / 6);
    fx32 k = FX32_ONE - Mul(Mul(Mul(d, d), 0xc000), g_dsFrameTime);
    velocity = Scale(velocity, k);
}
