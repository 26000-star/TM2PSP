// dsCarDynamics integration (ov003 0x02129098, 0x02129ce4): steering yaw, air control, visual roll/pitch,
// tilt, rotation about the body centre, position.
#include "car/dsCarDynamics.h"

#include "car/dsCarMath.h"
#include "core/dsTime.h"
#include "math/dsMath.h"

using namespace carmath;

static const fx32 RESTING_SPEED2 = 0xcd;           // |v + push|^2 < 0.05
static const fx32 ROOF_NORMAL_Y = 3277;            // ov003 0x02129cc8
static const fx32 PITCH_FADE = 0x666;              // ov003 0x02129ccc
static const fx32 HALF_PI = (fx32)0xffffe6de;      // ov003 0x02129cd0 (-pi/2)
static const fx32 WORLD_MIN = (fx32)0xfffd0001;    // ov003 0x02129cdc
static const fx32 WORLD_MAX = 0x290000;
static const fx32 WORLD_BOUNCE = 3973;             // ov003 0x02129ce0

// @port ov003:02129ce4 done
void dsCarDynamics::Rotate(const VecFx32 &w, const VecFx32 &pivot, MtxFx43 &m)
{
    VecFx32 axis = NormalizeSafe(w);
    fx32 a = Mul(-dsVec_Mag(&w), dt);
    MtxFx43 r;
    dsMtx43_RotateAxis(&r, &axis, dsSin(a), dsCos(a));
    VecFx32 c = pivot;
    dsVec_MulMtx43(&c, &m, &c);
    m.m[3][0] -= c.x;
    m.m[3][1] -= c.y;
    m.m[3][2] -= c.z;
    MtxFx43 t = m;
    MTX_Concat43(&t, &r, &t);
    dsMtx43_Copy(&t, &m);
    m.m[3][0] += c.x;
    m.m[3][1] += c.y;
    m.m[3][2] += c.z;
}

// @port ov003:02129098 done
void dsCarDynamics::Integrate()
{
    VecFx32 vp = Add(velocity, push);
    fx32 v2 = (fx32)(((s64)vp.x * vp.x + (s64)vp.y * vp.y + (s64)vp.z * vp.z + 0x800) >> 12);
    s_resting = v2 < RESTING_SPEED2 && skid == 0;
    VecFx32 right = Row(matrix, 0), up = Row(matrix, 1), fwd = Row(matrix, 2);
    fx32 roll = 0, pitch = 0;
    if (groundedWheels < 1 || maxSpeed < 1 || s_resting) {
        bool ground = (0 < bodyHitCount && ROOF_NORMAL_Y < hitNormalSum.y) || 0 < groundedWheels;
        if (!ground && -0x199 < up.y) {
            // Air control: steering yaws, the pitch input lifts the nose (less when already pointing up).
            fx32 a = Mul(Mul(steer, dt), p.AirControlSteer);
            angularVelocity = Sub(angularVelocity, Scale(up, a));
            if (0 < unk3a4) {
                fx32 k = FX32_ONE;
                if (0 < fwd.y)
                    k = Clamp01(FX32_ONE - FX_Div(fwd.y, PITCH_FADE));
                fx32 b = Mul(Mul(unk3a4, dt), Mul(p.AirControlPitch, k));
                angularVelocity = Add(angularVelocity, Scale(right, b));
            }
            if (boosterAlign)
                angularVelocity = Sub(angularVelocity, Project(groundNormal, angularVelocity));
        }
    } else {
        // Yaw rate = Steerability * speed / (|speed| + 4), firmed up with the speed.
        fx32 fs = forwardSpeed, afs = fs < 0 ? -fs : fs;
        fx32 rate = afs + 0x4000 != 0 ? FX_Div(Mul(p.Steerability, fs), afs + 0x4000) : 0;
        fx32 inv = FX_Inv(Mul(afs, p.d_SteerFirmUp100) + FX32_ONE);
        s_steerScale = inv;
        fx32 yawRate = Mul(Mul(rate, inv), steer);
        angularVelocity = Sub(angularVelocity, Scale(up, Mul(yawRate, dt)));
        if (p.unk_058 != 0) {
            fx32 k = p.d_FakePitchReduce == 0 ? 0 : FX_Div(afs, p.d_FakePitchReduce);
            k = FX_Inv(k + FX32_ONE);
            pitch -= Mul(Mul(throttle, p.unk_058), k);
        }
        if (p.d_FakeRollFactor100 != 0)
            roll = yawRate;
    }
    fakeRoll += Mul(roll - fakeRoll, dt * 5);
    fakeRollOut = Mul(fakeRoll, p.d_FakeRollFactor100);
    fakePitch += Mul(pitch - fakePitch, dt * 5);

    // Body tilt in turns (Tilt), limited by the ground's slope against TiltLimit degrees per unit of speed.
    int front = wheels[0]->grounded + wheels[1]->grounded, rear = wheels[2]->grounded + wheels[3]->grounded;
    if (front != 0 && rear != 0) {
        fx32 a = Mul(p.d_Tilt100, fakeRoll);
        fx32 fs = forwardSpeed < 0 ? -forwardSpeed : forwardSpeed;
        if (0x3bfff < fs)
            fs = 0x3c000;
        fx32 t = FX_Div(Mul(fs, Mul(fs, fs)), 0x790000);
        fx32 b = Mul(a, t);
        fx32 ang = dsAcosFx(Dot(groundNormal, right)) + HALF_PI;
        fx32 afs = forwardSpeed < 0 ? -forwardSpeed : forwardSpeed;
        fx32 lim = (fx32)((s64)12868 * Mul(p.TiltLimit, afs) / 0xb4000);
        if (Mul(b, ang) < 0) {
            fx32 aa = ang < 0 ? -ang : ang;
            fx32 k = Clamp01(FX32_ONE - FX_Div(aa - lim, 0x430));
            b = Mul(k, b);
        }
        b = Mul(b, dt);
        angularVelocity = Sub(angularVelocity, Scale(fwd, b));
    }

    VecFx32 pivot = bodyCentre;
    Rotate({angularVelocity.x, 0, angularVelocity.z}, pivot, matrix);
    if (0 < groundedWheels)
        pivot.z += model->rearWheel.z;
    Rotate({0, angularVelocity.y + wallVec.y, 0}, pivot, matrix);
    if (!s_resting) {
        matrix.m[3][0] += Mul(dt, vp.x);
        matrix.m[3][1] += Mul(dt, vp.y);
        matrix.m[3][2] += Mul(dt, vp.z);
        wheelSpin[0] += Mul(Mul(forwardSpeed, invFrontRadius), g_dsFrameTime);
        wheelSpin[1] += Mul(Mul(forwardSpeed, invRearRadius), g_dsFrameTime);
    }
    if (clampToWorld) {
        clampedToWorld = false;
        // Stadium and Snow: x and z kept in [-48, 656], the velocity damped at each clamp.
        for (int axis = 0; axis < 3; axis += 2) {
            fx32 &c = matrix.m[3][axis];
            if (c < WORLD_MIN) {
                c = WORLD_MIN;
                velocity = Scale(velocity, WORLD_BOUNCE);
                clampedToWorld = true;
            }
            if (WORLD_MAX < c) {
                c = WORLD_MAX;
                velocity = Scale(velocity, WORLD_BOUNCE);
                clampedToWorld = true;
            }
        }
    }
    speed = dsVec_Mag(&velocity);
}
