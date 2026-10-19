// Engine and gearbox of a car (0x64 bytes, ov003 0x0212ee38..0x0212f248), owned by dsCarDynamics (+0x460).
// Gears come from dsCarParams: index 0 is reverse (TopSpeedRev, TimeRev), 1..NumGears-1 the forward gears
// (TopSpeed1.., Time1..). The gear number is -1 (reverse), 0 (neutral) or 1..NumGears-1.
#pragma once

#include <vector>

#include "nitro.h"

struct dsCarParams;

struct dsCarGear {                 // 0x14 bytes
    fx32 ratio = 0;                // +0x00 speed per rpm: TopSpeed / shift-up rpm (last gear: / RPMMax)
    fx32 unk04 = 0;                // +0x04 (cleared by Reset; torque is scaled by 1 - unk04)
    fx32 torque = 0;               // +0x08 TopSpeed * ratio * (1 - start/shift) / Time
    fx32 startRpm = 0;             // +0x0c rpm at the previous gear's top speed
    fx32 downRpm = 0;              // +0x10 shift-down rpm = startRpm - 500 (gears 2+)
};

class dsCarEngine {
public:
    explicit dsCarEngine(const dsCarParams &p);    // ov003 0x0212ee38
    void Setup(const dsCarParams &p);              // ov003 0x0212edc8
    void BuildGears(const dsCarParams &p);         // ov003 0x0212ea78
    void Reset();                                  // ov003 0x0212ea0c
    fx32 MaxSpeed() const;                         // ov003 0x0212e590
    fx32 Torque(fx32 rpm) const;                   // ov003 0x0212e5c8
    void SetGear(s32 gear);                        // ov003 0x0212e6e8
    // Per step: wheel speed, throttle (-1..1, negative = brake/reverse) and a manual shift request (-1, 0, 1).
    void Update(fx32 speed, fx32 throttle, s32 shift); // ov003 0x0212e70c

    fx32 rpmMin;                   // +0x00 RPMMin << 12
    fx32 rpmMax;                   // +0x04 RPMMax << 12
    bool automatic = true;         // +0x08
    fx32 rpmShiftUp;               // +0x0c RPMMax - 100
    bool limiter = true;           // +0x10 torque fades above rpmShiftUp
    std::vector<dsCarGear> gears;  // +0x14 (count +0x18)
    s32 gear = 1;                  // +0x20
    fx32 speed = 0;                // +0x24 wheel speed of the last update
    fx32 force = 0;                // +0x28 drive force of the last update
    fx32 rpm = 0;                  // +0x2c
    fx32 throttle = 0;             // +0x30 throttle after the gear logic (>= 0)
    fx32 brake = 0;                // +0x34
    fx32 stopTime = 0;             // +0x38 time spent nearly stopped (automatic reverse/forward switch)
    fx32 shiftTime = 0;            // +0x3c no drive force while > 0
    fx32 unk40 = 0;                // +0x40 counts down
    fx32 unk44[3] = {};            // +0x44
    fx32 unk50 = 0;                // +0x50 fraction of the top gears that are locked out
    fx32 unk54 = 0;                // +0x54 lowers the shift points (x (1 - 0.75 unk54))
    fx32 rawThrottle = 0;          // +0x5c
    fx32 gearChangeGap;            // +0x60 GearChangeGap

private:
    void UpdateGear(fx32 throttle, s32 shift);     // ov003 0x0212e308
    fx32 BrakeInput(fx32 throttle) const;          // ov003 0x0212e4fc
};
