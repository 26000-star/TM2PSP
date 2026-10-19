#include "dsTime.h"

fx32 g_dsFrameTime = 0x88;

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
