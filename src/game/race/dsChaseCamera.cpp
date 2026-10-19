#include "race/dsChaseCamera.h"

#include <string>

#include "car/dsCarDynamics.h"
#include "car/dsCarMath.h"
#include "collision/dsCollisionWorld.h"
#include "core/dsTime.h"
#include "math/dsMath.h"
#include "script/dsLua.h"

using namespace carmath;

// (stiffness, damping) pairs of main 0x020c1d08; the first x and the last pair are cached in .bss by a static
// initialiser (0x020b3240).
static const fx32 kSpringTable[30][2] = {
    {40960, 24576},   {81920, 32768},   {122880, 40960},  {163840, 47104},  {204800, 51200},  {245760, 57344},
    {286720, 61440},  {327680, 63488},  {368640, 67584},  {409600, 71680},  {450560, 75776},  {491520, 77824},
    {532480, 79872},  {573440, 83968},  {614400, 86016},  {655360, 88064},  {696320, 92160},  {737280, 94208},
    {778240, 96256},  {819200, 96256},  {860160, 100352}, {901120, 102400}, {942080, 104448}, {983040, 106496},
    {1024000, 108544}, {1064960, 110592}, {1105920, 112640}, {1146880, 114688}, {1187840, 114688},
    {1228800, 116736}};

// Linear interpolation in the table; below the first pair the first segment is extended, above the last one the
// segment from the middle pair to the last pair.
// @port main:020abc68 done
fx32 dsSpring::Damping(fx32 x)
{
    const int n = 30;
    fx32 x0 = kSpringTable[0][0];
    if (x < x0) {
        fx32 t = kSpringTable[1][0] == x0 ? 0 : FX_Div(x - x0, kSpringTable[1][0] - x0);
        return kSpringTable[0][1] + Mul(kSpringTable[1][1] - kSpringTable[0][1], t);
    }
    fx32 xi = x0;
    for (int i = 0; i < n - 1; i++) {
        fx32 xn = kSpringTable[i + 1][0];
        if (x < xn) {
            fx32 t = xn == xi ? 0 : FX_Div(x - xi, xn - xi);
            return kSpringTable[i][1] + Mul(kSpringTable[i + 1][1] - kSpringTable[i][1], t);
        }
        xi = xn;
    }
    int m = n / 2;
    fx32 xm = kSpringTable[m][0], ym = kSpringTable[m][1];
    fx32 xl = kSpringTable[n - 1][0], yl = kSpringTable[n - 1][1];
    fx32 t = xl == xm ? 0 : FX_Div(x - xl, xl - xm);
    return yl + Mul(yl - ym, t);
}

// @port main:020abc38 done
void dsSpring::Init(fx32 stiffness)
{
    k = stiffness;
    d = Damping(stiffness);
    vel = pos = 0;
}

// vel += ((target - pos) k - vel d) dt, pos += vel dt (each product rounded).
// @port main:020abb78 done
fx32 dsSpring::Step(fx32 target)
{
    fx32 a = Mul(target - pos, k) - Mul(vel, d);
    vel += Mul(a, g_dsFrameTime);
    pos += Mul(vel, g_dsFrameTime);
    return pos;
}

// @port itcm:01ffabfc done
void dsMtx43_LookAt(MtxFx43 *m, const VecFx32 &pos, const VecFx32 &target, const VecFx32 &up)
{
    VecFx32 f = NormalizeSafe(Sub(target, pos));
    VecFx32 r = NormalizeSafe(Cross(up, f));
    VecFx32 u = Cross(f, r);
    SetRow(*m, 0, r);
    SetRow(*m, 1, u);
    SetRow(*m, 2, f);
    SetRow(*m, 3, pos);
}

// @port main:0208fe10 partial (the base camera's fade and FOV ramp fields are not kept)
dsChaseCamera::dsChaseCamera(int k) : kind(k)
{
    dsMtx43_Identity(&matrix);
}

// Reads cameras[<env>]: Chase<kind>Height, Distance, TiltTightness, LookAtHeight, FOV, DirTightness, DistInc,
// DistDec, RollMode (>> 12) and DistTightness (spring stiffness).
// @port main:0208fa18 done
bool dsChaseCamera::Load(const char *env)
{
    static dsScriptManager s_scripts;
    dsLuaStateRef state = s_scripts.Run("config/cameras");
    if (!state.get())
        return false;
    dsLuaObject cams = state->Global("cameras");
    std::string pre = kind == 2 ? "Chase2" : "Chase1";
    fx32 tight = 0;
    bool found = false;
    for (dsLuaObject k = cams.Begin(); !k.IsNil(); k = cams.Next(k)) {
        const char *name = k.ToString();
        if (!name || pre.empty() || std::string(name) != env)
            continue;
        dsLuaObject v = cams.Value(k);
        auto f = [&](const char *field) { return (fx32)v.FieldNumber((pre + field).c_str()); };
        height = f("Height");
        distance = f("Distance");
        tiltTightness = f("TiltTightness");
        lookAtHeight = f("LookAtHeight");
        fov = f("FOV");
        dirTightness = f("DirTightness");
        distInc = f("DistInc");
        distDec = f("DistDec");
        tight = f("DistTightness");
        rollMode = f("RollMode") >> 12;
        found = true;
    }
    distSpring.Init(tight);
    return found;
}

// @port main:0208fd68 done (0x0208e5c8 is empty)
void dsChaseCamera::Reset()
{
    reverse = (fx32)0xfffff001;
    init = false;
    distSpring.Reset(0);
    roll = 0;
    pitchHistory.resize(20, 0);
    boost = false;
    flipBlend = 0;
    flipUp = {0, FX32_ONE, 0};
    airBlend = 0;
}

static inline fx32 Lerp32(fx32 a, fx32 b, fx32 t) { return a + ((s32)((u32)t * (u32)(b - a)) >> 12); }
static inline VecFx32 Lerp32(const VecFx32 &a, const VecFx32 &b, fx32 t)
{
    return {Lerp32(a.x, b.x, t), Lerp32(a.y, b.y, t), Lerp32(a.z, b.z, t)};
}
static inline fx32 Smooth(fx32 h) { return Mul(Mul(h, h), 0x3000 - 2 * h); }

// @port main:0208e694 partial (the player and its car are the dynamics: display matrix +0x1f0, velocity +0x378,
// contact test ov003 0x021226f8 for the player's vtable +0x2c; the base update 0x0208e524 only runs the fade and
// FOV ramps, which the race does not start; the RollMode 0 divide is FX_Div(|up.y|, 0.1), its operands read from
// the register flow)
void dsChaseCamera::Update(const dsCarDynamics &car)
{
    const MtxFx43 &M = car.prevMatrix;
    const VecFx32 right = Row(M, 0), carUp = Row(M, 1), fwd = Row(M, 2), cpos = Row(M, 3);
    const VecFx32 vel = car.velocity;
    const bool air = !((car.bodyHitCount > 0 && car.hitNormalSum.y > 3277) || car.groundedWheels > 0);
    const fx32 dt = g_dsFrameTime;

    // Distance: a spring pushed by the forward acceleration (or the air / boost state).
    fx32 a = Dot(fwd, car.acceleration);
    if (Dot(Row(matrix, 2), fwd) < 0)
        a = -a;
    bool accel = 0x800 < a && 0 < car.throttle;
    bool brake = a < -0x800 && car.throttle < 0;
    if (air || boost)
        accel = true;
    fx32 s = distSpring.Step(accel ? FX32_ONE : (brake ? (fx32)0xfffff001 : 0));
    fx32 dist = distance + Mul(s, s < 0 ? distDec : distInc);

    // Behind / in front of the car (reversing).
    fx32 revTarget = (fx32)0xfffff001;
    if (!air && Dot(fwd, vel) < -1228)
        revTarget = FX32_ONE;
    dsApproach(&reverse, revTarget, 0x2000);
    if (!init)
        reverse = (fx32)0xfffff001;
    fx32 side = Dot(Sub(pos, cpos), right) < 0 ? (fx32)0xfffff001 : FX32_ONE;
    fx32 u = Smooth((reverse + FX32_ONE) / 2) * 2 - FX32_ONE;
    dsApproach(&airBlend, air && !boost ? FX32_ONE : 0, FX32_ONE);
    fx32 extraHeight = Mul(Smooth(airBlend), 0x2000);
    VecFx32 local = {Mul(Mul(side, FX_Sqrt(FX32_ONE - Mul(u, u))), dist), height + extraHeight, Mul(u, dist)};
    VecFx32 desired;
    dsVec_MulMtx43(&local, &M, &desired);
    if (!init) {
        pos = desired;
        targetUp = carUp.y < 0xb51 ? VecFx32{0, FX32_ONE, 0} : carUp;
        up = targetUp;
        reverse = (fx32)0xfffff001;
        init = true;
    }
    bool turbo = 0 < car.turboTimer;
    if (!air)
        boost = turbo;
    fx32 hs = FX_Sqrt(Mul(vel.x, vel.x) + Mul(vel.z, vel.z));
    bool falling = vel.y < 0;
    if (410 < hs)
        falling = FX_Div(vel.y, hs) < (fx32)0xfffff001 || vel.y < (fx32)0xfffd8001;
    if (!turbo && carUp.y < -0x199 && falling)
        boost = false;

    // Position: follow the desired point.
    fx32 dirT = dirTightness;
    if (air && boost && dirT < 0x6001)
        dirT = 0x6000;
    pos = Lerp32(pos, desired, Mul(dt, dirT));

    // Up vector: the car's up (RollMode 1: from its right axis half-way projected on the ground plane), back to the
    // world up while the car is upside down, smoothed by TiltTightness.
    fx32 tiltT = tiltTightness;
    if (turbo && tiltT < 0x6001)
        tiltT = 0x6000;
    if (rollMode == 1) {
        VecFx32 v = IsZero(car.groundNormal) ? right : Project(car.groundNormal, right);
        VecFx32 mid = {right.x + ((s32)((u32)(v.x - right.x) * 0x800u) >> 12),
                       right.y + ((s32)((u32)(v.y - right.y) * 0x800u) >> 12),
                       right.z + ((s32)((u32)(v.z - right.z) * 0x800u) >> 12)};
        targetUp = Cross(fwd, mid);
    } else {
        targetUp = carUp;
    }
    if (carUp.y < 0xb33 && !boost) {
        if (flipBlend == 0)
            flipUp = targetUp;
        dsApproach(&flipBlend, FX32_ONE, 0x2000);
    } else {
        dsApproach(&flipBlend, 0, FX32_ONE);
    }
    if (flipBlend != 0) {
        fx32 k5 = dt * 5;
        flipUp = Lerp32(flipUp, VecFx32{0, FX32_ONE, 0}, k5);
        flipUp = NormalizeSafe(flipUp);
        targetUp = NormalizeSafe(Lerp32(targetUp, flipUp, flipBlend));
    }
    up = NormalizeSafe(Lerp32(up, targetUp, Mul(dt, tiltT)));

    // Pitch roll: the change of the ground normal along the car, averaged over 20 updates.
    fx32 sample = 0;
    if (!air) {
        VecFx32 d = Sub(car.groundNormal, prevGround);
        if (Dot(d, d) < 0x400)
            sample = Dot(fwd, VecFx32{Mul(dt, d.x), Mul(dt, d.y), Mul(dt, d.z)}) * 500;
    }
    pitchHistory.resize(20, 0);
    pitchHistory[pitchIndex] = sample;
    if (19 < ++pitchIndex)
        pitchIndex = 0;
    s32 sum = 0, count = 0;
    for (fx32 h : pitchHistory) {
        if (h != 0)
            count++;
        sum += h;
    }
    s32 avg = sum / 20;
    if (count < 2 || air)
        avg = 0;
    roll += Mul(avg - roll, dt * 5);
    VecFx32 upV = up;
    if (rollMode != 0 && roll < 0) {
        fx32 r = roll <= -1228 ? -1228 : roll;
        VecFx32 axis = Row(matrix, 0);
        MtxFx43 rot;
        dsMtx43_RotateAxis(&rot, &axis, dsSin(r), dsCos(r));
        dsVec_MulMtx43(&upV, &rot, &upV);
    }

    // Final position: at the distance from the car in the plane across upV, Height (+ the air lift) above it.
    VecFx32 off = NormalizeSafe(Project(upV, Sub(pos, cpos)));
    pos = Add(cpos, Scale(off, dist));
    pos = Add(pos, Scale(upV, height + extraHeight));
    if (car.matrix.m[3][1] < car.minHeight && pos.y <= car.minHeight + 0x800)
        pos.y = car.minHeight + 0x800;

    if (active) {
        VecFx32 target = Add(cpos, Scale(upV, lookAtHeight));
        if (rollMode < 2) {
            VecFx32 dir = NormalizeSafe(Sub(target, pos));
            VecFx32 sideV = Cross(upV, dir);
            if (rollMode == 0) {
                fx32 ay = upV.y < 0 ? -upV.y : upV.y;
                sideV.y = ay < 410 ? Mul(sideV.y, FX32_ONE - FX_Div(ay, 410)) : 0;
                sideV = NormalizeSafe(sideV);
            }
            upV = NormalizeSafe(Cross(dir, sideV));
            target = Add(cpos, Scale(upV, lookAtHeight));
        }
        dsMtx43_LookAt(&matrix, pos, target, upV);
    }
    prevGround = car.groundNormal;
}
