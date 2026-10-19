// Rigid-body car simulation (ov003 0x021220a0..0x0212bed0, 0x554 bytes, no vtable). One per vehicle (+0x8c of
// dsVehicle); stepped by dsCarDynamicsList_Update (0x0212bd68) from the race manager. It starts with its own copy of
// the car's dsCarParams, then the state below (offsets of the original; see the slice-3 notes for the fields the
// step uses that are not declared yet).
//
// Ported so far: construction, reset, placement, wheels/engine/body box setup. The step (0x021222fc) and its
// callees are the next work item.
#pragma once

#include <functional>
#include <random>
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

// Driver input, 0x1c bytes at +0x404 (set by 0x02122b10, which also selects input mode 3).
struct dsCarInput {
    fx32 accel = 0;                 // +0x404 0..1
    fx32 brake = 0;                 // +0x408 0..1
    fx32 steer = 0;                 // +0x40c -1..1
    fx32 pitch = 0;                 // +0x410 -1..1 (second steering axis, air control)
    u8 flag = 0;                    // +0x414
    s32 unk418 = 0, unk41c = 0;     // +0x418 +0x41c
};

// Firebrand's random generator (main 0x0205af30, 0x14 bytes): MT19937, seeded by Seed (0x0205aee4).
struct dsRandom {
    std::mt19937 mt{0};
    void Seed(u32 s) { mt.seed(s); }                       // 0x0205aee4
    // lo + (12 random bits * (hi - lo) + 0x800) >> 12. 0x0205ae3c
    s32 Range(s32 lo, s32 hi)
    {
        u32 y = mt() & 0xfff;
        return lo + (s32)(((s64)y * (s64)(hi - lo) + 0x800) >> 12);
    }
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
    void SetInput(const dsCarInput &in);           // ov003 0x02122b10
    // Steering and throttle smoothing, forward speed, speed limit. ov003 0x02127af8
    void UpdateInputs();

    // Collision pass (dsCarCollide.cpp)
    void Collide();                                // ov003 0x02124fa0
    void ResetContacts();                          // ov003 0x02122730
    void AddWheelProbe(int i);                     // ov003 0x02124d54
    void AddBodyProbe(int i, const VecFx32 &from, const VecFx32 &to, bool world); // ov003 0x021245f4
    void WheelContact(int i);                      // ov003 0x02124910
    void BodyContact(int i, const VecFx32 &from, const VecFx32 &to, bool roof, bool world); // ov003 0x02123fac
    void BodyImpulse();                            // ov003 0x021233d0

    void Forces();                                 // ov003 0x02127f1c (dsCarForces.cpp)

    void UpdateWheels();                           // ov003 0x02127374 (dsCarStep.cpp)
    // Display side of the wheels: steering angle, spin, surfaces and the wheel matrices. ov003 0x02126d10
    void UpdateVisual();
    MtxFx43 wheelMatrix[4];                        // spin (+ steer for the front pair) at the hub, world space
    MtxFx43 wheelSideMatrix[4];                    // same without the spin
    void Step();                                   // ov003 0x021222fc (dsCarStep.cpp)
    void Surfaces();                               // ov003 0x02122b54 (dsCarSurfaces.cpp)
    void Drive();                                  // ov003 0x0212a9c0 (dsCarGrip.cpp)
    void Grip();                                   // ov003 0x02129f04 (dsCarGrip.cpp)
    fx32 GripLimit();                              // ov003 0x0212a848
    void Integrate();                              // ov003 0x02129098 (dsCarIntegrate.cpp)
    // Rotates m by angular velocity w over dt about pivot (car space). ov003 0x02129ce4
    void Rotate(const VecFx32 &w, const VecFx32 &pivot, MtxFx43 &m);

    // Booster pieces (material 7): rotation (0..3) of the track piece at a world position, -1 when none. The
    // original reads the race grid (main 0x0208d404 on *0x020d6628, piece +0x74).
    std::function<int(const VecFx32 &)> pieceRotationAt;
    bool *warmingUp = nullptr;                     // 0x021378fc set while the warm-up steps run

    // Values the original keeps in ov003 statics during a step.
    VecFx32 s_centre = {};                         // 0x02137944 body centre in world space
    VecFx32 s_prevVelocity = {};                   // 0x02137938 velocity at the start of the step
    bool s_resting = false;                        // 0x02137900 nearly stopped, no skid: position frozen
    VecFx32 s_driveDir = {};                       // 0x02137950 (set by Drive 0x0212a9c0)
    fx32 s_gripScale = FX32_ONE;                   // 0x02136d80 cos^2 between gravity and the ground
    fx32 s_donutBlend = 0;                         // 0x02137920 (1 - speed / DonutSpeed) * skid * throttle
    fx32 s_steerScale = FX32_ONE;                  // 0x02137924 1 / (1 + |speed| SteerFirmUp)

    dsCarParams p;                                 // +0x000 (0x1c0)
    MtxFx43 matrix;                                // +0x1c0 rows: right, up, forward, position
    MtxFx43 prevMatrix;                            // +0x1f0
    VecFx32 groundNormal = {0, FX32_ONE, 0};       // +0x220
    fx32 groundD = 0;                              // +0x22c (n.p + d = 0)
    // Contact block +0x230 (cleared by ResetContacts 0x02122730 at each collision pass, except push and airTime).
    s32 groundedWheels = 0;                        // +0x230
    VecFx32 push = {};                             // +0x234 position correction for the next integration
    bool touched = false;                          // +0x240 any contact this step
    fx32 airTime = 0;                              // +0x244
    VecFx32 extVelSum = {};                        // +0x248 external impulses (written by car/car contacts)
    VecFx32 extAngSum = {};                        // +0x254
    s32 extCount = 0;                              // +0x260
    VecFx32 extPushSum = {};                       // +0x264
    VecFx32 hitVelSum = {};                        // +0x270 sum of the body probe end velocities
    s32 bodyHitCount = 0;                          // +0x27c
    VecFx32 hitNormalSum = {};                     // +0x280
    VecFx32 hitPointSum = {};                      // +0x28c
    VecFx32 hitOffsetSum = {};                     // +0x298 probe ends relative to the body centre
    bool roofHit = false;                          // +0x2a4 roof probe hit while upside down
    bool impulseDone = false;                      // +0x2a5
    fx32 penetration = 0;                          // +0x2a8
    fx32 impulseSum = 0;                           // +0x2ac
    std::vector<dsCollisionHit> wheelHits[4];      // +0x2b0
    std::vector<dsCollisionHit> bodyHits[6];       // +0x2e0
    fx32 bodyProbeLength[6] = {};                  // +0x328
    fx32 bounce = 0;                               // +0x340 raises the body probes after a landing
    bool aheadPushed = false;                      // +0x344
    VecFx32 prevCentre = {};                       // +0x348 body centre at the previous pass (ahead probe origin)
    bool aheadHit = false;                         // +0x354 the ahead probe went into a wall
    VecFx32 aheadPush = {};                        // +0x358
    fx32 invFrontRadius = FX32_ONE;                // +0x368
    fx32 invRearRadius = FX32_ONE;                 // +0x36c
    fx32 forwardSpeed = 0;                         // +0x370
    fx32 lateralSpeed = 0;                         // +0x374 velocity along the ground-projected right axis
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
    dsRandom random;                               // +0x3e8
    s32 orthoRow = 0;                              // +0x3fc
    fx32 unk400 = 0;                               // +0x400
    dsCarInput input;                              // +0x404
    s32 inputMode = 0;                             // +0x420 0 none, 1/2 replay, 3 player
    fx32 wheelTurn = 0;                            // +0x454
    fx32 skid = 0;                                 // +0x458
    const dsCarModelGeometry *model;               // +0x45c
    dsCarEngine *engine = nullptr;                 // +0x460
    std::string name;                              // +0x464 car name ("coa")
    fx32 throttle = 0;                             // +0x468
    bool active = true;                            // +0x474
    fx32 dt = 0;                                   // +0x478
    fx32 roughTime = 0;                            // +0x4b8 distance on rough surfaces since the last jolt
    fx32 driveForce = 0;                           // +0x4b4 engine force of the last update
    VecFx32 driveDir = {};                         // +0x47c drive force direction (forward on the ground)
    fx32 fakeRoll = 0;                             // +0x488 smoothed yaw rate (visual roll)
    fx32 fakeRollOut = 0;                          // +0x48c fakeRoll * FakeRollFactor
    fx32 fakePitch = 0;                            // +0x490 smoothed throttle pitch (visual)
    fx32 angDamp[3] = {(fx32)0xffc19001, 0, 0};   // +0x498 angular damping per car axis (-999: not set yet)
    s32 surfaceCount[2] = {};                      // +0x4a4 +0x4a8 wheels on ..., wheels on grass
    fx32 grip = 0;                                 // +0x4ac surface grip (Surfaces 0x02122b54)
    fx32 invSteerOnset = 0;                        // +0x4b0
    fx32 gravity = 0;                              // +0x4c0
    VecFx32 halfSize = {};                         // +0x4c4
    fx32 probeLength = 0;                          // +0x4d0 wheel probe length
    VecFx32 wallVec = {};                          // +0x4d8 wall deflection, |wallVec| <= WallTurnMax
    fx32 wheelLowering = 0;                        // +0x4e4 (-0.04)
    fx32 turboTimer = 0;                           // +0x4ec
    fx32 groundTime = 0;                           // +0x50c
    VecFx32 gravityDir = {0, (fx32)0xfffff001, 0}; // +0x4f0 (booster pieces turn it)
    fx32 alignTime = 0;                            // +0x4fc after a booster: velocity kept in the ground plane
    bool boosterAlign = false;                     // +0x500 on a booster: keep the spin about the ground normal
    fx32 rampPitch = 0;                            // +0x504 nose-down torque time left after a ramp
    fx32 landTimer = 0;                            // +0x508 extra damping after a fast fall
    bool normalForces = false;                     // +0x528 suspension always along the contact normal
    fx32 wheelBlend = 0;                           // +0x52c -> 1 while a wheel is pushed past WheelTravelTop
    bool skipAheadProbe = false;                   // +0x530 (cleared at the end of each step)
    bool stuck = false;                            // +0x540 no wheel down, resting on the body
    fx32 stuckTime = 0;                            // +0x548
    bool respawnRequest = false;                   // owner vehicle +0xd5, set after 2 s stuck
    bool noThrottle = false;                       // +0x4e8 (surface 8)
    VecFx32 acceleration = {};                     // +0x514 (velocity change / dt over the step)
    bool warmUp = false;                           // +0x520: run warmUpSteps steps at the next Step
    s32 warmUpSteps = 0x14;                        // +0x524
    VecFx32 bodyCentre = {};                       // +0x534
    fx32 minHeight;                                // +0x544 (sea level on Coast/Island)
    fx32 speedLimit;                               // +0x54c
    bool clampedToWorld = false;                   // +0x550 the position was clamped this step
    bool clampToWorld = false;                     // +0x551 Stadium and Snow: keep x, z inside the arena
};
