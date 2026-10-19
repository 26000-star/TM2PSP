#include "dsCarWheel.h"

#include "collision/dsCollisionWorld.h"
#include "core/dsTime.h"
#include "math/dsMath.h"

// @port ov003:0212e2a0 done
dsCarWheel::dsCarWheel() {}

// @port ov003:0212e1f8 done
void dsCarWheel::Reset()
{
    target = compression = clamped = 0;
    grounded = false;
    contactNormal = {};
    contactPoint = {};
    spin = 0;
    surface = -1;
    hit = 0;
}

// @port ov003:0212e238 done
void dsCarWheel::Init(const VecFx32 &p, const VecFx32 &pr, fx32 tr, fx32 r, fx32 mn, fx32 mx)
{
    pos = p;
    probe = pr;
    travel = tr;
    radius = r;
    travelMin = mn;
    travelMax = mx;
    hit = 0;
}

// @port ov003:0212e060 done
void dsCarWheel::Lower(fx32 d)
{
    probe.y += d;
    pos.y += d;
}

// @port ov003:0212e07c done
void dsCarWheel::SetCompressionTarget(fx32 v)
{
    target = v;
    if (travel / 2 < target)
        target = travel / 2;
    if (target < -travel / 2)
        target = -travel / 2;
}

// @port ov003:0212e010 done
void dsCarWheel::UpdateCompression()
{
    if (compression < target)
        compression = target;
    else
        dsApproach(&compression, target, 0x2000);
    fx32 c = compression;
    if (c < travelMin)
        c = travelMin;
    else if (travelMax <= c)
        c = travelMax;
    clamped = c;
}

// @port ov003:0212e0b8 done
void dsCarWheel::Spin(fx32 speed)
{
    fx32 d = (fx32)(((s64)speed * g_dsFrameTime + 0x800) >> 12);
    fx32 a = spin + (radius != 0 ? FX_Div(d, radius) : 0);
    bool neg = a < 0;
    if (neg)
        a = -a;
    a = FX_ModS32(a, DS_FX_TWO_PI);
    if (neg)
        a = DS_FX_TWO_PI - a;
    spin = a;
}

// @port ov003:0212e13c done
VecFx32 dsCarWheel::ProbeOrigin(const MtxFx43 &m, fx32 dx) const
{
    VecFx32 v = {probe.x + dx, probe.y, probe.z};
    dsVec_MulMtx43(&v, &m, &v);
    return v;
}

// @port ov003:0212e198 done
VecFx32 dsCarWheel::HubPosition(const MtxFx43 &m) const
{
    VecFx32 v = {pos.x, pos.y + clamped, pos.z};
    dsVec_MulMtx43(&v, &m, &v);
    return v;
}
