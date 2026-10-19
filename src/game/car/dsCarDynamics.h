// Rigid-body car simulation (ov003 0x021220a0..0x0212bed0, 0x554 bytes, no vtable). One per vehicle (+0x8c of
// dsVehicle); stepped by dsCarDynamicsList_Update (0x0212bd68) from the race manager. It starts with its own copy of
// the car's dsCarParams, then the state below (offsets of the original; see the slice-3 notes for the fields the
// step uses that are not declared yet).
//
// Ported so far: construction, reset, placement, wheels/engine/body box setup. The step (0x021222fc) and its
// callees are the next work item.
#pragma once

#include <string>
#include <vector>

#include "car/dsCarEngine.h"
#include "car/dsCarParams.h"
#include "car/dsCarWheel.h"
#include "collision/dsCollisionWorld.h"
#include "nitro.h"

// What the dynamics reads from the car's visual model (dsCarModel, ov003 0x020fd914): wheel positions from the
// parameters and the body box of the lod1 model.
struct dsCarModelGeometry {
    VecFx32 bodySize = {};          // model +0xa4 (W, H, D)
    VecFx32 bodyCentre = {};        // model +0xb0 (0, H/2, 0)
    VecFx32 frontWheel = {};        // model +0xd8 = FrontWheel
    VecFx32 rearWheel = {};         // model +0xe4 = RearWheel

    // ov003 0x020fd914 (geometry part): "Cars/<car>/<skin>/lod1"; the snow car's box is trimmed.
    bool Load(const std::string &car, int skin, const dsCarParams &p);
};

class dsCarDynamics {
public:
    dsCarDynamics(const std::string &car, const dsCarModelGeometry *model); // ov003 0x0212b3c4
    ~dsCarDynamics();

    void LoadParams(const dsCarParams &table);     // ov003 0x0212696c: copy of the table entry, then BuildWheels
    void BuildWheels();                            // ov003 0x021266ec
    void ReadModelBox();                           // ov003 0x02126628
    void Reset();                                  // ov003 0x0212b0b0
    void Place(const MtxFx43 &m, bool updateVisual, bool warmUp); // ov003 0x021279e8

    dsCarParams p;                                 // +0x000 (0x1c0)
    MtxFx43 matrix;                                // +0x1c0 rows: right, up, forward, position
    MtxFx43 prevMatrix;                            // +0x1f0
    VecFx32 groundNormal = {0, FX32_ONE, 0};       // +0x220
    fx32 groundD = 0;                              // +0x22c
    s32 groundedWheels = 0;                        // +0x230
    VecFx32 push = {};                             // +0x234
    fx32 airTime = 0;                              // +0x244
    std::vector<dsCollisionHit> wheelHits[4];      // +0x2b0
    std::vector<dsCollisionHit> bodyHits[6];       // +0x2e0
    fx32 bounce = 0;                               // +0x340
    fx32 invFrontRadius = FX32_ONE;                // +0x368
    fx32 invRearRadius = FX32_ONE;                 // +0x36c
    fx32 forwardSpeed = 0;                         // +0x370
    fx32 unk374 = 0;                               // +0x374
    VecFx32 velocity = {};                         // +0x378
    fx32 speed = 0;                                // +0x384 |velocity|
    fx32 maxSpeed = 0;                             // +0x388 engine top speed
    fx32 wheelSpin[2] = {};                        // +0x38c
    std::vector<dsCarWheel *> wheels;              // +0x394: 0 front, 1 front mirrored, 2 rear, 3 rear mirrored
    fx32 steer = 0;                                // +0x3a0
    fx32 unk3a4 = 0;                               // +0x3a4
    VecFx32 angularVelocity = {};                  // +0x3a8
    MtxFx43 lastMatrix;                            // +0x3b4
    fx32 steerAccum = 0;                           // +0x3e4
    s32 orthoRow = 0;                              // +0x3fc
    fx32 unk400 = 0;                               // +0x400
    s32 inputMode = 0;                             // +0x420 0 none, 1/2 replay, 3 player
    fx32 wheelTurn = 0;                            // +0x454
    fx32 skid = 0;                                 // +0x458
    const dsCarModelGeometry *model;               // +0x45c
    dsCarEngine *engine = nullptr;                 // +0x460
    std::string name;                              // +0x464 car name ("coa")
    fx32 throttle = 0;                             // +0x468
    bool active = true;                            // +0x474
    fx32 dt = 0;                                   // +0x478
    fx32 invSteerOnset = 0;                        // +0x4b0
    fx32 gravity = 0;                              // +0x4c0
    VecFx32 halfSize = {};                         // +0x4c4
    fx32 probeLength = 0;                          // +0x4d0 wheel probe length
    fx32 wheelLowering = 0;                        // +0x4e4 (-0.04)
    bool warmUp = false;                           // +0x520: run warmUpSteps steps at the next Step
    s32 warmUpSteps = 0x14;                        // +0x524
    VecFx32 bodyCentre = {};                       // +0x534
    fx32 minHeight;                                // +0x544 (sea level on Coast/Island)
    fx32 speedLimit;                               // +0x54c
    bool clampToWorld = false;                     // +0x551 Stadium and Snow: keep x, z inside the arena
};
