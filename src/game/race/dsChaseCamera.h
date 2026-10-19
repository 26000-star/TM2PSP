// Race chase camera (class with vtable 0x020c00b8, constructor main 0x0208fe10, 0x100 bytes), configured by
// Scripts/config/cameras (global `cameras`, one table per environment, keys Chase1* / Chase2*).
// The update (main 0x0208e694, camera vtable +0x1c) runs once per frame, after the car steps, with the local
// player's display matrix (dynamics +0x1f0, the car entity's +0x58), velocity and contact state.
#pragma once

#include <vector>

#include "nitro.h"

class dsCarDynamics;

// Critically damped spring towards a target (main 0x020abb68 reset, 0x020abc38 constructor, 0x020abb78 step).
struct dsSpring {
    fx32 k = 0;      // +0x00 stiffness
    fx32 d = 0;      // +0x04 damping, from the stiffness table (main 0x020abc68)
    fx32 vel = 0;    // +0x08
    fx32 pos = 0;    // +0x0c

    void Init(fx32 stiffness);                         // main 0x020abc38
    void Reset(fx32 v) { pos = v; vel = 0; }           // main 0x020abb68
    fx32 Step(fx32 target);                            // main 0x020abb78 (uses the tick time)
    static fx32 Damping(fx32 stiffness);               // main 0x020abc68: table 0x020c1d08 (30 pairs)
};

class dsChaseCamera {
public:
    // kind 1 = Chase1, 2 = Chase2 (constructor argument, +0x04).
    explicit dsChaseCamera(int kind);                  // main 0x0208fe10
    bool Load(const char *env);                        // main 0x0208fa18
    void Reset();                                      // main 0x0208fd68 (vtable +0x18)
    void Update(const dsCarDynamics &car);             // main 0x0208e694 (vtable +0x1c)

    int kind;                       // +0x04
    bool active = true;             // +0x08 (vtable +0x08 0x0208e51c; the fade +0x0c/+0x10 is not used here)
    fx32 fov = 0x37000;             // +0x2c degrees (Chase FOV)
    MtxFx43 matrix;                 // +0x38 rows: right, up, forward (towards the target), position
    bool init = false;              // +0x68
    fx32 height = 0;                // +0x6c Height
    fx32 airBlend = 0;              // +0x70
    fx32 distance = 0;              // +0x74 Distance
    fx32 lookAtHeight = 0;          // +0x78 LookAtHeight
    fx32 tiltTightness = 0;         // +0x7c TiltTightness
    VecFx32 pos = {};               // +0x80
    VecFx32 up = {0, FX32_ONE, 0};  // +0x8c smoothed up
    fx32 reverse = (fx32)0xfffff001; // +0x98 -1 behind the car .. +1 in front (reversing)
    fx32 dirTightness = 0;          // +0x9c DirTightness
    fx32 distInc = 0, distDec = 0;  // +0xa0 +0xa4 DistInc / DistDec
    dsSpring distSpring;            // +0xa8 DistTightness
    VecFx32 prevGround = {0, FX32_ONE, 0}; // +0xb8 car ground normal of the last update
    std::vector<fx32> pitchHistory; // +0xc4 20 samples
    int pitchIndex = 0;             // +0xd0
    fx32 roll = 0;                  // +0xd4
    bool boost = false;             // +0xd8
    int rollMode = 2;               // +0xdc RollMode
    VecFx32 targetUp = {0, FX32_ONE, 0}; // +0xe0
    fx32 flipBlend = 0;             // +0xec
    VecFx32 flipUp = {0, FX32_ONE, 0};   // +0xf0
};

// Look-at frame (itcm 0x01ffabfc): forward = normalise(target - pos), right = normalise(up x forward),
// up' = forward x right, translation pos.
void dsMtx43_LookAt(MtxFx43 *m, const VecFx32 &pos, const VecFx32 &target, const VecFx32 &up);
