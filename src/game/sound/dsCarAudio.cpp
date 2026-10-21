#include "sound/dsCarAudio.h"

#include "car/dsCarParams.h"
#include "core/dsTime.h"
#include "math/dsMath.h"
#include "script/dsLua.h"
#include "sound/dsSound3D.h"

static dsCarAudio *s_carAudio;

// ov003 0x020f93e0: an RPM of config/caraudio (fx32, 1024 = MaxRPM) in RPM: value / 1024 x MaxRPM, MaxRPM going
// through the doubles' rounding (main 0x020125a4 / _dadd 0x0201201c with 0.5 / 0x02012438), the result truncated
// to an integer. (The port read the _dadd of 0.5 as a product: every threshold was halved, RevMed went silent
// above 7031 rpm while the engine runs at 6000 to 9000, so the engine was either silent or at full volume.)
// @port ov003:020f93e0 done
static int ConvRpm(fx32 value, int maxRpm)
{
    fx32 a = FX_Div(value, 0x400000);
    fx32 b = dsRoundD(maxRpm << 12);
    return (int)(((s64)a * b + 0x800) >> 12) >> 12;
}

static fx32 MulRound(fx32 a, fx32 b) { return (fx32)(((s64)a * b + 0x800) >> 12); }

dsCarAudio *dsCarAudio::Get()
{
    if (!s_carAudio)
        s_carAudio = new dsCarAudio;
    return s_carAudio;
}

void dsCarAudio::Destroy()
{
    if (s_carAudio)
        s_carAudio->Reset();
    delete s_carAudio;
    s_carAudio = nullptr;
}

// @port ov003:020f7000 done
void dsCarAudio::Start(const VecFx32 &pos, Source &s, bool loop)
{
    dsSound3D *s3 = dsSound3D::Get();
    s.loop = loop;
    int v = s.volume;
    if (v < 0x80) {
        s.handle[0] = s3->Create(s.name.c_str(), pos, m_minDist, m_maxDist, loop);
        s3->SetVolume(s.handle[0], v);
        return;
    }
    for (int i = 0, n = v / 127; i < n && i < 2; i++) {   // a louder sound plays several times at 127
        s.handle[i] = s3->Create(s.name.c_str(), pos, m_minDist, m_maxDist, loop);
        if (v < 0x80)
            s3->SetVolume(s.handle[i], v);
        else {
            s3->SetVolume(s.handle[i], 0x7f);
            v -= 0x7f;
        }
    }
}

void dsCarAudio::Release(Source &s)
{
    dsSound3D *s3 = dsSound3D::Get();
    for (u32 &h : s.handle)
        if (s3->IsValid(h))
            h = s3->Release(h, 0);
    s.loop = false;
}

// @port ov003:020f8700 done (one car entry)
void dsCarAudio::Reset()
{
    for (Source &s : m_engine)
        Release(s);
    for (Source &s : m_skid)
        Release(s);
    for (Source &s : m_collision)
        Release(s);
    Release(m_horn);
    m_ready = false;
}

// @port ov003:020f7e6c partial (one car entry: the other cars' engines are restarted there) / ov003:020f6a30 done
void dsCarAudio::RaceReset()
{
    for (Source &s : m_skid)
        Release(s);
    for (Source &s : m_collision)
        Release(s);
    for (Source &s : m_engine)
        Release(s);
    m_master = m_target = 0x7f;
    m_step = 0;
    m_byte0c = 0;
    m_minDist = 5;
    m_maxDist = 100;
    m_player = CarEntry();
}

// @port ov003:020f89ac partial (one car entry: *0x02134e58 = 1) / ov003:020f8c90 done / ov003:020f8c28 done /
// ov003:020f92cc done
bool dsCarAudio::Init(const std::string &car, const dsCarParams &params)
{
    Reset();
    u32 invalid = dsSound3D::Get()->InvalidHandle();
    auto init = [&](Source &s, const char *name) {
        s = Source();
        s.name = name;
        s.handle[0] = s.handle[1] = invalid;
    };
    m_master = m_target = 0x7f;
    m_step = 0;
    m_byte0c = 0;
    m_player = CarEntry();
    m_engineGear = 0;
    m_engineRpm = 0;
    m_collisionWait = 0;
    init(m_engine[0], "CAR_IDLE");
    init(m_engine[1], "CAR_LOW");
    init(m_engine[2], "CAR_MED");
    init(m_skid[0], "SKID_TARMAC");   // ov003 0x02134e50
    init(m_skid[1], "SKID_DIRT");
    static const char *const hits[] = {"COLLISION_METAL_1", "COLLISION_METAL_2", "COLLISION_METAL_3", "COLLISION_WOOD_1"};
    for (int i = 0; i < 4; i++)
        init(m_collision[i], hits[i]);
    init(m_horn, "HORN");
    for (auto &g : gears)
        for (Layer &l : g)
            l = Layer();
    for (int i = 0; i < 7; i++) {
        gearSpeed[i] = params.d_TopSpeed[i];
        gearTime[i] = i == 0 ? params.TimeRev : params.Time[i - 1];
    }
    maxRpm = reverseGear = 0;

    static dsScriptManager s_scripts;
    dsLuaStateRef state = s_scripts.Run("config/caraudio");
    if (!state.get())
        return false;
    dsLuaObject all = state->Global("AudioParams");
    bool found = false;
    auto layer = [&](const dsLuaObject &o, Layer &l) {
        l.maxVolume = o.FieldNumber("MaxVolume") >> 12;
        l.start = ConvRpm(o.FieldNumber("StartRPM"), maxRpm);
        l.sustain = ConvRpm(o.FieldNumber("SustainRPM"), maxRpm);
        l.decay = ConvRpm(o.FieldNumber("DecayRPM"), maxRpm);
        l.release = ConvRpm(o.FieldNumber("ReleaseRPM"), maxRpm);
    };
    for (dsLuaObject k = all.Begin(); !k.IsNil(); k = all.Next(k)) {
        const char *name = k.ToString();
        if (!name || car != name)
            continue;
        dsLuaObject v = all.Value(k);
        maxRpm = v.FieldNumber("MaxRPM") >> 12;
        reverseGear = (v.FieldNumber("ReverseGear") >> 12) - 1;
        dsLuaObject list = v.Field("Gears");
        int n = 0;
        if (list.IsNil()) {
            layer(v.Field("Idle"), gears[0][0]);
            layer(v.Field("RevLow"), gears[0][1]);
            layer(v.Field("RevMed"), gears[0][2]);
            n = 1;
        } else {
            for (int count = list.Count(); n < count && n < 6; n++) {
                dsLuaObject g = list.Index(n);
                layer(g.Field("Idle"), gears[n][0]);
                layer(g.Field("RevLow"), gears[n][1]);
                layer(g.Field("RevMed"), gears[n][2]);
            }
        }
        for (; n < 6 && n > 0; n++)   // the missing gears take the previous one's
            for (int l = 0; l < 3; l++)
                gears[n][l] = gears[n - 1][l];
        found = true;
    }
    m_ready = true;
    return found;
}

// @port ov003:020f6d70 done
void dsCarAudio::FadeOut(int frames)
{
    m_target = 0;
    m_step = frames ? 0x7f / frames : 0x7f;
}

// @port ov003:020f6da0 done
void dsCarAudio::FadeIn(int frames)
{
    m_target = 0x7f;
    m_step = frames ? 0x7f / frames : 0x7f;
}

// Argument 3 (set by Engine while byte +0x0c is clear, i.e. for the human player): the car entity's engine rpm and
// gear as they are. Otherwise a "sound rpm" from the smoothed speed: 7000 to 9000 across each gear's speed band.
// @port ov003:020f799c done
void dsCarAudio::SoundRpm(const dsCarDynamics &car, CarEntry &e)
{
    if (m_byte0c == 0) {
        e.rpm = m_engineRpm >> 12;
        e.gear = m_engineGear;
        return;
    }
    e.speed += MulRound(car.speed - e.speed, 0x29);
    if (e.speed < 0x19a) {
        e.gear = 0;
        e.rpm = 7000;
    } else {
        e.gear = 6;
        for (int i = 0; i < 7; i++)
            if (e.speed < gearSpeed[i]) {
                e.gear = i + 1;
                break;
            }
        fx32 lo = e.gear == 1 ? 0 : gearSpeed[e.gear - 2], hi = gearSpeed[e.gear - 1];
        u32 frac = hi == lo ? 0 : (u32)FX_Div(e.speed - lo, hi - lo);
        int target = (int)((((u64)frac * 0x7d0000 + 0x800) >> 12) + 0x1b58000) >> 12; // 7000 + 2000 x frac
        fx32 k = e.gear > 1 && target <= e.rpm ? 0x333 : 0x800;
        e.rpm += MulRound(target - e.rpm, k);
    }
    e.rpm = e.rpm < 800 ? 800 : e.rpm - 800;
    e.gearCopy = e.gear;
    e.stopped = e.gearCopy == 0;
}

// @port ov003:020f75cc done (the player's car: no Doppler, byte +0x0d clear)
void dsCarAudio::Engine(const dsCarDynamics &car)
{
    dsSound3D *s3 = dsSound3D::Get();
    CarEntry &e = m_player;
    e.pitch = 0;
    SoundRpm(car, e);
    int g = reverseGear;
    if (e.gear >= 0) {
        g = e.gear > 0 ? e.gear - 1 : 0;
        if (g > 5)
            g = 5;
    }
    if (g < 0 || g > 5)
        g = 0;
    int rpm = e.rpm < maxRpm ? e.rpm : maxRpm;
    for (int l = 0; l < 3; l++) {
        const Layer &c = gears[g][l];
        Source &s = m_engine[l];
        if (c.release < rpm || rpm < c.start) {
            s.volume = 0;
            Release(s);
        } else if (!s3->IsValid(s.handle[0]))
            Start(e.pos, s, true);
        if (s3->IsValid(s.handle[0])) {
            if (rpm < c.sustain) {
                int v = c.maxVolume * (rpm - c.start) / (c.sustain - c.start);
                v = v > m_master ? m_master : v;
                s3->SetVolume(s.handle[0], v);
                s.volume = v;
            } else if (c.decay < rpm) {
                int v = c.maxVolume * (c.release - rpm) / (c.release - c.decay);
                v = v > m_master ? m_master : v;
                s3->SetVolume(s.handle[0], v);
                s.volume = v;
            } else {
                int v = c.maxVolume > m_master ? m_master : c.maxVolume;
                if (s.volume != v)
                    s3->SetVolume(s.handle[0], v);
                s.volume = v;
            }
            s3->SetPitch(s.handle[0], s.volume * 3 + e.pitch);
        }
        PlaceSource(s);
    }
}

// The per-frame part of each source: the volume held under the master volume, the position at the car.
void dsCarAudio::PlaceSource(Source &s)
{
    dsSound3D *s3 = dsSound3D::Get();
    for (u32 h : s.handle)
        if (s3->IsValid(h)) {
            if (m_master <= s.volume)
                s3->SetVolume(h, m_master);
            VecFx32 p = {m_player.pos.x + s.offset.x, m_player.pos.y + s.offset.y, m_player.pos.z + s.offset.z};
            s3->SetPosition(h, p);
        }
}

// @port ov003:020f7f58 partial (one car entry: the other cars' engines and their Doppler pitch, 0x020f7148 /
// 0x020f68e0, are not needed)
void dsCarAudio::Update(const MtxFx43 &camera, const dsCarDynamics &car)
{
    if (!m_ready)
        return;
    dsSound3D::Get()->SetListener(camera);
    m_listenerPos = {camera.m[3][0], camera.m[3][1], camera.m[3][2]};
    if (m_step) {
        if (m_master < m_target) {
            m_master += m_step;
            if (m_master > m_target)
                m_master = m_target;
        } else {
            m_master -= m_step;
            if (m_master < m_target)
                m_master = m_target;
        }
        if (m_master == m_target)
            m_step = 0;
    }
    const MtxFx43 &m = car.matrix;   // 0.1 unit ahead of the car's origin
    for (int i = 0; i < 3; i++)
        (&m_player.pos.x)[i] = m.m[3][i] + MulRound(m.m[2][i], 0x19a);
    Engine(car);
    for (Source &s : m_skid)
        PlaceSource(s);
    for (Source &s : m_collision)
        PlaceSource(s);
    PlaceSource(m_horn);
}

// @port ov003:020f7b4c done
void dsCarAudio::Skid(bool skid, bool tarmac, bool dirt)
{
    dsSound3D *s3 = dsSound3D::Get();
    auto up = [&](Source &s, int limit) {
        s.volume += 0x12;
        if (s.volume > limit)
            s.volume = limit;
        if (!s3->IsValid(s.handle[0])) {
            s.volume = 0;
            Start(m_player.pos, s, true);
        } else
            s3->SetVolume(s.handle[0], s.volume);
    };
    auto down = [&](Source &s) {
        if (!s3->IsValid(s.handle[0]))
            return;
        s.volume -= 0x12;
        if (s.volume < 1) {
            Release(s);
            s.volume = 0;
        } else
            s3->SetVolume(s.handle[0], s.volume);
    };
    if (skid) {
        if (tarmac)
            up(m_skid[0], m_master);
        if (dirt)
            up(m_skid[1], m_master >> 2);
    }
    if (!skid || !tarmac)
        down(m_skid[0]);
    if (!skid || !dirt)
        down(m_skid[1]);
}

// @port ov003:020f6e38 done
void dsCarAudio::Collision(int index, const VecFx32 &offset, fx32 speed, fx32 impact)
{
    dsSound3D *s3 = dsSound3D::Get();
    Source &s = m_collision[index];
    if (s3->IsValid(s.handle[0]))
        return;
    fx32 a = MulRound(speed, 0x33);
    fx32 b = impact < 0 ? -impact : impact;
    int v = (MulRound(b, 0x3f8000) >> 12) * (MulRound(a, 0x3f8000) >> 12);
    v /= 127;
    v = v < 0x20 ? 0x20 : v > 0xfe ? 0xfe : v;
    s.volume = v > m_master ? m_master : v;
    Start(m_player.pos, s, false);
    s.offset = offset;
    int pitch = MulRound(a, 0x258000) >> 12;
    if (pitch > 0x258000)
        pitch = 0x258000;
    for (u32 h : s.handle)
        if (s3->IsValid(h))
            s3->SetPitch(h, pitch);
}

// @port ov003:020f6dc8 done
void dsCarAudio::Horn()
{
    if (!m_ready || dsSound3D::Get()->IsValid(m_horn.handle[0]))
        return;
    m_horn.volume = m_master < 0x7f ? m_master : 0x7f;
    Start(m_player.pos, m_horn, false);
}

// @port ov003:0210ed9c partial (its sound part: skids from the rear wheels' surfaces and the lateral speed; a
// collision sound when the body contacts gave an impulse, at most every 0.5 s)
void dsCarAudio::PlayerUpdate(const dsCarDynamics &car)
{
    // The car entity's copy of the engine: the gear, and the rpm eased toward the engine's, by 0.3 of the gap per
    // frame going down (0x0210f300) and 0.1 going up (0x0210f304).
    if (car.engine) {
        m_engineGear = car.engine->gear;
        fx32 k = m_engineRpm > car.engine->rpm ? 0x4cd : 0x19a;
        m_engineRpm += (fx32)(((s64)(car.engine->rpm - m_engineRpm) * k + 0x800) >> 12);
    }
    if (!m_ready)
        return;
    s32 w2 = car.wheels.size() > 3 ? car.wheels[2]->surface : -1;
    s32 w3 = car.wheels.size() > 3 ? car.wheels[3]->surface : -1;
    bool tarmac = w2 == 0 || w3 == 0, dirt = w2 != 0 || w3 != 0;
    bool sliding = 0x1666 < car.lateralSpeed || car.lateralSpeed < -0x1665;
    // ov003 0x021226f8: touching the ground (body contacts on a floor, or wheels down)
    bool ground = (0 < car.bodyHitCount && 0xccd < car.hitNormalSum.y) || 0 < car.groundedWheels;
    Skid(sliding && ground, tarmac, dirt);
    if (car.impulseDone && m_collisionWait < 1) {
        VecFx32 n = car.hitNormalSum, v = car.velocity;
        if (n.x == 0 && n.y == 0 && n.z == 0)
            n = {0, 0, FX32_ONE};
        else
            dsVec_Normalize(&n, &n);
        if (v.x == 0 && v.y == 0 && v.z == 0)
            v = {0, 0, FX32_ONE};
        else
            dsVec_Normalize(&v, &v);
        const MtxFx43 &m = car.matrix;
        VecFx32 off = {MulRound(car.hitPointSum.x - m.m[3][0], 0x5000), MulRound(car.hitPointSum.y - m.m[3][1], 0x5000),
                       MulRound(car.hitPointSum.z - m.m[3][2], 0x5000)};
        u32 r = (u32)m_random.mt();
        fx32 impact = (fx32)(((s64)v.z * n.z + (s64)v.x * n.x + (s64)n.y * v.y + 0x800) >> 12);
        Collision((int)(r & 3), off, car.speed, impact);
        m_collisionWait = 0x800;
    }
    if (0 < m_collisionWait)
        m_collisionWait -= g_dsFrameTime;
}
