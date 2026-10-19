// One wheel of dsCarDynamics (0x5c bytes, ov003 0x0212e010..0x0212e2a0): suspension travel, contact and spin.
#pragma once

#include "nitro.h"

struct dsCarWheel {
    dsCarWheel();                                  // ov003 0x0212e2a0
    void Reset();                                  // ov003 0x0212e1f8
    void Init(const VecFx32 &pos, const VecFx32 &probe, fx32 travel, fx32 radius, fx32 travelMin,
              fx32 travelMax);                     // ov003 0x0212e238
    void Lower(fx32 d);                            // ov003 0x0212e060
    void SetCompressionTarget(fx32 v);             // ov003 0x0212e07c: clamped to +-travel / 2
    void UpdateCompression();                      // ov003 0x0212e010
    void Spin(fx32 speed);                         // ov003 0x0212e0b8
    // Probe origin in world space: probe position (+ dx along the axle) through the car matrix. 0x0212e13c
    VecFx32 ProbeOrigin(const MtxFx43 &m, fx32 dx) const;
    // Hub position for the display, with the current compression. 0x0212e198
    VecFx32 HubPosition(const MtxFx43 &m) const;

    bool grounded = false;         // +0x00
    VecFx32 contactNormal = {};    // +0x04
    VecFx32 contactPoint = {};     // +0x10
    s32 hit = 0;                   // +0x1c index of the nearest hit in the probe's hit list
    VecFx32 pos = {};              // +0x20 hub, car space
    VecFx32 probe = {};            // +0x2c probe start, car space
    fx32 target = 0;               // +0x38 compression wanted this step
    fx32 compression = 0;          // +0x3c follows target, extends at 2.0/s
    fx32 clamped = 0;              // +0x40 compression within [travelMin, travelMax]
    fx32 travel = 0;               // +0x44 WheelTravelPhys
    fx32 radius = 0;               // +0x48 hub height
    fx32 spin = 0;                 // +0x4c angle, [0, 2pi)
    fx32 travelMin = 0;            // +0x50 WheelTravelBot
    fx32 travelMax = 0;            // +0x54 WheelTravelTop
    s32 surface = -1;              // +0x58 material under the wheel (-1: none)
};
