#include "dsCarParams.h"

#include <string.h>

#include "script/dsLua.h"

// Default editor limits of the descriptors: +-100000.0
static const s32 FX_MIN = -409599999;
static const s32 FX_MAX = 409600000;

// 1 mph = 0.44704 m/s; 1831 / 4096 = 0.44702
static const fx32 MPH_TO_MS = 1831;
// pi * 4096 and 180 * 4096, used for degrees -> radians (with a 64-bit intermediate)
static const s64 FX_PI = 12868;
static const s64 FX_180 = 737280;
static const s32 FX_TWO_PI = 25736;

// @port ov003:0212ddd8 done (port: value-initialises every field)
dsCarParams::dsCarParams()
{
    // The original only sets name = -1 and an empty descriptor vector; the map node is copy-constructed from this
    // stack object, so the other fields start with whatever was on the stack. They are all loaded or derived,
    // except unk_058, unk_060 and unk_15c.
    s32 *first = &SteerOnset;
    memset(first, 0, (char *)&params - (char *)first);
    memset(&CollPushAway, 0, (char *)(&Tilt + 1) - (char *)&CollPushAway);
}

// @port ov003:0212da88 done
dsCarParams::dsCarParams(const dsCarParams &o)
{
    *this = o;
}

dsCarParams &dsCarParams::operator=(const dsCarParams &o)
{
    name = o.name;
    memcpy(&SteerOnset, &o.SteerOnset, (char *)&params - (char *)&SteerOnset);
    memcpy(&CollPushAway, &o.CollPushAway, (char *)(&Tilt + 1) - (char *)&CollPushAway);
    params.clear(); // descriptors point into their own object; Load() registers them again
    return *this;
}

// @port ov003:0212cbb8 done
void dsCarParams::RegisterParam(s32 *value, const char *pname, s32 type, u8 editable, s32 min, s32 max)
{
    dsCarParamDesc d;
    d.type = type;
    d.value = value;
    d.loaded = *value;
    d.name = pname;
    d.min = min;
    d.max = max;
    d.editable = editable;
    params.push_back(d);
}

// @port ov003:0212cd74 done
void dsCarParams::RegisterParams()
{
    params.clear();
    RegisterParam(&SteerOnset, "SteerOnset", dsCarParamDesc::FX32, 1, FX_MIN, FX_MAX); // 0x004
    RegisterParam(&Steerability, "Steerability", dsCarParamDesc::FX32, 1, FX_MIN, FX_MAX); // 0x008
    RegisterParam(&SteerFirmUp, "SteerFirmUp", dsCarParamDesc::FX32, 1, FX_MIN, FX_MAX); // 0x18c
    RegisterParam(&SkidSpeedEffect, "SkidSpeedEffect", dsCarParamDesc::FX32, 1, FX_MIN, FX_MAX); // 0x188
    RegisterParam(&GripOversteer, "GripOversteer", dsCarParamDesc::FX32, 1, FX_MIN, FX_MAX); // 0x02c
    RegisterParam(&OversteerCancel, "OversteerCancel", dsCarParamDesc::FX32, 1, FX_MIN, FX_MAX); // 0x064
    RegisterParam(&Grip, "Grip", dsCarParamDesc::FX32, 1, FX_MIN, FX_MAX); // 0x028
    RegisterParam(&GripGrass, "GripGrass", dsCarParamDesc::FX32, 1, FX_MIN, FX_MAX); // 0x01c
    RegisterParam(&GripDirt, "GripDirt", dsCarParamDesc::FX32, 1, FX_MIN, FX_MAX); // 0x018
    RegisterParam(&GripSnow, "GripSnow", dsCarParamDesc::FX32, 1, FX_MIN, FX_MAX); // 0x024
    RegisterParam(&GripGravel, "GripGravel", dsCarParamDesc::FX32, 1, FX_MIN, FX_MAX); // 0x020
    RegisterParam(&GripInc, "GripInc", dsCarParamDesc::FX32, 1, FX_MIN, FX_MAX); // 0x030
    RegisterParam(&SuspLinearForce, "SuspLinearForce", dsCarParamDesc::FX32, 1, FX_MIN, FX_MAX); // 0x034
    RegisterParam(&SuspLinearDamp, "SuspLinearDamp", dsCarParamDesc::FX32, 1, FX_MIN, FX_MAX); // 0x038
    RegisterParam(&SuspAngForce, "SuspAngForce", dsCarParamDesc::FX32, 1, FX_MIN, FX_MAX); // 0x03c
    RegisterParam(&AngDampGroundXZ, "AngDampGroundXZ", dsCarParamDesc::FX32, 1, FX_MIN, FX_MAX); // 0x044
    RegisterParam(&AngDampGroundY, "AngDampGroundY", dsCarParamDesc::FX32, 1, FX_MIN, FX_MAX); // 0x048
    RegisterParam(&AngDampAir, "AngDampAir", dsCarParamDesc::FX32, 1, FX_MIN, FX_MAX); // 0x050
    RegisterParam(&AngDampRelease, "AngDampRelease", dsCarParamDesc::FX32, 1, FX_MIN, FX_MAX); // 0x084
    RegisterParam(&DonutSpeed, "DonutSpeed", dsCarParamDesc::FX32, 0, FX_MIN, FX_MAX); // 0x194
    RegisterParam(&WheelTravelPhys, "WheelTravelPhys", dsCarParamDesc::FX32, 0, FX_MIN, FX_MAX); // 0x06c
    RegisterParam(&SelfRighting, "SelfRighting", dsCarParamDesc::FX32, 1, FX_MIN, FX_MAX); // 0x080
    RegisterParam(&AirControlSteer, "AirControlSteer", dsCarParamDesc::FX32, 1, FX_MIN, FX_MAX); // 0x088
    RegisterParam(&AirControlPitch, "AirControlPitch", dsCarParamDesc::FX32, 1, FX_MIN, FX_MAX); // 0x08c
    RegisterParam(&Tilt, "Tilt", dsCarParamDesc::FX32, 1, FX_MIN, FX_MAX); // 0x1bc
    RegisterParam(&TiltLimit, "TiltLimit", dsCarParamDesc::FX32, 1, FX_MIN, FX_MAX); // 0x090
    RegisterParam(&TiltSkidFactor, "TiltSkidFactor", dsCarParamDesc::FX32, 1, FX_MIN, FX_MAX); // 0x094
    RegisterParam(&HandbrakeFrict, "HandbrakeFrict", dsCarParamDesc::FX32, 1, FX_MIN, FX_MAX); // 0x098
    RegisterParam(&BrakeFriction, "BrakeFriction", dsCarParamDesc::FX32, 1, FX_MIN, FX_MAX); // 0x09c
    RegisterParam(&GrassFriction, "GrassFriction", dsCarParamDesc::FX32, 1, FX_MIN, FX_MAX); // 0x0a0
    RegisterParam(&Gravity, "Gravity", dsCarParamDesc::FX32, 1, FX_MIN, FX_MAX); // 0x0a4
    RegisterParam(&SlopeClimb, "SlopeClimb", dsCarParamDesc::FX32, 1, FX_MIN, FX_MAX); // 0x0a8
    RegisterParam(&RampPitchTime, "RampPitchTime", dsCarParamDesc::FX32, 1, FX_MIN, FX_MAX); // 0x0b0
    RegisterParam(&RampPitchForce, "RampPitchForce", dsCarParamDesc::FX32, 1, FX_MIN, FX_MAX); // 0x0b4
    RegisterParam(&AirSlowdown, "AirSlowdown", dsCarParamDesc::FX32, 1, FX_MIN, FX_MAX); // 0x0b8
    RegisterParam(&StaticGripSpeed, "StaticGripSpeed", dsCarParamDesc::FX32, 1, FX_MIN, FX_MAX); // 0x0bc
    RegisterParam(&StaticGrip, "StaticGrip", dsCarParamDesc::FX32, 1, FX_MIN, FX_MAX); // 0x0c0
    RegisterParam(&GroundSlowdown, "GroundSlowdown", dsCarParamDesc::FX32, 1, FX_MIN, FX_MAX); // 0x0c8
    RegisterParam(&GroundSlowInc, "GroundSlowInc", dsCarParamDesc::FX32, 1, FX_MIN, FX_MAX); // 0x0c4
    RegisterParam(&FlatAng, "FlatAng", dsCarParamDesc::FX32, 1, FX_MIN, FX_MAX); // 0x180
    RegisterParam(&MagicGripSpeed, "MagicGripSpeed", dsCarParamDesc::FX32, 1, FX_MIN, FX_MAX); // 0x184
    RegisterParam(&Mass, "Mass", dsCarParamDesc::FX32, 0, FX_MIN, FX_MAX); // 0x0d4
    RegisterParam(&RotInertia, "RotInertia", dsCarParamDesc::FX32, 1, FX_MIN, FX_MAX); // 0x190
    RegisterParam(&CarCarShunt, "CarCarShunt", dsCarParamDesc::FX32, 0, FX_MIN, FX_MAX); // 0x0dc
    RegisterParam(&CollElasticity, "CollElasticity", dsCarParamDesc::FX32, 1, FX_MIN, FX_MAX); // 0x0e0
    RegisterParam(&WallTurnFactor, "WallTurnFactor", dsCarParamDesc::FX32, 1, FX_MIN, FX_MAX); // 0x0e4
    RegisterParam(&WallTurnMax, "WallTurnMax", dsCarParamDesc::FX32, 1, FX_MIN, FX_MAX); // 0x0e8
    RegisterParam(&WallSlowdown, "WallSlowdown", dsCarParamDesc::FX32, 1, FX_MIN, FX_MAX); // 0x0ec
    RegisterParam(&CollPushAway, "CollPushAway", dsCarParamDesc::FX32, 1, FX_MIN, FX_MAX); // 0x158
    RegisterParam(&WallEnforcer, "WallEnforcer", dsCarParamDesc::FX32, 1, FX_MIN, FX_MAX); // 0x0f0
    RegisterParam(&RPMMin, "RPMMin", dsCarParamDesc::INT, 1, FX_MIN, FX_MAX); // 0x0f4
    RegisterParam(&RPMMax, "RPMMax", dsCarParamDesc::INT, 1, FX_MIN, FX_MAX); // 0x0f8
    RegisterParam(&GearChangeGap, "GearChangeGap", dsCarParamDesc::FX32, 1, FX_MIN, FX_MAX); // 0x100
    RegisterParam(&NumGears, "NumGears", dsCarParamDesc::INT, 1, 8192, 28672); // 0x0fc
    RegisterParam(&TimeRev, "TimeRev", dsCarParamDesc::FX32, 1, FX_MIN, FX_MAX); // 0x120
    RegisterParam(&TopSpeedRev, "TopSpeedRev", dsCarParamDesc::FX32, 1, FX_MIN, FX_MAX); // 0x1a0
    RegisterParam(&Time[0], "Time1", dsCarParamDesc::FX32, 1, FX_MIN, FX_MAX); // 0x124
    RegisterParam(&TopSpeed[0], "TopSpeed1", dsCarParamDesc::FX32, 1, FX_MIN, FX_MAX); // 0x1a4
    RegisterParam(&Time[1], "Time2", dsCarParamDesc::FX32, 1, FX_MIN, FX_MAX); // 0x128
    RegisterParam(&TopSpeed[1], "TopSpeed2", dsCarParamDesc::FX32, 1, FX_MIN, FX_MAX); // 0x1a8
    RegisterParam(&Time[2], "Time3", dsCarParamDesc::FX32, 1, FX_MIN, FX_MAX); // 0x12c
    RegisterParam(&TopSpeed[2], "TopSpeed3", dsCarParamDesc::FX32, 1, FX_MIN, FX_MAX); // 0x1ac
    RegisterParam(&Time[3], "Time4", dsCarParamDesc::FX32, 1, FX_MIN, FX_MAX); // 0x130
    RegisterParam(&TopSpeed[3], "TopSpeed4", dsCarParamDesc::FX32, 1, FX_MIN, FX_MAX); // 0x1b0
    RegisterParam(&Time[4], "Time5", dsCarParamDesc::FX32, 1, FX_MIN, FX_MAX); // 0x134
    RegisterParam(&TopSpeed[4], "TopSpeed5", dsCarParamDesc::FX32, 1, FX_MIN, FX_MAX); // 0x1b4
    RegisterParam(&Time[5], "Time6", dsCarParamDesc::FX32, 1, FX_MIN, FX_MAX); // 0x138
    RegisterParam(&TopSpeed[5], "TopSpeed6", dsCarParamDesc::FX32, 1, FX_MIN, FX_MAX); // 0x1b8
    RegisterParam(&SpeedBoost, "SpeedBoost", dsCarParamDesc::FX32, 1, FX_MIN, FX_MAX); // 0x0ac
    RegisterParam(&AccelOnset, "AccelOnset", dsCarParamDesc::FX32, 1, FX_MIN, FX_MAX); // 0x074
    RegisterParam(&WheelTravelTop, "WheelTravelTop", dsCarParamDesc::FX32, 1, FX_MIN, FX_MAX); // 0x13c
    RegisterParam(&WheelTravelBot, "WheelTravelBot", dsCarParamDesc::FX32, 1, FX_MIN, FX_MAX); // 0x140
    RegisterParam(&WheelTurnVisual, "WheelTurnVisual", dsCarParamDesc::FX32, 1, FX_MIN, FX_MAX); // 0x148
    RegisterParam(&FakeRollFactor, "FakeRollFactor", dsCarParamDesc::FX32, 1, FX_MIN, FX_MAX); // 0x198
    RegisterParam(&FakePitchFactor, "FakePitchFactor", dsCarParamDesc::FX32, 1, FX_MIN, FX_MAX); // 0x19c
    RegisterParam(&FakePitchReduce, "FakePitchReduce", dsCarParamDesc::FX32, 1, FX_MIN, FX_MAX); // 0x07c
    RegisterParam(&RearRadius, "RearRadius", dsCarParamDesc::FX32, 0, FX_MIN, FX_MAX); // 0x178
    RegisterParam(&FrontRadius, "FrontRadius", dsCarParamDesc::FX32, 0, FX_MIN, FX_MAX); // 0x17c
}

// @port ov003:0212cc58 done
void dsCarParams::Load(const dsLuaObject &table, const char *carName)
{
    name = carName;
    RegisterParams();
    for (dsCarParamDesc &d : params) {
        if (d.type == dsCarParamDesc::INT) {
            if (table.HasField(d.name))
                *d.value = table.FieldNumber(d.name) >> FX32_SHIFT;
            d.loaded = *d.value;
        } else if (d.type == dsCarParamDesc::FX32) {
            if (table.HasField(d.name))
                *d.value = table.FieldNumber(d.name);
            d.loaded = *d.value;
        }
    }
    dsLuaObject front = table.Field("FrontWheel");
    FrontWheel[0] = front.IndexNumber(0);
    FrontWheel[1] = front.IndexNumber(1);
    FrontWheel[2] = front.IndexNumber(2);
    dsLuaObject rear = table.Field("RearWheel");
    RearWheel[0] = rear.IndexNumber(0);
    RearWheel[1] = rear.IndexNumber(1);
    RearWheel[2] = rear.IndexNumber(2);
    ComputeDerived();
}

static fx32 MphToMs(fx32 v)
{
    return FX_MUL(v, MPH_TO_MS);
}

// @port ov003:0212c9ec done
void dsCarParams::ComputeDerived()
{
    d_SkidSpeedEffect = SkidSpeedEffect;
    if (d_SkidSpeedEffect < 0)
        d_SkidSpeedEffect = d_SkidSpeedEffect / 1000;
    d_SkidSpeedEffect2 = d_SkidSpeedEffect;
    d_SteerFirmUp100 = SteerFirmUp / 100;
    GripOversteer = Grip;
    d_SuspAngForce = SuspAngForce;
    d_AngDampGroundY = AngDampGroundY;

    // cos(FlatAng): degrees -> radians, wrapped into [0, 2pi), then the SDK sin/cos table
    s32 rad = (s32)((s64)FlatAng * FX_PI / FX_180);
    bool neg = rad < 0;
    if (neg)
        rad = -rad;
    rad = FX_ModS32(rad, FX_TWO_PI);
    if (neg)
        rad = FX_TWO_PI - rad;
    s32 idx = 0xffff * rad / FX_TWO_PI;
    d_FlatAngCos = FX_SinCosTable_[(idx >> 4) * 2 + 1];

    d_MagicGripSpeed = MphToMs(MagicGripSpeed);
    d_Inertia = FX_MUL(Mass, RotInertia);
    d_SteerOnset = SteerOnset;
    d_TopSpeed[0] = MphToMs(TopSpeedRev);
    for (int i = 0; i < 6; i++)
        d_TopSpeed[i + 1] = MphToMs(TopSpeed[i]);
    d_FakeRollFactor100 = FakeRollFactor / 100;
    FakePitchFactor = unk_058 / 5;
    d_FakePitchReduce = MphToMs(FakePitchReduce);
    d_WheelTurnVisual = (fx32)((s64)WheelTurnVisual * FX_PI / FX_180);
    d_DonutSpeed = MphToMs(DonutSpeed);
    d_Tilt100 = Tilt / 100;
}

// ---- dsCarParamsTable ----

// @port ov003:0212c47c done
void dsCarParamsTable::Clear()
{
    m_cars.clear();
}

// @port ov003:0212c2c8 done
dsCarParams &dsCarParamsTable::Get(const std::string &car)
{
    auto it = m_cars.find(car);
    if (it != m_cars.end())
        return it->second;
    return m_cars.emplace(car, dsCarParams()).first->second;
}

static dsScriptManager *s_scriptManager; // ov003 static at DAT_ov003__0212c0e0

// @port ov003:0212bed0 done
void dsCarParamsTable::Load()
{
    Clear();
    if (!s_scriptManager)
        s_scriptManager = new dsScriptManager();
    dsLuaStateRef state = s_scriptManager->Run("config/vehicles");
    dsLuaObject cars = state->Global("carParams");
    for (dsLuaObject key = cars.Begin(); !key.IsNil(); key = cars.Next(key)) {
        dsLuaObject table = cars.Value(key);
        dsCarParams &p = Get(std::string(key.ToString()));
        p.Load(table, key.ToString());
    }
    dsLuaObject saveTime = state->Global("SaveTime");
    if (!saveTime.IsNil())
        m_saveTime = saveTime.ToString();
    else
        m_saveTime.clear();
    m_unk04 = false;
}
