// 3D sound sources (*0x020c8e8c, main 0x0205f820..0x020606d0, vtable 0x020bd6f0): up to 64 named sound effects
// placed in the world and heard from a listener (the camera), through dsSoundManager.
//  - a source (0x34 bytes): +0x08 position, +0x14 name, +0x18 / +0x1c min and max distance (whole units),
//    +0x20 2^30 / (max - min), +0x24 the manager's handle, +0x28 its own handle (3 << 29 | slot << 21 | serial),
//    +0x2c bit 0 looping / bit 1 not started yet / bit 2 wave, +0x2d its volume, +0x2e / +0x2f volume and pan heard;
//  - the update (0x0206032c): each source's distance in listener space gives the volume (full up to min, linear to
//    0 at max), its direction a pan and a pan-law attenuation (the DTCM tables 0x027e00ec / 0x027e0130); the sources
//    are sorted by that volume and at most 8 play; a one-shot source ends with its sound or out of range, a looping
//    one is stopped out of range and restarted in range.
#pragma once

#include <string>

#include "nitro.h"

class dsSound3D {
public:
    enum { SOURCES = 64, MAX_PLAYING = 8 };

    static dsSound3D *Get();
    static void Destroy();

    void SetListener(const MtxFx43 &cameraMatrix);                  // +0x0c (the camera's world matrix)
    u32 Create(const char *name, const VecFx32 &pos, int minDist, int maxDist, bool loop); // +0x10 / +0x18
    u32 Release(u32 h, fx32 fadeSeconds);                          // +0x20, returns the invalid handle
    void ReleaseAll();                                             // +0x24
    void SetPosition(u32 h, const VecFx32 &pos);                   // +0x28 / +0x30
    void SetVolume(u32 h, int volume);                             // +0x34
    void SetPitch(u32 h, int pitch);                               // +0x38
    bool IsValid(u32 h) const;                                     // +0x3c
    u32 InvalidHandle() const;                                     // +0x40
    void Update();                                                 // +0x44, once per game frame
    void SetPaused(bool p) { m_paused = p; }   // the update is skipped while the game is paused (*0x027e06cc +0x10c)

private:
    struct Source {
        bool used = false;
        VecFx32 pos = {};
        std::string name;
        int minDist = 0, maxDist = 0;
        s32 invRange = 0;
        u32 sound = 0, handle = 0;
        bool loop = false, fresh = false;
        s8 volume = 0, heardVolume = 0, heardPan = 0;
    };

    Source *Find(u32 h);
    void Compute();      // 0x02060118
    void StartStop();    // 0x0205ffb4

    Source m_src[SOURCES];
    MtxFx43 m_listener = {};   // +0xd0c, the inverse of the camera matrix
    u32 m_serial = 1;
    int m_playing = 0;         // +0x04
    bool m_paused = false;
};
