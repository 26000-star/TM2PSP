#include "dsCarDynamics.h"

#include <stdio.h>

#include "core/dsTime.h"
#include "gfx/dsModelInfo.h"
#include "math/dsMath.h"
#include "track/dsRaceSetup.h"

static inline fx32 Mul(fx32 a, fx32 b)
{
    return (fx32)(((s64)a * b + 0x800) >> 12);
}

static const fx32 NO_MIN_HEIGHT = (fx32)0xffc19001;  // ov003 0x0212b634: about -999
static const fx32 SEA_LEVEL_ISLAND = -16219;         // ov003 0x02134f98 (-3.96)
static const fx32 SEA_LEVEL_COAST = 0x2800;          // ov003 0x02134f9c (2.5)
static const fx32 SPEED_LIMIT = 0xb48e4;             // ov003 0x0212b64c (180.6 units/s)

// @port ov003:020fd914 partial (geometry only: the model, LODs, wheels and shadow come with the renderer)
bool dsCarModelGeometry::Load(const std::string &car, int skin, const dsCarParams &prm)
{
    char nm[64];
    snprintf(nm, sizeof nm, "Cars/%s/%d/lod1", car.c_str(), skin);
    VecFx32 c;
    if (!dsModel_GetBox(nm, &bodySize, &c))
        return false;
    bodyCentre = {0, bodySize.y / 2, 0};
    if (car == "sno") {
        bodySize.z -= 0x666;
        bodyCentre.z += 0x333;
        bodySize.x -= 0x333;
    }
    frontWheel = {prm.FrontWheel[0], prm.FrontWheel[1], prm.FrontWheel[2]};
    rearWheel = {prm.RearWheel[0], prm.RearWheel[1], prm.RearWheel[2]};
    return true;
}

// @port ov003:0212b3c4 partial (the contact block's vectors, the random generator and the debug fields are
// value-initialised by the declarations)
dsCarDynamics::dsCarDynamics(const std::string &car, const dsCarModelGeometry *m) : model(m), name(car)
{
    dsMtx43_Identity(&matrix);
    dsMtx43_Identity(&prevMatrix);
    dsMtx43_Identity(&lastMatrix);
    s32 env = dsRaceSetup::Get()->environment;
    minHeight = NO_MIN_HEIGHT;
    if (env == ENV_ISLAND)
        minHeight = SEA_LEVEL_ISLAND;
    if (env == ENV_COAST)
        minHeight = SEA_LEVEL_COAST;
    clampToWorld = env == ENV_STADIUM || env == ENV_SNOW;
    speedLimit = SPEED_LIMIT;
}

dsCarDynamics::~dsCarDynamics()
{
    for (dsCarWheel *w : wheels)
        delete w;
    delete engine;
}

// @port ov003:0212696c done (the parameter block is copied whole, then the descriptors re-registered)
void dsCarDynamics::LoadParams(const dsCarParams &table)
{
    p = table;
    p.RegisterParams();
    BuildWheels();
}

// @port ov003:02126628 done
void dsCarDynamics::ReadModelBox()
{
    if (!model)
        return;
    halfSize = {Mul(model->bodySize.x, 0x800), Mul(model->bodySize.y, 0x800), Mul(model->bodySize.z, 0x800)};
    bodyCentre = model->bodyCentre;
}

// Engine, wheels (front pair from FrontWheel, rear pair from RearWheel, mirrored in x), the body box, and the
// wheels lowered by 0.04 with the body raised as much.
// @port ov003:021266ec done
void dsCarDynamics::BuildWheels()
{
    if (!engine)
        engine = new dsCarEngine(p);
    else
        engine->Setup(p);
    maxSpeed = engine->MaxSpeed();
    invSteerOnset = FX_Inv(p.SteerOnset);
    if (!model)
        return;
    invFrontRadius = FX_Inv(model->frontWheel.y);
    invRearRadius = FX_Inv(model->rearWheel.y);
    VecFx32 fl = model->frontWheel, rl = model->rearWheel;
    VecFx32 fr = {-fl.x, fl.y, fl.z}, rr = {-rl.x, rl.y, rl.z};
    if (wheels.empty())
        for (int i = 0; i < 4; i++)
            wheels.push_back(new dsCarWheel);
    const VecFx32 *pos[4] = {&fl, &fr, &rl, &rr};
    for (int i = 0; i < 4; i++)
        wheels[i]->Init(*pos[i], *pos[i], p.WheelTravelPhys, pos[i]->y, p.WheelTravelBot, p.WheelTravelTop);
    gravity = p.Gravity;
    ReadModelBox();
    wheelLowering = (fx32)0xffffff5d;
    for (dsCarWheel *w : wheels)
        w->Lower(wheelLowering);
    matrix.m[3][1] -= wheelLowering;
}

// @port ov003:0212b0b0 partial (the input record, the contact block and the visual fields are cleared by Place)
void dsCarDynamics::Reset()
{
    inputMode = 0;
    steerAccum = 0;
    wheelTurn = 0;
    throttle = 0;
    dsMtx43_Identity(&matrix);
    dsMtx43_Identity(&prevMatrix);
    groundNormal = {0, FX32_ONE, 0};
    groundD = 0;
    dsMtx43_Copy(&matrix, &lastMatrix);
    velocity = angularVelocity = {};
    forwardSpeed = unk374 = 0;
    speed = 0;
    steer = unk3a4 = 0;
    wheelSpin[0] = wheelSpin[1] = 0;
    orthoRow = 0;
    unk400 = 0;
    skid = 0;
    for (dsCarWheel *w : wheels)
        w->Reset();
    if (engine) {
        engine->Reset();
        maxSpeed = engine->MaxSpeed();
    }
    warmUp = false;
}

// @port ov003:021279e8 partial (the visual update of the car model, 0x02126d10, is not ported)
void dsCarDynamics::Place(const MtxFx43 &m, bool updateVisual, bool warm)
{
    dsMtx43_Copy(&m, &matrix);
    dsMtx43_Copy(&matrix, &prevMatrix);
    dsMtx43_Copy(&matrix, &lastMatrix);
    airTime = 0;
    push = {};
    groundedWheels = 0;
    for (auto &h : wheelHits)
        h.clear();
    for (auto &h : bodyHits)
        h.clear();
    bounce = 0;
    forwardSpeed = unk374 = 0;
    velocity = angularVelocity = {};
    unk400 = 0;
    probeLength = 0x1800;
    warmUp = warm;
    (void)updateVisual;
}

// @port ov003:02122b10 done
void dsCarDynamics::SetInput(const dsCarInput &in)
{
    input = in;
    inputMode = 3;
}

static inline fx32 Clamp1(fx32 v, fx32 lo)
{
    if (v < lo)
        return lo;
    if (0xfff < v)
        return FX32_ONE;
    return v;
}

// Steering: steer follows input.steer at 1/SteerOnset per second; steerAccum builds up (5/s) while turning hard at
// speed with the brake held, and recentres by dt/OversteerCancel otherwise. pitch follows input.pitch the same
// way. Throttle = accel - 2 brake, approached at dt/AccelOnset (cut to <= 0 on surface 8). Then the velocity is
// limited to speedLimit and forwardSpeed = velocity . forward.
// @port ov003:02127af8 done
void dsCarDynamics::UpdateInputs()
{
    fx32 target = input.steer;
    bool mirrored = target < steer;
    if (mirrored) {
        steer = -steer;
        target = -target;
        steerAccum = -steerAccum;
    }
    bool braking = forwardSpeed > 0 && input.brake > 0;
    fx32 at = target < 0 ? -target : target;
    if (at < 0x800 || !braking) {
        fx32 d = p.OversteerCancel != 0 ? FX_Div(dt, p.OversteerCancel) : 0;
        if (steerAccum < 1) {
            steerAccum += d;
            if (steerAccum > 0)
                steerAccum = 0;
        } else {
            steerAccum -= d;
            if (steerAccum < 0)
                steerAccum = 0;
        }
    } else {
        steerAccum += dt * 5;
        if (steerAccum > FX32_ONE)
            steerAccum = FX32_ONE;
    }
    if (steer < target) {
        steer += Mul(invSteerOnset, dt);
        if (target < steer)
            steer = target;
    }
    if (mirrored) {
        steer = -steer;
        steerAccum = -steerAccum;
    }
    steer = Clamp1(steer, (fx32)0xfffff001);

    fx32 pt = input.pitch;
    bool pm = pt < unk3a4;
    if (pm) {
        unk3a4 = -unk3a4;
        pt = -pt;
    }
    if (unk3a4 < pt) {
        unk3a4 += Mul(invSteerOnset, dt);
        if (pt < unk3a4)
            unk3a4 = pt;
    }
    if (pm)
        unk3a4 = -unk3a4;
    unk3a4 = Clamp1(unk3a4, (fx32)0xfffff001);

    fx32 tt = Clamp1(input.accel + input.brake * -2, (fx32)0xfffff001);
    if (tt < 0 && throttle > 0)
        throttle = 0;
    fx32 cur = throttle;
    if (cur != tt) {
        bool neg = tt < cur;
        if (neg)
            throttle = -cur;
        fx32 lim = neg ? -tt : tt;
        fx32 d = p.AccelOnset != 0 ? FX_Div(g_dsFrameTime, p.AccelOnset) : 0;
        throttle += d;
        if (lim < throttle)
            throttle = lim;
        if (neg)
            throttle = -throttle;
    }
    if (noThrottle) {
        if (forwardSpeed < 0x801)
            throttle = 0;
        else if (throttle >= 0)
            throttle = 0;
    }
    fx32 v = dsVec_Mag(&velocity);
    if (speedLimit < v) {
        if (velocity.x == 0 && velocity.y == 0 && velocity.z == 0)
            velocity = {0, 0, FX32_ONE};
        else
            dsVec_Normalize(&velocity, &velocity);
        velocity = {Mul(velocity.x, speedLimit), Mul(velocity.y, speedLimit), Mul(velocity.z, speedLimit)};
    }
    VecFx32 fwd = {matrix.m[2][0], matrix.m[2][1], matrix.m[2][2]};
    forwardSpeed = (fx32)(((s64)velocity.z * fwd.z + (s64)velocity.x * fwd.x + (s64)fwd.y * velocity.y + 0x800) >> 12);
    fx32 sa = steerAccum < 0 ? -steerAccum : steerAccum;
    skid = unk400 <= sa ? sa : unk400;
    if (0xfff < skid)
        skid = FX32_ONE;
}
