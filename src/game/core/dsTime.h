// Frame time and the small time-based helpers built on it.
#pragma once

#include "nitro.h"

// Duration of the current frame in seconds (fx32), *0x020d6a78 (main .bss, written by the main loop; its writer
// is not ported yet: host drivers set it, 1/30 s = 0x88 for the race).
extern fx32 g_dsFrameTime;

// Moves *v towards target by rate * frame time, without overshooting; true once it is reached. main 0x020b29e8
bool dsApproach(fx32 *v, fx32 target, fx32 rate);
