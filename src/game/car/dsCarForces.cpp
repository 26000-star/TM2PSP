// dsCarDynamics forces (ov003 0x02127f1c): suspension springs and damping, self-righting, ramp pitch, angular
// damping per axis.
#include "car/dsCarDynamics.h"

#include "car/dsCarMath.h"
#include "core/dsTime.h"
#include "math/dsMath.h"

using namespace carmath;

static const fx32 COMPRESSED_MIN = -1023;          // ov003 0x02128940
static const fx32 ANGDAMP_UNSET = (fx32)0xffc19001; // ov003 0x02128970
static const fx32 LANDING_TIME = 3277;             // ov003 0x02128974 (0.8 s)
static const fx32 TILT_UP_MIN = 3891;              // ov003 0x02128978 (0.95)
static const fx32 RAMP_ANGLE = 714;                // ov003 0x02128968 (10 degrees in radians)

// @port ov003:02127f1c done
void dsCarDynamics::Forces()
{
    // Car-space rotation (the inverse's translation is cleared).
    MtxFx43 inv = matrix;
    MTX_Inverse43(&inv, &inv);
    inv.m[3][0] = inv.m[3][1] = inv.m[3][2] = 0;
    VecFx32 al = angularVelocity;
    dsVec_MulMtx43(&al, &inv, &al);

    VecFx32 up = Row(matrix, 1);
    VecFx32 force = {}, torque = {};
    s32 compressed = 0;
    for (size_t i = 0; i < wheels.size(); i++) {
        const dsCarWheel *w = wheels[i];
        if (!w->grounded)
            continue;
        fx32 c = w->target;
        if (COMPRESSED_MIN < c)
            compressed++;
        if (c < 0)
            continue;
        VecFx32 n = w->contactNormal;
        if (Dot(up, n) < 0)
            continue;
        VecFx32 f;
        if (normalForces) {
            f = Scale(n, c);
        } else if (p.d_FlatAngCos < n.y) {
            f = {0, c, 0};
        } else {
            if (turboTimer != 0) {
                fx32 k = p.SlopeClimb;
                n.x += Mul32(k, up.x - n.x);
                n.y += Mul32(k, up.y - n.y);
                n.z += Mul32(k, up.z - n.z);
            }
            f = Scale(n, c);
        }
        force = Add(force, Scale(f, p.SuspLinearForce));
        fx32 kz = p.d_SuspAngForce;
        if (groundNormal.y < FX32_ONE) {
            fx32 t = Clamp01(FX_Div(FX32_ONE - groundNormal.y, 0x29));
            kz += Mul(0x1e000 - kz, t);
        }
        VecFx32 r = {Mul(w->probe.x - bodyCentre.x / 3, p.SuspAngForce), w->probe.y, Mul(w->probe.z, kz)};
        dsVec_MulMtx33(&r, &matrix, &r);
        torque = Add(torque, Cross(f, r));
    }

    // Damping along the ground normal, in proportion to the spring force.
    VecFx32 gn = groundNormal;
    if (turboTimer != 0) {
        fx32 k = p.SlopeClimb;
        gn.x += Mul32(k, up.x - gn.x);
        gn.y += Mul32(k, up.y - gn.y);
        gn.z += Mul32(k, up.z - gn.z);
    }
    fx32 fn = Dot(gn, force);
    if (fn < 0)
        fn = -fn;
    fx32 kd = Clamp01(Mul(Mul(fn, p.SuspLinearDamp), dt));
    fx32 j = -Mul(Dot(gn, velocity), kd);
    velocity = Add(velocity, Scale(gn, j));

    if (stuck) {
        angularVelocity.x += Mul(-up.z, -up.y);
        angularVelocity.z += Mul(Mul(up.x, FX32_ONE), -up.y);
        angularVelocity.y -= Mul(Mul(angularVelocity.y, g_dsFrameTime), 0x800);
    }
    if (0 < p.SelfRighting) {
        VecFx32 r = {Mul(Mul(up.z, -FX32_ONE), p.SelfRighting), 0, Mul(Mul(up.x, FX32_ONE), p.SelfRighting)};
        torque.x -= r.x;
        torque.z -= r.z;
        fx32 k = Clamp01(Mul(dsVec_Mag(&r), dt) << 2);
        angularVelocity = Scale(angularVelocity, FX32_ONE - k);
    }

    // Front wheels off the ground, rear down (the original tests wheel 2 twice): pitch the nose down for a while
    // in proportion to the slope.
    if (!wheels[0]->grounded && !wheels[1]->grounded && wheels[2]->grounded) {
        static const fx32 s_sin0 = dsSin(0);
        static const fx32 s_sinRamp = dsSin(RAMP_ANGLE);
        VecFx32 f = Project(groundNormal, Row(matrix, 2));
        fx32 t = s_sinRamp == 0 ? 0 : FX_Div(f.y - s_sin0, s_sinRamp);
        rampPitch = Mul(p.RampPitchTime, Clamp01(t));
    }
    if (rampPitch != 0) {
        fx32 t = p.RampPitchTime == 0 ? 0 : FX_Div(rampPitch, p.RampPitchTime);
        fx32 k = Mul(p.RampPitchForce, t);
        torque = Sub(torque, Scale(Row(matrix, 0), k));
        rampPitch -= dt;
        if (rampPitch < 0)
            rampPitch = 0;
    }

    velocity.x += Mul(dt, force.x);
    velocity.y += Mul(dt, force.y);
    velocity.z += Mul(dt, force.z);
    angularVelocity = Add(angularVelocity, Scale(torque, dt));
    VecFx32 tl = torque;
    dsVec_MulMtx43(&tl, &inv, &tl);

    // Angular damping: air value, towards the ground values with the wheels down.
    if (angDamp[0] == ANGDAMP_UNSET)
        angDamp[0] = angDamp[1] = angDamp[2] = p.AngDampAir;
    fx32 tilt = Mul((compressed << 12) / 4 - FX32_ONE, p.TiltSkidFactor) + FX32_ONE;
    angDamp[1] = p.AngDampAir + Mul(grip - p.AngDampAir, tilt);
    if (groundTime < LANDING_TIME && 0 < groundedWheels)
        angDamp[1] = grip + Mul(angDamp[1] - grip, FX_Div(groundTime, LANDING_TIME));
    fx32 kxz = 0;
    if (velocity.y < -0x1e000 && groundedWheels < 1) {
        fx32 t = Clamp01(FX_Div(-(velocity.y + 0x1e000), 0x1e000));
        fx32 u = Mul(t, 0x400);
        if (landTimer <= u)
            landTimer = u;
    }
    if (landTimer != 0) {
        landTimer -= dt;
        if (landTimer < 0)
            landTimer = 0;
        if (0 < groundedWheels)
            kxz = 0x800;
    }
    if (2 < compressed && up.y < TILT_UP_MIN) {
        fx32 t1 = Clamp01(FX_Div(up.y - TILT_UP_MIN, -205));
        fx32 t2 = Clamp01((forwardSpeed - 0x46000) / 20);
        kxz += Mul(FX32_ONE - kxz, Mul(t1, t2));
    }
    if (stuck && kxz < 0x801)
        kxz = 0x800;
    // Torque opposing the spin on an axis: damp harder (sqrt of the torque / 2).
    fx32 kx = kxz;
    if (Mul(al.x, tl.x) < 0) {
        fx32 a = tl.x < 0 ? -tl.x : tl.x;
        kx = Clamp01(FX_Div(FX_Sqrt(a), 0x2000));
        if (kx < kxz)
            kx = kxz;
    }
    fx32 target = p.AngDampAir + Mul(p.AngDampGroundXZ - p.AngDampAir, kx);
    if (target < angDamp[0])
        dsApproach(&angDamp[0], target, p.AngDampRelease);
    else
        angDamp[0] = target;
    if (Mul(al.z, tl.z) < 0) {
        fx32 a = tl.z < 0 ? -tl.z : tl.z;
        fx32 k = Clamp01(FX_Div(FX_Sqrt(a), 0x2000));
        if (kxz <= k)
            kxz = k;
    }
    target = p.AngDampAir + Mul(p.AngDampGroundXZ - p.AngDampAir, kxz);
    if (target < angDamp[2])
        dsApproach(&angDamp[2], target, p.AngDampRelease);
    else
        angDamp[2] = target;
    VecFx32 d = {-Mul(angDamp[0], al.x), -Mul(al.y, angDamp[1]), -Mul(angDamp[2], al.z)};
    dsVec_MulMtx33(&d, &matrix, &d);
    angularVelocity = Add(angularVelocity, Scale(d, dt));
}
