#include "dsCarLights.h"

// @port ov003:020fcdf0 done (the visual's +0xd0 copy of the flag is not kept)
void dsCarLights::Brake(bool on)
{
    int v = level[0] + (on ? 3 : -3);
    if (v > brakeMax)
        v = brakeMax;
    if (v < brakeMin)
        v = brakeMin;
    level[0] = (s8)v;
}

// @port ov003:020fcda4 done (the visual's +0xd1 copy of the flag is not kept)
void dsCarLights::Reverse(bool on)
{
    int v = level[1] + (on ? 3 : -3);
    if (v > 0x18)
        v = 0x18;
    if (v < 0)
        v = 0;
    level[1] = (s8)v;
}

// @port ov003:0210ed9c partial (the lights only; the rest of the human player's update is in dsRaceProgress and
// the car step)
void dsCarLights::Update(bool accelerate, bool brake, s32 gear)
{
    Brake((brake && gear >= 0) || (accelerate && gear < 0));
    Reverse(gear < 0);
}
