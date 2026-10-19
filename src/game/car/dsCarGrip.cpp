// dsCarDynamics tyre grip (ov003 0x02129f04, 0x0212a848): gravity, lateral grip, skid drag, brakes, rolling
// resistance, air slowdown.
#include "car/dsCarDynamics.h"

#include "car/dsCarMath.h"
#include "core/dsTime.h"
#include "math/dsMath.h"

using namespace carmath;

static const fx32 ROOF_NORMAL_Y = 3277;            // ov003 0x0212a844
static const fx32 COS_PI = -4096;                  // ov003 0x02137934 (cos(pi) from the table, __sinit 0x021354dc)

// Mean grip of the grounded wheels by surface, raised towards GripOversteer with the skid, plus GripInc per
// unit of speed; above 500 it is pulled back by the donut blend.
// @port ov003:0212a848 done
fx32 dsCarDynamics::GripLimit()
{
    fx32 sum = 0;
    s32 n = 0;
    for (int i = 0; i < 4; i++) {
        const dsCarWheel *w = wheels[i];
        if (normalForces || !w->grounded)
            continue;
        n++;
        switch (w->surface) {
        case 1:
            sum += p.GripGrass;
            break;
        case 2:
        case 3:
            sum += p.GripGravel;
            break;
        case 5:
            sum += p.GripDirt;
            break;
        case 13:
            sum += p.GripSnow;
            break;
        default:
            sum += p.Grip;
            break;
        }
    }
    fx32 g = sum;
    if (n != 0)
        g = sum / n;
    if (p.GripOversteer < g)
        g += Mul(p.GripOversteer - g, skid);
    fx32 a = speed < 0 ? -speed : speed;
    fx32 r = Mul(g + Mul(a, p.GripInc), s_gripScale);
    if (0x1f4000 < r)
        return r + Mul(0x1f4000 - r, s_donutBlend);
    return r;
}

static inline fx32 ClampSym(fx32 v, fx32 lim)
{
    // -v limited to [-lim, lim]
    if (v < -lim)
        return lim;
    if (lim <= v)
        return -lim;
    return -v;
}

// @port ov003:02129f04 done
void dsCarDynamics::Grip()
{
    velocity = Add(velocity, Scale(gravityDir, Mul(gravity, dt)));
    VecFx32 right = Project(groundNormal, Row(matrix, 0));
    lateralSpeed = Dot(velocity, right);
    normalForces = 0 < Dot(gravityDir, Row(matrix, 1));
    VecFx32 vdir = NormalizeSafe(velocity);
    if (0 < groundedWheels) {
        // Lateral grip: cancel the sideways speed up to grip * dt.
        fx32 u = ClampSym(lateralSpeed, Mul(GripLimit(), dt));
        velocity = Add(velocity, Scale(right, u));
        VecFx32 dd = NormalizeSafe(s_driveDir);
        fx32 c = Dot(dd, right);
        if (c < 0)
            c = -c;
        fx32 sk = p.d_SkidSpeedEffect + Mul(p.d_SkidSpeedEffect2 - p.d_SkidSpeedEffect, skid);
        fx32 au = u < 0 ? -u : u;
        fx32 drag = 0, boost = 0;
        if (sk < 1)
            drag = Mul(Mul(au, c), -sk);
        else
            boost = Mul(Mul(au, c), Mul(engine->throttle, sk));
        VecFx32 q = Project(right, s_driveDir);
        velocity = Add(velocity, Scale(q, -Clamp01(drag)));

        // Brakes, handbrake and grass friction against the forward speed.
        fx32 brk = 0;
        if (0 < engine->brake)
            brk = Mul(engine->brake, p.BrakeFriction);
        if (0 < unk400)
            brk += Mul(unk400, p.HandbrakeFrict);
        fx32 afs = forwardSpeed < 0 ? -forwardSpeed : forwardSpeed;
        fx32 grass = surfaceCount[1] * Mul(p.GrassFriction, afs);
        fx32 dec = Mul(brk + grass / 4, dt);
        fx32 k = ClampSym(forwardSpeed, dec) + boost;
        velocity = Add(velocity, Scale(driveDir, k));

        // Rolling resistance when off the throttle.
        fx32 thr = engine->throttle;
        if (thr < FX32_ONE) {
            VecFx32 nv = NormalizeSafe(Neg(vdir));
            fx32 sl = Mul(Mul(p.GroundSlowdown + Mul(speed, p.GroundSlowInc), FX32_ONE - thr), dt);
            if (noThrottle)
                sl = Mul(sl, 0x2b8);
            VecFx32 dv = Scale(nv, sl);
            velocity = Add(velocity, dv);
            if (0 < Dot(dv, velocity))
                velocity = {};
        }
    }
    bool ground = (0 < bodyHitCount && ROOF_NORMAL_Y < hitNormalSum.y) || 0 < groundedWheels;
    if (!ground && throttle < 0x800) {
        fx32 k = Clamp01(FX32_ONE - Dot(vdir, Row(matrix, 2)));
        if (k != 0) {
            k = FX32_ONE - Mul(Mul(dt, p.AirSlowdown), k);
            velocity.x = Mul(velocity.x, k);
            velocity.z = Mul(velocity.z, k);
        }
    }
    fx32 d = dt * -6 + FX32_ONE;
    push = Scale(push, d);
    wallVec.y = Mul(wallVec.y, d);
}

// Planar velocity, booster gravity, engine update, drive direction (forward along the front wheels' ground),
// drive force.
// @port ov003:0212a9c0 done
void dsCarDynamics::Drive()
{
    gravityDir = {0, (fx32)0xfffff001, 0};
    s_driveDir = Project(groundNormal, velocity);
    if (turboTimer != 0) {
        // Boosters: gravity turns towards the ground normal as the speed reaches MagicGripSpeed.
        fx32 v = dsVec_Mag(&s_driveDir);
        fx32 t = p.d_MagicGripSpeed != 0 ? FX_Div(v, p.d_MagicGripSpeed) : 0;
        if (t < 0)
            t = -t;
        if (FX32_ONE <= t)
            t = FX32_ONE;
        fx32 k = Mul(t, t);
        gravityDir.x += (s32)((u32)k * (u32)(-(groundNormal.x + gravityDir.x))) >> 12;
        gravityDir.y += (s32)((u32)k * (u32)(-(groundNormal.y + gravityDir.y))) >> 12;
        gravityDir.z += (s32)((u32)k * (u32)(-(groundNormal.z + gravityDir.z))) >> 12;
    }
    normalForces = 0 < Dot(gravityDir, Row(matrix, 1));
    fx32 c = -Dot(groundNormal, gravityDir);
    if (c <= 0)
        c = 0;
    s_gripScale = Mul(c, c);

    VecFx32 sn = {};
    s32 n = 0;
    for (int i = 0; i < 2; i++) {
        const dsCarWheel *w = wheels[i];
        if (w->grounded) {
            sn = Add(sn, w->contactNormal);
            n++;
        }
    }
    engine->Update(forwardSpeed, throttle, input.unk41c);
    if (n == 0) {
        driveDir = {};
        driveForce = 0;
        return;
    }
    driveForce = engine->force;
    sn = ScaleFx64c(sn, FX_InvFx64c(n << 12));
    driveDir = Project(sn, Row(matrix, 2));
    if (sn.y < COS_PI) {
        VecFx32 h = NormalizeSafe(Project(sn, {0, FX32_ONE, 0}));
        fx32 d = Dot(h, velocity);
        if (0 < d) {
            fx32 k = Mul(d, dt) * 10;
            velocity = Add(velocity, Scale(Neg(h), k));
        }
        d = Dot(h, driveDir);
        if (0 < d)
            driveDir = Sub(driveDir, Scale(h, d));
    }
    fx32 f = Mul(driveForce, dt);
    if (turboTimer != 0)
        f = Mul(f, s_gripScale);
    driveDir = NormalizeSafe(driveDir);
    if (bodyHitCount == 0)
        velocity = Add(velocity, Scale(driveDir, f));
}
