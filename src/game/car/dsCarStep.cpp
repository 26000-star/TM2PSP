// dsCarDynamics step (ov003 0x021222fc) and the wheel update that ends it (0x02127374).
#include "car/dsCarDynamics.h"

#include "car/dsCarMath.h"
#include "core/dsTime.h"
#include "math/dsMath.h"

using namespace carmath;

static const fx32 OVER_TRAVEL = 410;               // ov003 0x021279e0 (0.1)
static bool s_warmingUp = false;                   // ov003 0x021378fc

// Wheel compression; while a wheel is pushed more than 0.1 past WheelTravelTop the display matrix (+0x1f0) is
// rebuilt from the four wheel positions (and, under a turbo, the physics matrix too).
// @port ov003:02127374 partial (the visual update 0x02126d10 and the model's roll/pitch/steer inputs are left
// to the renderer)
void dsCarDynamics::UpdateWheels()
{
    for (int i = 0; i < 4; i++)
        wheels[i]->UpdateCompression();
    UpdateWheelSurfaces();
    fx32 ext[4];
    fx32 mx = 0;
    for (int i = 0; i < 4; i++) {
        const dsCarWheel *w = wheels[i];
        fx32 e = 0;
        if (w->grounded)
            e = turboTimer == 0 ? w->compression : w->target;
        e -= w->travelMax;
        if (e < 1)
            e = 0;
        ext[i] = e;
    }
    mx = ext[0] <= ext[1] ? ext[1] : ext[0];
    fx32 mr = ext[2] <= ext[3] ? ext[3] : ext[2];
    if (mx <= mr)
        mx = mr;
    dsApproach(&wheelBlend, OVER_TRAVEL < mx ? FX32_ONE : 0, 0x4000);
    if (wheelBlend == 0) {
        dsMtx43_Copy(&matrix, &prevMatrix);
        return;
    }
    VecFx32 pw[4];
    for (int i = 0; i < 4; i++) {
        pw[i] = {wheels[i]->probe.x, Mul(ext[i], wheelBlend), wheels[i]->probe.z};
        dsVec_MulMtx43(&pw[i], &matrix, &pw[i]);
    }
    fx32 half = FX_Inv(0x2000);
    VecFx32 front = Scale(Add(pw[0], pw[1]), half);
    half = FX_Inv(0x2000);
    VecFx32 rear = Scale(Add(pw[2], pw[3]), half);
    fx32 f = 0;
    fx32 dz = wheels[0]->probe.z - wheels[2]->probe.z;
    if (dz != 0)
        f = FX_Div(wheels[2]->probe.z, dz);
    VecFx32 dir = Sub(front, rear);
    half = FX_Inv(0x2000);
    f = -f;
    VecFx32 side = {Mul((pw[2].x - pw[3].x) + (pw[0].x - pw[1].x), half),
                    Mul((pw[2].y - pw[3].y) + (pw[0].y - pw[1].y), half),
                    Mul((pw[2].z - pw[3].z) + (pw[0].z - pw[1].z), half)};
    VecFx32 up = Cross(dir, side);
    VecFx32 pos = Add(rear, Scale(dir, f));
    MtxFx43 m;
    dsMtx43_FromAxes(&m, &pos, &dir, &up);
    dsMtx43_Copy(&m, &prevMatrix);
    if (turboTimer != 0)
        dsMtx43_Copy(&prevMatrix, &matrix);
}

// Material under each wheel from its probe's nearest hit (-1 without hit), the physics part of the visual
// update; it runs at the end of the step in the original (from UpdateWheels), the port does it first.
// @port ov003:02126d10 partial (only the wheel surfaces; wheel/body display matrices go to the renderer)
void dsCarDynamics::UpdateWheelSurfaces()
{
    for (int i = 0; i < 4; i++) {
        dsCarWheel *w = wheels[i];
        const auto &h = wheelHits[i];
        w->surface = h.empty() ? -1 : (s32)(s8)h[w->hit].material;
    }
}

// @port ov003:021222fc done (replay modes 1 and 2 take their target from +0x424/+0x448, not filled by the port)
void dsCarDynamics::Step()
{
    dt = g_dsFrameTime;
    if (warmUp) {
        warmUp = false;
        input = dsCarInput();
        inputMode = 3;
        s_warmingUp = true;
        for (int i = 0; i < warmUpSteps; i++)
            Step();
        if (warmUpSteps != 0x14)
            warmUpSteps = 0x14;
        s_warmingUp = false;
    }
    warmingUp = &s_warmingUp;
    fx32 t = p.d_DonutSpeed == 0 ? 0 : FX_Div(speed - p.d_DonutSpeed, p.d_DonutSpeed);
    t = Clamp01(t);
    s_donutBlend = Mul(Mul(FX32_ONE - t, skid), Clamp01(throttle));
    s_prevVelocity = velocity;
    bool simulate = false;
    switch (inputMode) {
    case 0:
        input = dsCarInput();
        simulate = true;
        break;
    case 3:
        simulate = true;
        break;
    default:
        s_steerScale = FX32_ONE;
        break;
    }
    if (simulate) {
        UpdateInputs();
        Drive();
        Grip();
        Integrate();
    }
    if (inputMode != 2)
        Collide();
    Surfaces();
    Forces();
    if ((matrix.m[2][0] < 0 ? -matrix.m[2][0] : matrix.m[2][0]) < 2)
        matrix.m[2][0] = 0;
    if ((matrix.m[2][2] < 0 ? -matrix.m[2][2] : matrix.m[2][2]) < 2)
        matrix.m[2][2] = 0;
    // Re-orthonormalise one row per step.
    switch (orthoRow) {
    case 0:
        SetRow(matrix, 2, NormalizeSafe(Row(matrix, 2)));
        break;
    case 1:
        SetRow(matrix, 0, Cross(Row(matrix, 1), Row(matrix, 2)));
        break;
    case 2:
        SetRow(matrix, 0, NormalizeSafe(Row(matrix, 0)));
        break;
    case 3:
        SetRow(matrix, 1, Cross(Row(matrix, 2), Row(matrix, 0)));
        break;
    }
    orthoRow = (orthoRow + 1) & 3;
    UpdateWheels();
    dsMtx43_Copy(&matrix, &lastMatrix);
    fx32 inv = FX_Inv(dt);
    acceleration = Scale(Sub(velocity, s_prevVelocity), inv);
    skipAheadProbe = false;
}
