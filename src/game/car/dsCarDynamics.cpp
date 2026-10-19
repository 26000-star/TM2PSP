#include "dsCarDynamics.h"

#include <stdio.h>

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
