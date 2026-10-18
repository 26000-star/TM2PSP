// Vehicle handling parameters, read from the global carParams of Scripts/config/vehicles.luac (ov003).
// One dsCarParams per environment car ("coa", "isl", "sno", "sta"). Field names are the Lua keys; offsets are the
// original's (0x1c0 bytes). Values are fx32 unless noted. The "d_" fields are derived by ComputeDerived().
#pragma once

#include <map>
#include <string>
#include <vector>

#include "core/dsStringId.h"
#include "nitro.h"

class dsLuaObject;

// Descriptor of one Lua-loaded field, 0x1c bytes (0x0212cbb8).
struct dsCarParamDesc {
    enum Type { INT = 0, FX32 = 1 };
    s32 type;          // INT: the Lua fx32 value >> 12 is stored
    s32 *value;
    s32 loaded;        // value after Load (or the previous value when the key is absent)
    const char *name;
    s32 min, max;      // editor limits (debug menu), unused by the loader
    u8 editable;
};

struct dsCarParams {
    dsStringIdB name;               // 0x000
    fx32 SteerOnset;                // 0x004
    fx32 Steerability;              // 0x008
    fx32 d_SteerFirmUp100;          // 0x00c = SteerFirmUp / 100
    fx32 d_SkidSpeedEffect;         // 0x010 = SkidSpeedEffect, / 1000 when negative
    fx32 d_SkidSpeedEffect2;        // 0x014 = 0x010
    fx32 GripDirt;                  // 0x018
    fx32 GripGrass;                 // 0x01c
    fx32 GripGravel;                // 0x020
    fx32 GripSnow;                  // 0x024
    fx32 Grip;                      // 0x028
    fx32 GripOversteer;             // 0x02c (overwritten with Grip by ComputeDerived)
    fx32 GripInc;                   // 0x030
    fx32 SuspLinearForce;           // 0x034
    fx32 SuspLinearDamp;            // 0x038
    fx32 SuspAngForce;              // 0x03c
    fx32 d_SuspAngForce;            // 0x040 = SuspAngForce
    fx32 AngDampGroundXZ;           // 0x044
    fx32 AngDampGroundY;            // 0x048
    fx32 d_AngDampGroundY;          // 0x04c = AngDampGroundY
    fx32 AngDampAir;                // 0x050
    fx32 d_FakeRollFactor100;       // 0x054 = FakeRollFactor / 100
    fx32 unk_058;                   // 0x058 (not loaded; FakePitchFactor is overwritten with unk_058 / 5)
    fx32 d_SteerOnset;              // 0x05c = SteerOnset
    fx32 unk_060;                   // 0x060
    fx32 OversteerCancel;           // 0x064
    fx32 d_DonutSpeed;              // 0x068 = DonutSpeed in m/s (input in mph)
    fx32 WheelTravelPhys;           // 0x06c
    fx32 d_Tilt100;                 // 0x070 = Tilt / 100
    fx32 AccelOnset;                // 0x074
    fx32 d_FakePitchReduce;         // 0x078 = FakePitchReduce mph -> m/s
    fx32 FakePitchReduce;           // 0x07c
    fx32 SelfRighting;              // 0x080
    fx32 AngDampRelease;            // 0x084
    fx32 AirControlSteer;           // 0x088
    fx32 AirControlPitch;           // 0x08c
    fx32 TiltLimit;                 // 0x090
    fx32 TiltSkidFactor;            // 0x094
    fx32 HandbrakeFrict;            // 0x098
    fx32 BrakeFriction;             // 0x09c
    fx32 GrassFriction;             // 0x0a0
    fx32 Gravity;                   // 0x0a4
    fx32 SlopeClimb;                // 0x0a8
    fx32 SpeedBoost;                // 0x0ac
    fx32 RampPitchTime;             // 0x0b0
    fx32 RampPitchForce;            // 0x0b4
    fx32 AirSlowdown;               // 0x0b8
    fx32 StaticGripSpeed;           // 0x0bc
    fx32 StaticGrip;                // 0x0c0
    fx32 GroundSlowInc;             // 0x0c4
    fx32 GroundSlowdown;            // 0x0c8
    fx32 d_FlatAngCos;              // 0x0cc = cos(FlatAng degrees)
    fx32 d_MagicGripSpeed;          // 0x0d0 = MagicGripSpeed mph -> m/s
    fx32 Mass;                      // 0x0d4
    fx32 d_Inertia;                 // 0x0d8 = Mass * RotInertia
    fx32 CarCarShunt;               // 0x0dc
    fx32 CollElasticity;            // 0x0e0
    fx32 WallTurnFactor;            // 0x0e4
    fx32 WallTurnMax;               // 0x0e8
    fx32 WallSlowdown;              // 0x0ec
    fx32 WallEnforcer;              // 0x0f0
    s32 RPMMin;                     // 0x0f4 (int)
    s32 RPMMax;                     // 0x0f8 (int)
    s32 NumGears;                   // 0x0fc (int)
    fx32 GearChangeGap;             // 0x100
    fx32 d_TopSpeed[7];             // 0x104 = TopSpeedRev, TopSpeed1..6 mph -> m/s
    fx32 TimeRev;                   // 0x120
    fx32 Time[6];                   // 0x124 Time1..Time6
    fx32 WheelTravelTop;            // 0x13c
    fx32 WheelTravelBot;            // 0x140
    fx32 d_WheelTurnVisual;         // 0x144 = WheelTurnVisual degrees -> radians
    fx32 WheelTurnVisual;           // 0x148
    std::vector<dsCarParamDesc> params; // 0x14c (data, count, capacity)
    fx32 CollPushAway;              // 0x158
    fx32 unk_15c;                   // 0x15c
    fx32 RearWheel[3];              // 0x160
    fx32 FrontWheel[3];             // 0x16c
    fx32 RearRadius;                // 0x178
    fx32 FrontRadius;               // 0x17c
    fx32 FlatAng;                   // 0x180
    fx32 MagicGripSpeed;            // 0x184
    fx32 SkidSpeedEffect;           // 0x188
    fx32 SteerFirmUp;               // 0x18c
    fx32 RotInertia;                // 0x190
    fx32 DonutSpeed;                // 0x194
    fx32 FakeRollFactor;            // 0x198
    fx32 FakePitchFactor;           // 0x19c
    fx32 TopSpeedRev;               // 0x1a0
    fx32 TopSpeed[6];               // 0x1a4 TopSpeed1..TopSpeed6
    fx32 Tilt;                      // 0x1bc

    dsCarParams();                  // 0x0212ddd8 (the port zeroes the fields the original leaves uninitialised)
    dsCarParams(const dsCarParams &o);            // 0x0212da88
    dsCarParams &operator=(const dsCarParams &o);

    void Load(const dsLuaObject &table, const char *carName);   // 0x0212cc58
    void RegisterParams();                                      // 0x0212cd74
    void RegisterParam(s32 *value, const char *name, s32 type, u8 editable, s32 min, s32 max); // 0x0212cbb8
    void ComputeDerived();                                      // 0x0212c9ec
};

// Owner of the table (ov003). Its Load is 0x0212bed0.
class dsCarParamsTable {
public:
    void Load();
    void Clear();                                   // 0x0212c47c
    dsCarParams &Get(const std::string &car);       // 0x0212c2c8 (find or insert)
    const std::map<std::string, dsCarParams> &All() const { return m_cars; }
    const std::string &SaveTime() const { return m_saveTime; }

private:
    bool m_unk04 = false;                           // +0x04, cleared by Load
    std::string m_saveTime;                         // +0x08: global SaveTime ("Firebrand19Feb")
    std::map<std::string, dsCarParams> m_cars;      // +0x14
};
