// The car's light intensities: the light struct of the car model (*(model + 0x18), created by ov003 0x020fd914
// with the three bytes at 0), read when the car is drawn by the material controllers of the "blight" (+4, red),
// "rlight" (+5, white) and "hlight" (+6, white) materials (main 0x0206b0xx, itcm 0x01fff814 / 0x01fff858): a
// light's shapes are drawn only while its byte is not 0, with that byte as the polygon alpha.
#pragma once

#include "nitro.h"

struct dsCarLights {
    s8 level[3] = {0, 0, 0};        // +4 brake, +5 reverse, +6 head (never lit in a race)
    u8 brakeMin = 0, brakeMax = 0x10; // car visual +0x104 / +0x105 (set by ov003 0x020fd914)

    void Reset() { level[0] = level[1] = level[2] = 0; }
    void Brake(bool on);            // ov003 0x020fcdf0
    void Reverse(bool on);          // ov003 0x020fcda4
    // The human player's update (ov003 0x0210ed9c): the brake lights show while braking forwards or accelerating
    // in reverse (the pad's accelerate +0x98 / brake +0x99 bytes), the reverse lights while the gear is negative.
    void Update(bool accelerate, bool brake, s32 gear);
};
