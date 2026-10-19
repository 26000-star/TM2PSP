#include "dsTime.h"

// FX_Inv(60.0) = 0x44, set once by a static initialiser (__sinit main 0x020b32a0 from the constant 0x020c20b0).
// @port main:020b32a0 done
fx32 g_dsFrameTime = 0x44;

// @port main:020b29e8 done
bool dsApproach(fx32 *v, fx32 target, fx32 rate)
{
    fx32 step = (fx32)(((s64)g_dsFrameTime * rate + 0x800) >> 12);
    if (target < *v) {
        *v -= step;
        if (*v <= target) {
            *v = target;
            return true;
        }
    } else {
        *v += step;
        if (target <= *v) {
            *v = target;
            return true;
        }
    }
    return false;
}
