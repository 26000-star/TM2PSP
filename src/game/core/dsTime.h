// Frame time and the small time-based helpers built on it.
#pragma once

#include "nitro.h"

// Duration of one simulation tick in seconds (fx32), *0x020d6a78: a constant 1/60 s (0x44) set by a static
// initialiser. The race update runs the car step (ov003 0x0212bd68) and its other per-tick work once per elapsed
// tick, so a frame drawn every second VBlank advances the simulation by two 1/60 s ticks.
extern fx32 g_dsFrameTime;

// Moves *v towards target by rate * frame time, without overshooting; true once it is reached. main 0x020b29e8
bool dsApproach(fx32 *v, fx32 target, fx32 rate);
