// dsCarDynamics collision pass (ov003 0x02122730, 0x021233d0..0x021265fc): wheel and body probes against the
// collision world, ground plane from the wheel contacts, body impulse against walls.
#include "car/dsCarDynamics.h"

#include "car/dsCarMath.h"
#include "core/dsTime.h"
#include "math/dsMath.h"

using namespace carmath;

static const fx32 BODY_PROBE_MARGIN = 614;         // ov003 0x02125768 (0.15)
static const fx32 BODY_PROBE_INSET = -819;         // ov003 0x0212576c (-0.2)
static const fx32 TURBO_LIFT = 0x666;              // ov003 0x02125774 (0.4)
static const fx32 WHEEL_PROBE_EXTRA = 0x7ff;       // ov003 0x02124f90
static const fx32 FLAT_ANGLE_DEG = 0x1e000;        // ov003 0x02136d7c (30 degrees)
static const fx32 SIDE_HIT_COS = 2896;             // ov003 0x02137930 = cos(pi/4) from the table (__sinit 0x021354dc)
static const fx32 WALL_FWD_LIMIT = (fx32)0xfffff4b1; // ov003 0x02123f90 (-0.707: the test is always true)
static const fx32 MAX_ANG_IMPULSE2 = 0x1324000;    // ov003 0x02123f9c (17.5^2)
static const fx32 ANG_IMPULSE_CLAMP = 0x46000;     // 70.0

// @port ov003:02122730 done (push and airTime are kept)
void dsCarDynamics::ResetContacts()
{
    groundedWheels = 0;
    touched = false;
    extVelSum = extAngSum = extPushSum = {};
    extCount = 0;
    hitVelSum = hitNormalSum = hitPointSum = hitOffsetSum = {};
    bodyHitCount = 0;
    roofHit = impulseDone = false;
    penetration = impulseSum = 0;
    for (auto &h : wheelHits)
        h.clear();
    for (int i = 0; i < 6; i++) {
        bodyHits[i].clear();
        bodyProbeLength[i] = 0;
    }
    aheadHit = false;
}

// Downward probe through the wheel: from probeLength above the bottom of the tyre, along -up, probeLength + 0.5
// long. Compression = probeLength - hit distance.
// @port ov003:02124d54 done (debug line drawing left out)
void dsCarDynamics::AddWheelProbe(int i)
{
    const dsCarWheel *w = wheels[i];
    VecFx32 hub = w->ProbeOrigin(matrix, 0);
    VecFx32 up = Row(matrix, 1);
    fx32 h = w->radius - wheelLowering;
    VecFx32 o = Add(Sub(hub, Scale(up, h)), Scale(up, probeLength));
    dsCollisionWorld::Get()->AddSegment(o, Neg(up), probeLength + WHEEL_PROBE_EXTRA, &wheelHits[i]);
}

// Body probe from -> to (car space unless world), its origin raised along up by bounce * up.y. Probes shorter
// than 0.01 are not cast (their length is still recorded).
// @port ov003:021245f4 done (debug line drawing left out)
void dsCarDynamics::AddBodyProbe(int i, const VecFx32 &from, const VecFx32 &to, bool world)
{
    VecFx32 f = from, t = to;
    if (!world) {
        dsVec_MulMtx43(&t, &matrix, &t);
        dsVec_MulMtx43(&f, &matrix, &f);
    }
    VecFx32 up = Row(matrix, 1);
    f = Add(f, Scale(up, Mul(bounce, Clamp01(up.y))));
    VecFx32 d = Sub(t, f);
    fx32 len = dsVec_Mag(&d);
    bodyProbeLength[i] = len;
    if (len <= 0x28)
        return;
    d = ScaleFx64c(d, FX_InvFx64c(len));
    dsCollisionWorld::Get()->AddSegment(f, d, len, &bodyHits[i]);
}

// Nearest hit, skipping material 12 except as the first entry.
template <class V> static size_t Nearest(const V &hits)
{
    size_t best = 0;
    for (size_t i = 1; i < hits.size(); i++)
        if (hits[i].t < hits[best].t && hits[i].material != 12)
            best = i;
    return best;
}

// @port ov003:02124910 done (debug line drawing left out)
void dsCarDynamics::WheelContact(int i)
{
    dsCarWheel *w = wheels[i];
    const auto &hits = wheelHits[i];
    VecFx32 up = Row(matrix, 1);
    auto release = [&] {
        w->grounded = false;
        fx32 v = w->target;
        dsApproach(&v, Mul(up.y, -w->travel / 2), FX32_ONE);
        w->SetCompressionTarget(v);
    };
    if (hits.empty()) {
        release();
        return;
    }
    w->hit = 0;
    w->hit = (s32)Nearest(hits);
    const dsCollisionHit &h = hits[w->hit];
    fx32 side = Dot(h.normal, Sub(s_centre, h.point));
    if (h.material == 12 || Dot(up, h.normal) < 0 || side < 0) {
        release();
        return;
    }
    fx32 c = probeLength - h.t;
    w->SetCompressionTarget(c);
    w->grounded = -w->travel / 2 < c;
    w->contactNormal = h.normal;
    w->contactPoint = h.point;
    if (w->grounded)
        groundedWheels++;
}

// @port ov003:02123fac done (debug line drawing left out; materials 9 and 10 only change the debug colour)
void dsCarDynamics::BodyContact(int i, const VecFx32 &from, const VecFx32 &to, bool roof, bool world)
{
    const auto &hits = bodyHits[i];
    if (hits.empty())
        return;
    const dsCollisionHit &h = hits[Nearest(hits)];
    if (h.material == 12)
        return;
    fx32 len = bodyProbeLength[i];
    VecFx32 f, t, tPrev;
    if (!world) {
        f = from;
        dsVec_MulMtx43(&f, &matrix, &f);
        t = to;
        dsVec_MulMtx43(&t, &matrix, &t);
        tPrev = to;
        dsVec_MulMtx43(&tPrev, &lastMatrix, &tPrev);
    } else {
        t = to;
        tPrev = Sub(Add(to, Row(lastMatrix, 3)), Row(matrix, 3));
        f = from;
    }
    VecFx32 d = Sub(t, f);
    if (Dot(h.normal, d) >= 0)
        return;
    fx32 pen = len - h.t;
    if (!roof || matrix.m[1][1] >= 0) {
        fx32 q = pen * 50;
        if (q < 0)
            q = 0;
        else if (p.CollPushAway <= q)
            q = p.CollPushAway;
        penetration += q;
    } else {
        push.y += Mul(h.normal.y, pen * 8);
        roofHit = true;
        if (groundedWheels < 1) {
            groundNormal = h.normal;
            groundD = -Dot(h.point, groundNormal);
        }
    }
    if (i == 5 && 0x800 < pen) {
        aheadHit = true;
        aheadPush = Scale(NormalizeSafe(d), pen);
    }
    fx32 idt = FX_Inv(dt);
    hitVelSum = Add(hitVelSum, Scale(Sub(t, tPrev), idt));
    hitOffsetSum = Add(hitOffsetSum, Sub(t, s_centre));
    hitNormalSum = Add(hitNormalSum, h.normal);
    hitPointSum = Add(hitPointSum, h.point);
    bodyHitCount++;
}

// Impulse of the averaged body contact: j = -(1 + e) vn / (1/m + |r x n|^2 / I), penetration resolved first
// as a position push; against walls (contact normal far from the ground normal while on wheels) the normal is
// pulled into the ground plane by WallEnforcer, only the yaw part of the angular impulse is kept and fed to
// wallVec, and the speed is cut by WallSlowdown.
// @port ov003:021233d0 done
void dsCarDynamics::BodyImpulse()
{
    fx64c k = FX_InvFx64c(bodyHitCount << 12);
    hitVelSum = ScaleFx64c(hitVelSum, k);
    hitPointSum = ScaleFx64c(hitPointSum, k);
    hitOffsetSum = ScaleFx64c(hitOffsetSum, k);
    hitNormalSum = ScaleFx64c(hitNormalSum, k);
    VecFx32 n = hitNormalSum;
    fx32 vn = Dot(n, hitVelSum) - penetration;
    if (vn >= 0)
        vn = 0;
    bool side = false;
    if (groundedWheels != 0) {
        fx32 d = Dot(groundNormal, n);
        if (d < 0)
            d = -d;
        side = d < SIDE_HIT_COS;
    }
    if (side) {
        VecFx32 pn = Project(groundNormal, n);
        fx32 e = p.WallEnforcer;
        n.x += Mul32(e, pn.x - n.x);
        n.y += Mul32(e, pn.y - n.y);
        n.z += Mul32(e, pn.z - n.z);
        n = NormalizeSafe(n);
    }
    VecFx32 c = Cross(hitOffsetSum, n);
    if (aheadHit)
        c = {};
    fx32 num = Mul(-(p.CollElasticity + FX32_ONE), vn);
    fx32 c2 = (fx32)(((s64)c.x * c.x + (s64)c.y * c.y + (s64)c.z * c.z + 0x800) >> 12);
    fx32 a = 0;
    if (p.d_Inertia != 0)
        a = FX_Div(c2, p.d_Inertia);
    fx32 im = FX_Inv(p.Mass);
    fx32 j = im + a == 0 ? 0 : FX_Div(num, im + a);
    fx32 dv = p.Mass == 0 ? 0 : FX_Div(j, p.Mass);
    fx32 pen = penetration;
    if (0 < pen) {
        if (dv <= pen)
            pen = dv;
        dv -= pen;
        push = Add(push, Scale(n, pen));
    }
    VecFx32 dvv = Scale(n, dv);
    fx32 da = p.d_Inertia != 0 ? FX_Div(j, p.d_Inertia) : 0;
    VecFx32 dang = Scale(c, -da);
    if (side) {
        dang = Sub(dang, Project(Row(matrix, 1), dang));
        fx32 fd = Dot(Row(matrix, 2), n);
        if (fd < 0)
            fd = -fd;
        if (WALL_FWD_LIMIT < fd)
            wallVec = Add(wallVec, Scale(dang, p.WallTurnFactor));
        fx32 w2 = (fx32)(((s64)wallVec.x * wallVec.x + (s64)wallVec.y * wallVec.y + (s64)wallVec.z * wallVec.z +
                          0x800) >> 12);
        if (Mul(p.WallTurnMax, p.WallTurnMax) < w2)
            wallVec = Scale(NormalizeSafe(wallVec), p.WallTurnMax);
    }
    fx32 a2 = (fx32)(((s64)dang.x * dang.x + (s64)dang.y * dang.y + (s64)dang.z * dang.z + 0x800) >> 12);
    if (MAX_ANG_IMPULSE2 < a2)
        dang = Scale(NormalizeSafe(dang), ANG_IMPULSE_CLAMP);
    velocity = Add(velocity, dvv);
    angularVelocity = Add(angularVelocity, dang);
    bool was = aheadPushed;
    aheadPushed = false;
    if (!was && aheadHit) {
        SetRow(matrix, 3, Sub(Row(matrix, 3), aheadPush));
        aheadPushed = true;
    }
    if (side) {
        fx32 f = p.Mass == 0 ? 0 : FX_Div(j, p.Mass);
        f = Mul(f, p.WallSlowdown) / 1000;
        if (0x800 < f)
            f = 0x800;
        velocity = Scale(velocity, FX32_ONE - f);
    }
    fx32 k2 = FX32_ONE;
    if (roofHit) {
        velocity.y = Mul(velocity.y, g_dsFrameTime) * -5 + velocity.y;
        k2 = 0x3000;
    }
    fx32 s = Mul(k2, g_dsFrameTime);
    velocity.x -= Mul(velocity.x, s);
    velocity.z -= Mul(velocity.z, s);
    impulseDone = true;
    impulseSum += j;
}

// sin(30 degrees) through the table, computed once (static guard bit 0 of 0x0213791c).
static fx32 FlatSin()
{
    static fx32 s_v = 0;
    static bool s_init = false;
    if (!s_init) {
        s64 a = (s64)12868 * (0x5a000 - FLAT_ANGLE_DEG) / 0xb4000;
        s_v = dsCos((fx32)a);
        s_init = true;
    }
    return s_v;
}

// @port ov003:02124fa0 done (the sparks effect 0x020fbe34 is left to the renderer)
void dsCarDynamics::Collide()
{
    dsCollisionWorld *world = dsCollisionWorld::Get();
    ResetContacts();
    VecFx32 c = bodyCentre;
    dsVec_MulMtx43(&c, &matrix, &c);
    s_centre = c;
    world->SetMasks(1, 0);
    VecFx32 up = Row(matrix, 1);
    fx32 vup = Dot(velocity, up);
    world->SetMasks(1, 0);

    // Wheel probe length: 0.75, up to 1.5 when falling fast or spinning.
    fx32 k = 0;
    if (vup < 0)
        k = Mul(-vup, 0x7b);
    k += dsVec_Mag(&angularVelocity) / 7;
    if (turboTimer != 0)
        k = FX32_ONE;
    k = Clamp01(k);
    fx32 target = Mul(k, 0xc00) + 0xc00;
    if (probeLength < target) {
        probeLength = target;
    } else {
        probeLength -= dt;
        if (probeLength < target)
            probeLength = target;
    }
    for (int i = 0; i < (int)wheels.size(); i++)
        AddWheelProbe(i);

    bounce = Mul(bounce, FX32_ONE - g_dsFrameTime * 10);
    if (velocity.y < 0) {
        fx32 b = -velocity.y / 50;
        if (bounce <= b)
            bounce = b;
    }

    // Body probes from the body centre to the four corners (front and rear wheel tips, at least the body box),
    // a roof probe and one along the velocity.
    VecFx32 origin = bodyCentre;
    VecFx32 fl = {model->frontWheel.x + BODY_PROBE_MARGIN, model->frontWheel.y, model->frontWheel.z};
    VecFx32 rl = {model->rearWheel.x + BODY_PROBE_MARGIN, model->rearWheel.y, model->rearWheel.z};
    fl.z += model->frontWheel.y;
    rl.z -= model->rearWheel.y;
    fx32 m = bodyCentre.z - (halfSize.z + BODY_PROBE_INSET);
    if (m <= rl.z)
        rl.z = m;
    m = bodyCentre.z + halfSize.z + BODY_PROBE_INSET;
    if (fl.z <= m)
        fl.z = m;
    m = bodyCentre.x + halfSize.x;
    if (rl.x <= m)
        rl.x = m;
    if (fl.x <= m)
        fl.x = m;
    fx32 topY = bodyCentre.y + halfSize.y;
    if (topY <= fl.y + FX32_ONE)
        topY = fl.y + FX32_ONE;
    VecFx32 top = {0, topY, 0};
    VecFx32 flm = {-fl.x, fl.y, fl.z};
    VecFx32 rlm = {-rl.x, rl.y, rl.z};
    fx32 uy = 0;
    if (up.y >= 0)
        uy = up.y < FX32_ONE ? up.y : FX32_ONE;
    origin.y += Mul(Mul(halfSize.y, 0xc00), uy);

    bool ahead = false;
    VecFx32 aheadTo = {};
    VecFx32 v = velocity;
    fx32 vm = dsVec_Mag(&v);
    if (4 < vm) {
        MtxFx43 inv = matrix;
        MTX_Inverse43(&inv, &inv);
        dsVec_MulMtx33(&v, &inv, &v);
        v = ScaleFx64c(v, FX_InvFx64c(vm));
        v = Scale(v, dsVec_Mag(&halfSize) << 1);
        auto clip = [&v](fx32 comp, fx32 limit) {
            fx32 t = comp != 0 ? FX_Div(limit, comp) : 0;
            v = Scale(v, t);
        };
        if (fl.z - origin.z < v.z)
            clip(v.z, fl.z - origin.z);
        else if (v.z < rl.z - origin.z)
            clip(v.z, rl.z - origin.z);
        if (fl.x - origin.x < v.x)
            clip(v.x, fl.x - origin.x);
        else if (v.x < flm.x - origin.x)
            clip(v.x, flm.x - origin.x);
        if (halfSize.y < v.y)
            clip(v.y, halfSize.y);
        else if (v.y < fl.y - halfSize.y)
            clip(v.y, fl.y - halfSize.y);
        aheadTo = {origin.x + v.x, bodyCentre.y + v.y, origin.z + v.z};
        ahead = !skipAheadProbe && !stuck;
    }
    if (turboTimer != 0) {
        fl.y += TURBO_LIFT;
        flm.y += TURBO_LIFT;
        rl.y += TURBO_LIFT;
        rlm.y += TURBO_LIFT;
        origin.y += TURBO_LIFT;
        top.y += TURBO_LIFT;
        aheadTo.y += TURBO_LIFT;
    }
    VecFx32 roofFrom = {0, (origin.y + (rl.y + fl.y) / 2) / 2, 0};
    AddBodyProbe(0, origin, fl, false);
    AddBodyProbe(1, origin, flm, false);
    AddBodyProbe(2, origin, rl, false);
    AddBodyProbe(3, origin, rlm, false);
    AddBodyProbe(4, roofFrom, top, false);
    if (ahead) {
        dsVec_MulMtx43(&aheadTo, &matrix, &aheadTo);
        AddBodyProbe(5, prevCentre, aheadTo, true);
    }
    world->TestSegments();

    // Ground plane: mean of the wheel contacts, without the most deviating normal when 3 or 4 wheels touch.
    s32 n = 0;
    VecFx32 sn = {}, sp = {};
    for (int i = 0; i < (int)wheels.size(); i++) {
        WheelContact(i);
        const dsCarWheel *w = wheels[i];
        if (w->grounded) {
            sn = Add(sn, w->contactNormal);
            sp = Add(sp, w->contactPoint);
            n++;
        }
    }
    if (1 < n) {
        fx64c inv = FX_InvFx64c(n << 12);
        VecFx32 an = ScaleFx64c(sn, inv);
        VecFx32 ap = ScaleFx64c(sp, inv);
        if (2 < n) {
            int worst = -1;
            fx32 wd = 0;
            for (int i = 0; i < (int)wheels.size(); i++) {
                if (!wheels[i]->grounded)
                    continue;
                fx32 d = Dot(an, wheels[i]->contactNormal);
                if (worst == -1 || d < wd) {
                    worst = i;
                    wd = d;
                }
            }
            VecFx32 s = {};
            for (int i = 0; i < (int)wheels.size(); i++)
                if (i != worst && wheels[i]->grounded)
                    s = Add(s, wheels[i]->contactNormal);
            an = ScaleFx64c(s, FX_InvFx64c((n - 1) * FX32_ONE));
        }
        groundNormal = an;
        groundD = -Dot(ap, groundNormal);
    }

    BodyContact(0, origin, fl, false, false);
    BodyContact(1, origin, flm, false, false);
    BodyContact(2, origin, rl, false, false);
    BodyContact(3, origin, rlm, false, false);
    BodyContact(4, roofFrom, top, true, false);
    if (ahead)
        BodyContact(5, prevCentre, aheadTo, false, true);
    prevCentre = s_centre;

    if (0 < extCount) {
        fx32 inv = FX_Inv(extCount << 12);
        velocity = Add(velocity, Scale(extVelSum, inv));
        angularVelocity = Add(angularVelocity, Scale(extAngSum, inv));
        push = Add(push, Scale(Scale(extPushSum, inv), p.CarCarShunt));
        touched = true;
    }

    // Two wheels of one side on ground tilted more than 30 degrees towards that side: no roll further over.
    fx32 thr = FlatSin();
    VecFx32 right = Row(matrix, 0), fwd = Row(matrix, 2);
    if (wheels[1]->grounded && wheels[3]->grounded && thr < Dot(groundNormal, right)) {
        if (Dot(fwd, angularVelocity) < 0)
            angularVelocity = Project(fwd, angularVelocity);
    }
    if (wheels[2]->grounded && wheels[0]->grounded && Dot(groundNormal, right) < -thr) {
        if (0 < Dot(fwd, angularVelocity))
            angularVelocity = Project(fwd, angularVelocity);
    }

    if (0 < bodyHitCount) {
        BodyImpulse();
        touched = true;
    }
    if (groundedWheels < 1)
        groundTime = 0;
    else
        groundTime += dt;
    stuck = groundedWheels < 1 && matrix.m[1][1] > 0 && 0 < bodyHitCount && 0 < hitNormalSum.y;
    if (!stuck) {
        stuckTime = 0;
    } else {
        stuckTime += dt;
        if (0x1fff < stuckTime)
            respawnRequest = true;
    }
    if (!touched)
        airTime += dt;
    else
        airTime = 0;
}
