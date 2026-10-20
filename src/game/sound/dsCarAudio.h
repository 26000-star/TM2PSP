// Car sounds of the race (*0x02137740, ov003 0x020f68e0..0x020f9440, 0x320 bytes): the engine as three looping
// layers (CAR_IDLE, CAR_LOW, CAR_MED of SEQARC_CAR_<env>) whose volume and pitch follow a "sound RPM", the rear
// wheels' skid (SKID_TARMAC / SKID_DIRT), collisions (COLLISION_METAL_1..3, COLLISION_WOOD_1) and the horn, all as
// dsSound3D sources at the car (min distance 5, max 100: +0x314 / +0x318).
//  - The sound RPM (0x020f799c) is not the engine's: the car speed, smoothed by 41 / 4096 per frame, picks a gear
//    among the car's TopSpeed thresholds (vehicles: TopSpeedRev, TopSpeed1..6, kept at +0x2dc) and the RPM runs
//    from 7000 to 9000 across the gear, followed at 0.5 (0.2 when falling, above the first gear), minus 800.
//  - config/caraudio (0x020f8c90) gives per car MaxRPM, ReverseGear and per gear (Gears, or Idle / RevLow / RevMed
//    for all gears) each layer's MaxVolume and its StartRPM < SustainRPM < DecayRPM < ReleaseRPM envelope over the
//    RPM; a layer's pitch is 3 x its volume (1/64 semitone).
//  - Only one car is heard (*0x02134e58 = 1 car entry: the player's).
#pragma once

#include <string>
#include <vector>

#include "car/dsCarDynamics.h"
#include "nitro.h"

struct dsCarParams;

class dsCarAudio {
public:
    struct Source {                 // 0x20 bytes (ov003 0x020f8c28)
        std::string name;           // +0x00
        u32 handle[2];              // +0x04: a volume above 127 plays the sound twice
        bool loop = false;          // +0x0c bit 0
        int volume = 0;             // +0x10
        VecFx32 offset = {};        // +0x14 from the car
    };
    struct Layer {                  // 5 ints per layer and gear (+0x60 of the engine block)
        int maxVolume = 0, start = 0, sustain = 0, decay = 10000, release = 10000;
    };
    struct CarEntry {               // 0x6c bytes per heard car (+0x1c)
        int rpm = 0;                // +0x04
        int gear = 0;               // +0x08
        int pitch = 0;              // +0x0c Doppler pitch
        VecFx32 pos = {};           // +0x10
        fx32 speed = 0;             // +0x5c smoothed speed
        bool stopped = true;        // +0x60
        int gearCopy = 0;           // +0x68
    };

    static dsCarAudio *Get();
    static void Destroy();

    // 0x020f89ac (from the game audio's race start 0x0208c04c): sources, config/caraudio of `car`, the car's gear
    // thresholds. False when config/caraudio has no entry for the car.
    bool Init(const std::string &car, const dsCarParams &params);
    void Reset();                   // 0x020f8700: every source released
    void PlayerReset() { m_byte0c = 0; } // 0x020f7e64 (from the player's reset)
    // 0x020f7f58, once per frame: the listener (camera matrix), the master fade, the engine of `car`, the
    // positions of the skid / collision / horn sources.
    void Update(const MtxFx43 &camera, const dsCarDynamics &car);
    // The sound part of the human player's update (ov003 0x0210ed9c): skids from the rear wheels and the lateral
    // speed (0x020f7b4c), collisions from the body contacts (0x020f6e38) with a 0.5 s cool-down.
    void PlayerUpdate(const dsCarDynamics &car);
    void Horn();                    // 0x020f6dc8 (dsRace_Update, on the horn button)
    void FadeOut(int frames);       // 0x020f6d70
    void FadeIn(int frames);        // 0x020f6da0

    // config/caraudio, for the checks (tm_cfgdump --caraudio)
    int maxRpm = 0, reverseGear = 0;  // +0x1f4, +0x1f8
    Layer gears[6][3];                // +0x8c
    fx32 gearSpeed[7] = {};           // +0x2dc: TopSpeedRev, TopSpeed1..6
    fx32 gearTime[7] = {};            // +0x2f8: TimeRev, Time1..6 (not used by the sound)
    const CarEntry &Player() const { return m_player; }
    const Source &EngineLayer(int i) const { return m_engine[i]; }

private:
    void Start(const VecFx32 &pos, Source &s, bool loop);    // 0x020f7000
    void Release(Source &s);
    void SoundRpm(const dsCarDynamics &car, CarEntry &e);    // 0x020f799c
    void Engine(const dsCarDynamics &car);                   // 0x020f75cc
    void Skid(bool skid, bool tarmac, bool dirt);            // 0x020f7b4c
    void Collision(int index, const VecFx32 &offset, fx32 speed, fx32 impact); // 0x020f6e38
    void PlaceSource(Source &s);

    int m_master = 0x7f, m_target = 0x7f, m_step = 0;  // +0x00, +0x04, +0x08
    u8 m_byte0c = 0;                                   // +0x0c
    VecFx32 m_listenerPos = {};                        // +0x10
    CarEntry m_player;                                 // +0x28 (the last car entry)
    Source m_engine[3];                                // +0x2c: CAR_IDLE, CAR_LOW, CAR_MED
    Source m_skid[2];                                  // +0x1fc: SKID_TARMAC, SKID_DIRT
    Source m_collision[4];                             // +0x23c
    Source m_horn;                                     // +0x2bc
    int m_minDist = 5, m_maxDist = 100;                // +0x314, +0x318
    fx32 m_collisionWait = 0;                          // player +0xa4
    dsRandom m_random;                                 // the game's global random (*0x0210f314 +0x20)
    bool m_ready = false;
};
