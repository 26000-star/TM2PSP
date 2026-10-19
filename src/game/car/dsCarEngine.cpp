#include "dsCarEngine.h"

#include "car/dsCarParams.h"
#include "core/dsTime.h"

static inline fx32 Mul(fx32 a, fx32 b)
{
    return (fx32)(((s64)a * b + 0x800) >> 12);
}

static const fx32 SHIFT_LOCK_TIME = 0x19a; // 0.1 s without drive force after hitting the rev limit (manual box)

// @port ov003:0212ee38 done
dsCarEngine::dsCarEngine(const dsCarParams &p)
{
    Setup(p);
}

// @port ov003:0212edc8 done
void dsCarEngine::Setup(const dsCarParams &p)
{
    rpmMin = p.RPMMin << 12;
    rpmMax = p.RPMMax << 12;
    rpmShiftUp = rpmMax - 0x64000;
    gearChangeGap = p.GearChangeGap;
    BuildGears(p);
    if ((u32)gears.size() - 1u < (u32)gear) // unsigned, as the original: reverse (-1) is clamped too
        gear = (s32)gears.size() - 1;
}

// One gear: ratio = top / rpmTop; startRpm = previous gear's top speed / ratio; torque from the time to reach the
// top speed from startRpm. Reverse uses RPMMax and starts from 0. The last forward gear also uses RPMMax.
// @port ov003:0212ea78 done
void dsCarEngine::BuildGears(const dsCarParams &p)
{
    s32 n = p.NumGears;
    gears.resize((size_t)(n < 0 ? 0 : n));
    auto build = [&](s32 i, fx32 rpmTop, fx32 prevTop) {
        dsCarGear &g = gears[i];
        fx32 top = p.d_TopSpeed[i];
        fx32 time = i == 0 ? p.TimeRev : p.Time[i - 1];
        g.ratio = rpmTop != 0 ? FX_Div(top, rpmTop) : 0;
        g.startRpm = 0;
        if (prevTop > 0)
            g.startRpm = g.ratio != 0 ? FX_Div(prevTop, g.ratio) : 0;
        fx32 f = rpmTop != 0 ? FX_Div(rpmTop - g.startRpm, rpmTop) : 0;
        fx32 t = Mul(Mul(top, g.ratio), f);
        g.torque = time != 0 ? FX_Div(t, time) : 0;
        if (i > 1)
            g.downRpm = g.startRpm - 0x1f4000;
    };
    fx32 prevTop = 0;
    s32 i = 1;
    for (; i < n - 1; i++) {
        build(i, rpmShiftUp, prevTop);
        prevTop = p.d_TopSpeed[i];
    }
    if (n >= 1)
        build(n - 1, rpmMax, prevTop);
    if (n >= 1) {
        // reverse: from standstill, up to RPMMax
        dsCarGear &g = gears[0];
        fx32 top = p.d_TopSpeed[0];
        g.ratio = rpmMax != 0 ? FX_Div(top, rpmMax) : 0;
        g.startRpm = 0;
        fx32 f = rpmMax != 0 ? FX_Div(rpmMax - g.startRpm, rpmMax) : 0;
        fx32 t = Mul(Mul(top, g.ratio), f);
        g.torque = p.TimeRev != 0 ? FX_Div(t, p.TimeRev) : 0;
    }
}

// @port ov003:0212ea0c done
void dsCarEngine::Reset()
{
    gear = 1;
    speed = force = rpm = throttle = 0;
    rawThrottle = 0;
    brake = stopTime = shiftTime = unk40 = 0;
    unk50 = unk54 = 0;
    for (dsCarGear &g : gears)
        g.unk04 = 0;
}

// @port ov003:0212e590 done
fx32 dsCarEngine::MaxSpeed() const
{
    return gears.empty() ? 0 : Mul(rpmMax, gears.back().ratio);
}

// Torque at rpm in the current gear. Below the gear's start rpm it falls off with (1 - t)^3 where the original
// subtracts t from the integer 1, not from 1.0 (rsb r3, r12, #1 at 0x0212e63c): kept as is.
// @port ov003:0212e5c8 done
fx32 dsCarEngine::Torque(fx32 r) const
{
    if (gear == 0)
        return 0;
    const dsCarGear &g = gear > 0 ? gears[gear] : gears[0];
    fx32 tq = Mul(g.torque, 0x1000 - g.unk04);
    if (r < g.startRpm) {
        fx32 t = g.startRpm != 0 ? FX_Div(r, g.startRpm) : 0;
        fx32 a = 1 - t;
        fx32 c = Mul(Mul(a, a), a);
        tq = Mul(tq, 0x1000 - c);
    } else if (r >= rpmShiftUp && limiter) {
        fx32 t = rpmMax - rpmShiftUp != 0 ? FX_Div(r - rpmShiftUp, rpmMax - rpmShiftUp) : 0;
        if (t > 0x1000)
            t = 0x1000;
        tq += Mul(-tq, t);
    }
    return tq;
}

// @port ov003:0212e6e8 done
void dsCarEngine::SetGear(s32 g)
{
    s32 last = (s32)gears.size() - 1;
    if (last < g)
        g = last;
    if (g < -1)
        g = -1;
    gear = g;
}

// @port ov003:0212e4fc done
fx32 dsCarEngine::BrakeInput(fx32 t) const
{
    if (!automatic)
        return t < 1 ? -t : 0;
    if (gear < 1) {
        if (speed < 1 && t > 0)
            return t;
        if (speed > 0x400 && t < 0)
            return -t;
    } else {
        if (speed >= 0 && t < 0)
            return -t;
        if (speed < -0x400 && t > 0)
            return t;
    }
    return 0;
}

// @port ov003:0212e308 done
void dsCarEngine::UpdateGear(fx32 t, s32 shift)
{
    rawThrottle = t;
    fx32 used;
    if (!automatic)
        used = t < 0 ? 0 : t;
    else if (t >= 0 && gear > 0)
        used = t;
    else if (t <= 0 && gear < 0)
        used = -t;
    else
        used = 0;
    throttle = used;
    brake = BrakeInput(t);
    s32 n = (s32)gears.size();
    if (shift == 1 && gear < n - 1) {
        gear++;
        if (shiftTime <= gearChangeGap)
            shiftTime = gearChangeGap;
    }
    if (shift == -1 && gear >= 0) {
        gear--;
        if (shiftTime <= gearChangeGap)
            shiftTime = gearChangeGap;
    }
    if (!automatic)
        return;
    stopTime += g_dsFrameTime;
    bool slow = (gear > 0 && speed < 0x800) || (gear < 0 && speed >= -0x7ff);
    if (!slow || stopTime < 0x401)
        return;
    if (gear == -1 && rawThrottle > 0) {
        s32 g = 1;
        if (n - 1 < 1)
            g = n - 1;
        if (g < -1)
            g = -1;
        gear = g;
    } else if (gear >= 0 && rawThrottle < 0) {
        s32 g = -1;
        if (n - 1 < -1)
            g = n - 1;
        gear = g;
    }
    stopTime = 0;
}

// rpm from the wheel speed, drive force = torque * throttle / ratio, automatic shifting with hysteresis.
// @port ov003:0212e70c done
void dsCarEngine::Update(fx32 spd, fx32 thr, s32 shift)
{
    speed = spd;
    UpdateGear(thr, shift);
    s32 n = (s32)gears.size();
    fx32 lock = unk50;
    fx32 ratio = 0;
    if (gear == 0) {
        force = 0;
        rpm = gears[0].ratio != 0 ? FX_Div(spd, gears[0].ratio) : 0;
        if (rpm < rpmMin)
            rpm = rpmMin;
    } else {
        if (gear > 0)
            ratio = gears[gear].ratio;
        else if (gear == -1)
            ratio = gears[0].ratio;
        rpm = ratio != 0 ? FX_Div(spd, ratio) : 0;
        if (rpm < rpmMin)
            rpm = rpmMin;
        fx32 t = throttle < 1 ? 0 : throttle;
        force = Mul(Torque(rpm), t);
        if (shiftTime > 0)
            force = 0;
        force = ratio != 0 ? FX_Div(force, ratio) : 0;
    }
    fx32 k = 0x1000;
    if (unk54 != 0)
        k = 0x1000 - Mul(unk54, 0xc00);
    if (rpmMax < rpm && limiter && !automatic)
        shiftTime = SHIFT_LOCK_TIME;
    if (automatic && gear > 0) {
        fx32 up = rpmShiftUp;
        fx32 down = gears[gear].downRpm;
        if (unk54 != 0) {
            up = Mul(up, k);
            down = Mul(down, k);
        }
        if (up < rpm && gear < n - 1 && gear < n - ((s32)((u32)lock * (u32)n) >> 12)) {
            gear++;
            if (shiftTime <= gearChangeGap)
                shiftTime = gearChangeGap;
        } else if (rpm < down && gear > 1) {
            fx32 r = gears[gear - 1].ratio != 0 ? FX_Div(spd, gears[gear - 1].ratio) : 0;
            if (r < up) {
                gear--;
                if (shiftTime <= gearChangeGap)
                    shiftTime = gearChangeGap;
            }
        }
    }
    if (shiftTime > 0) {
        shiftTime -= g_dsFrameTime;
        if (shiftTime < 0)
            shiftTime = 0;
    }
    if (unk40 != 0) {
        unk40 -= g_dsFrameTime;
        if (unk40 < 0)
            unk40 = 0;
    }
}
