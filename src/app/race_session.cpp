// Race session shared by the host and PSP race drivers (see race_session.h).
#include "race_session.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include <algorithm>
#include <map>
#include <string>

#include "collision/dsCollisionWorld.h"
#include "core/dsTime.h"
#include "fs/dsFileSystem.h"
#include "math/dsMath.h"
#include "nitro.h"
#include "track/dsPieceData.h"
#include "track/dsRaceSetup.h"
#include "track/dsTrackLighting.h"

bool RaceReadFile(const std::string &path, std::vector<u8> *out)
{
    dsFile *f = dsFileSystem::Get()->Open(path.c_str());
    if (!f || !f->IsOpen()) {
        delete f;
        return false;
    }
    out->resize((size_t)f->GetSize());
    bool ok = !out->empty() && f->Read(out->data(), (u32)out->size()) == (int)out->size();
    delete f;
    return ok;
}

G3Model *RaceLoadModel(const std::string &name, bool upload)
{
    std::vector<u8> d;
    if (!RaceReadFile("graphics/" + name + ".n", &d))
        return nullptr;
    G3Model *m = new G3Model;
    if (!m->Load(d)) {
        delete m;
        return nullptr;
    }
    if (upload)
        m->Upload();
    return m;
}

// DS 4x3 matrix (row vectors, fx32) to a column-major 4x4 float matrix.
void RaceMatrix(const MtxFx43 &m, float *g)
{
    for (int r = 0; r < 4; r++)
        for (int c = 0; c < 3; c++)
            g[r * 4 + c] = m.m[r][c] / 4096.0f;
    g[3] = g[7] = g[11] = 0;
    g[15] = 1;
}

// @port ov003:0211dc68 partial (light vectors and colours; the colour sets reach the renderer +0xdc / +0xe0)
// @port main:0204f528 partial (shininess and fog tables only)
bool RaceBuildLighting(const char *env, const char *timeOfDay, G3LightEnv *out)
{
    dsTrackLighting tl;
    if (!tl.Load(env, timeOfDay))
        return false;
    // fx32 0..1 -> 5 bits as the renderer does (_s32_div_f(v, 0x84)).
    auto rgb = [](const VecFx32 &c) {
        auto k = [](fx32 v) { return std::min(31, std::max(0, (int)(v / 0x84))); };
        return (u16)(k(c.x) | k(c.y) << 5 | k(c.z) << 10);
    };
    G3LightEnv e;
    // ov003 0x0211dc68: lights 0 and 1 along (-4095, -5119, 0), lights 2 and 3 along (2048, -6757, 2048),
    // all white (0x1000 -> 31); the vectors are normalised by the game (dsVec_Normalize).
    const float d01[3] = {-4095, -5119, 0}, d23[3] = {2048, -6757, 2048};
    for (int i = 0; i < 4; i++) {
        const float *d = i < 2 ? d01 : d23;
        float l = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
        for (int k = 0; k < 3; k++) {
            e.lights[i].dir[k] = d[k] / l;
            e.lights[i].color[k] = 1;
        }
    }
    // Renderer +0xdc: set 0 (lights mask 3) = colours 1, 2, 3; set 1 (mask 0xc) = colours 5, 6, 7, as ambient,
    // diffuse and specular (main 0x02055970); set 0 also turns the shininess table on.
    e.sets[0] = {3, rgb(tl.colors[1]), rgb(tl.colors[2]), rgb(tl.colors[3]), true};
    e.sets[1] = {0xc, rgb(tl.colors[5]), rgb(tl.colors[6]), rgb(tl.colors[7]), false};
    // main 0x0204f528: shininess[i] = (2i + 1)^9 / 256^8 (in 8 bits), the last entry 255.
    for (int i = 0; i < 127; i++) {
        u32 v = (u32)(2 * i + 1) & 0xff, x = v, p = 0;
        for (int n = 0; n < 8; n++) {
            p = v * x;
            x = p >> 8;
        }
        e.shininess[i] = (u8)(p >> 8);
    }
    e.shininess[127] = 255;
    // Fog: colour 0, G3X_SetFog(on, colour + alpha, shift 1, offset 0x60) with the linear table 0, 4, .. 124 of
    // main 0x0204f528. The game draws in W-buffer mode (SWAP_BUFFERS = 2), where the fog compares the top 15 bits of
    // the 24-bit W (1.0 = 0x1000), i.e. 8 per world unit: density 0 at w = 0x60 / 8 = 12 units, rising by
    // 4/127 every 0x200 / 8 = 64 units (full at 2044 units). Port reading, not checked on hardware; the fog alpha
    // (the 3D layer fading into the 2D backdrop) is not reproduced.
    e.fog = true;
    e.fogColor = rgb(tl.colors[0]);
    e.fogStart = 0x60 / 8.0f;
    e.fogEnd = e.fogStart + 127.0f / 4 * 0x200 / 8;
    *out = e;
    return true;
}

static void RotationOf(const MtxFx43 &m, float *rot)
{
    float g[16];
    RaceMatrix(m, g);
    for (int c = 0; c < 3; c++) {
        float l = sqrtf(g[c * 4] * g[c * 4] + g[c * 4 + 1] * g[c * 4 + 1] + g[c * 4 + 2] * g[c * 4 + 2]);
        for (int r = 0; r < 3; r++)
            rot[c * 3 + r] = l > 0 ? g[c * 4 + r] / l : 0;
    }
}

const G3DrawLight *RaceSession::PieceLight(size_t k, G3DrawLight *out) const
{
    if (!lighting)
        return nullptr;
    *out = G3DrawLight();
    out->env = &lightEnv;
    out->set = 0;
    RotationOf(pieces[k].second->matrix, out->rot);
    out->variant = pieces[k].second->rotation & 3;
    return out;
}

const G3DrawLight *RaceSession::CarLight(G3DrawLight *out) const
{
    if (!lighting)
        return nullptr;
    *out = G3DrawLight();
    out->env = &lightEnv;
    out->set = 1;
    RotationOf(car->matrix, out->rot);
    const MtxFx43 &cm = cams[camIndex].matrix;
    float f[3] = {cm.m[2][0] / 4096.0f, cm.m[2][1] / 4096.0f, cm.m[2][2] / 4096.0f};
    float l = sqrtf(f[0] * f[0] + f[1] * f[1] + f[2] * f[2]);
    for (int i = 0; i < 3; i++)
        out->eye[i] = l > 0 ? f[i] / l : 0;
    out->specular = true;
    out->variant = -1;
    return out;
}

int RaceSession::CarDraws(Draw *out) const
{
    int n = 0;
    auto add = [&](const G3Model *m, const MtxFx43 &mtx) {
        if (!m)
            return;
        Draw &d = out[n++];
        d.model = m;
        RaceMatrix(mtx, d.matrix);
        d.lit = CarLight(&d.light) != nullptr;
        if (d.lit)
            RotationOf(mtx, d.light.rot);
    };
    add(carModel, car->matrix);
    for (int i = 0; i < 4; i++)
        add(wheelModels[i == 0 || i == 2 ? 0 : 1], car->wheelMatrix[i]);
    return n;
}

// The material controllers (itcm 0x01fff814 / 0x01fff858): a light's shapes are drawn while its level is not 0,
// with the level as polygon alpha and its colour (brake red 0x1f, reverse and head white) as DIF_AMB and SPE_EMI.
void RaceSession::ApplyCarLights()
{
    static const u16 colors[3] = {0x1f, 0x7fff, 0x7fff}; // main 0x0206b0xx
    for (int l = 0; l < 3; l++) {
        int m = carLightMaterial[l];
        if (!carModel || m < 0 || m >= (int)carModel->overrides.size())
            continue;
        G3Model::MatOverride &o = carModel->overrides[m];
        o.hidden = carLights.level[l] == 0;
        o.alpha = carLights.level[l] & 31;
        o.color = colors[l];
    }
}

// The car visual's update (0x020fc6b4) places the shadow model (+0x10, shown while the car's LOD is below 3,
// 0x020fbf20 area) at the car's display matrix, moved by the visual's vector +0x88 x 0x11f (never written in the
// race: zero) and lowered by 0x666 (0.4). Its material "lambert1" gives the colour (DIF 0x318c, the vertex colour
// unlit: port reading) and the shadow passes keep its polygon alpha 0x0e (itcm 0x01ffc7a4: mask attr | 0x000e0070).
bool RaceSession::ShadowDraw(float *matrix, u16 *rgb555, int *alpha) const
{
    if (!shadowModel || !car)
        return false;
    MtxFx43 m = car->matrix;
    m.m[3][1] -= 0x666;
    RaceMatrix(m, matrix);
    *rgb555 = shadowModel->materials.empty() ? 0 : (u16)(shadowModel->materials[0].difAmb & 0x7fff);
    *alpha = 0x0e;
    return true;
}

RaceSession::~RaceSession()
{
    pieces.clear();
    models.clear(); // G3Model's destructor frees the back-end textures and vertex arrays
}

void RaceUnloadWorld() { dsCollisionWorld::Get()->Clear(); }

bool RaceSession::Load(const char *trackName)
{
    dsRaceSetup *setup = dsRaceSetup::Get();
    setup->mode = 1;
    setup->SetEnvironment(ENV_COAST);
    dsPieceDataTable::Get()->Load(ENV_COAST);
    std::string scene = std::string("coa/") + trackName;
    if (!track.LoadScene(scene.c_str(), true)) {
        fprintf(stderr, "cannot load scene %s\n", scene.c_str());
        return false;
    }
    int terrain = track.BuildTerrain();
    printf("terrain: %d pieces\n", terrain);
    for (dsTrackPiece *p : track.pieces)
        p->CreateCollision();
    table.Load();
    if (table.All().find("coa") == table.All().end())
        return false;
    const dsCarParams &prm = table.Get("coa");
    if (!geo.Load("coa", 0, prm))
        return false;
    car.reset(new dsCarDynamics("coa", &geo));
    car->LoadParams(prm);
    car->pieceRotationAt = [this](const VecFx32 &p) { return track.PieceRotationAt(p); };

    // Start, triggers and countdown: the race mode's full reset (ov003 0x02115af4).
    progress.reset(new dsRaceProgress(&track, car.get()));
    progress->Reset(true);
    if (!progress->startPiece) {
        fprintf(stderr, "no start piece\n");
        return false;
    }
    for (dsChaseCamera &c : cams) {
        if (!c.Load("coa"))
            fprintf(stderr, "config/cameras: no coa entry\n");
        c.Reset();
    }

    auto own = [this](G3Model *m) {
        if (m)
            models.emplace_back(m);
        return m;
    };
    // material.bin replaces the batch's texture list: upload once, after it (an upload before leaked the first
    // list's back-end textures on every track load).
    batch = own(RaceLoadModel("Tracks/coa/texturebatch", false));
    std::vector<u8> mb;
    if (batch) {
        if (RaceReadFile("graphics/Tracks/coa/material.bin", &mb))
            batch->LoadMaterialBin(mb);
        batch->Upload();
    }
    std::map<std::string, G3Model *> cache;
    for (const dsTrackPiece *p : track.pieces) {
        auto it = cache.find(p->modelPath);
        if (it == cache.end()) {
            G3Model *m = own(RaceLoadModel(p->modelPath));
            // The texture SRT animation next to the model (main 0x02055f78: the arrow signs' scrolling lights).
            std::vector<u8> na;
            if (m && batch && RaceReadFile("graphics/" + p->modelPath + ".na", &na))
                m->LoadTexAnim(na, m->batchMaterials ? *batch : *m);
            it = cache.emplace(p->modelPath, m).first;
        }
        if (it->second)
            pieces.push_back({it->second, p});
        if (it->second && !it->second->hasBounds)
            it->second->ComputeBounds();
    }
    // lod1: the body at the physics scale (lod0 adds the wheel, shadow and light-cone shapes that the game
    // places itself in the visual update 0x02126d10, not ported yet).
    carModel = own(RaceLoadModel("Cars/coa/0/lod1"));
    shadowModel = own(RaceLoadModel("Cars/coa/shadow", false));
    carLights.Reset(); // ov003 0x020fd914
    G3SetTexAnimFrame(texAnimFrame = 0);
    if (carModel) {
        static const char *const lightNames[3] = {"blight", "rlight", "hlight"};
        carModel->overrides.assign(carModel->materials.size(), G3Model::MatOverride());
        for (int l = 0; l < 3; l++) {
            carLightMaterial[l] = -1;
            for (size_t i = 0; i < carModel->materials.size(); i++)
                if (carModel->materials[i].name == lightNames[l])
                    carLightMaterial[l] = (int)i;
        }
        ApplyCarLights();
    }
    // Wheels: lwheel for 0 and 2, rwheel for 1 and 3 (ov003 0x020fd914 picks the name per wheel; rwheel has no
    // textures of its own and shows lwheel's).
    G3Model *lw = RaceLoadModel("Cars/coa/lwheel", false), *rw = RaceLoadModel("Cars/coa/rwheel", false);
    if (lw && rw)
        rw->BorrowTextures(*lw);
    for (G3Model *w : {lw, rw})
        if (w)
            w->Upload();
    wheelModels[0] = own(lw);
    wheelModels[1] = own(rw);
    // Lighting of the track's time of day (the race setup takes it from the track, as the game's menus do).
    setup->timeOfDay = track.info.timeOfDay;
    if (!RaceBuildLighting(setup->EnvironmentName(), setup->TimeOfDayName(), &lightEnv))
        lighting = false;
    // Sky dome: Sky/<env>/sky_<tod>, scaled (7987, 6349, 7987) / 4096 on Coast (ov003 0x0211de20; Island uses
    // (10035, 7168, 10035)); drawn around the camera by the drivers.
    skyModel = own(RaceLoadModel(std::string("Sky/") + setup->EnvironmentName() + "/sky_" + setup->TimeOfDayName()));
    if (skyModel)
        skyModel->guardEdge = 150; // 1,370 units away: about 6 degrees
    if (setup->environment == ENV_ISLAND) {
        skyScale[0] = skyScale[2] = 10035 / 4096.0f;
        skyScale[1] = 7168 / 4096.0f;
    } else {
        skyScale[0] = skyScale[2] = 7987 / 4096.0f;
        skyScale[1] = 6349 / 4096.0f;
    }
    // Back-end conversion ahead of the race (PSP: vertex arrays with the lit colours of each piece rotation).
    G3DrawLight dl;
    for (size_t k = 0; k < pieces.size(); k++) {
        G3SetDrawLight(PieceLight(k, &dl));
        pieces[k].first->Prepare(batch);
    }
    G3SetDrawLight(nullptr);
    Draw cd[5];
    for (int i = 0, n = CarDraws(cd); i < n; i++) {
        G3SetDrawLight(cd[i].lit ? &cd[i].light : nullptr); // lit: geometry and normals cached, colours per draw
        cd[i].model->Prepare(nullptr);
    }
    G3SetDrawLight(nullptr);
    if (skyModel)
        skyModel->Prepare(nullptr);
    hud.Init(progress->lapCount);
    printf("track %s: %d pieces drawn, batch %s, car model %s\n", trackName, (int)pieces.size(),
           batch ? "ok" : "missing", carModel ? "ok" : "missing");
    return true;
}

void RaceSession::Cull(float aspect, float nearZ, float farZ)
{
    // Camera basis as the drivers' look-at: f = forward, s = f x up, u = s x f.
    const MtxFx43 &cm = cams[camIndex].matrix;
    float eye[3], f[3], up[3];
    for (int i = 0; i < 3; i++) {
        eye[i] = cm.m[3][i] / 4096.0f;
        f[i] = cm.m[2][i] / 4096.0f;
        up[i] = cm.m[1][i] / 4096.0f;
    }
    auto norm = [](float *v) {
        float l = sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
        if (l > 0)
            for (int i = 0; i < 3; i++)
                v[i] /= l;
    };
    norm(f);
    float s[3] = {f[1] * up[2] - f[2] * up[1], f[2] * up[0] - f[0] * up[2], f[0] * up[1] - f[1] * up[0]};
    norm(s);
    float u[3] = {s[1] * f[2] - s[2] * f[1], s[2] * f[0] - s[0] * f[2], s[0] * f[1] - s[1] * f[0]};
    float ty = tanf(cams[camIndex].fov / 4096.0f * 0.5f * 3.14159265f / 180), tx = ty * aspect;
    // Side planes through the eye: normal (cos a, -sin a) in the (x, z) plane, tan a = tx (resp. ty).
    float cx = 1 / sqrtf(1 + tx * tx), sx = tx * cx, cy = 1 / sqrtf(1 + ty * ty), sy = ty * cy;
    visible.assign(pieces.size(), 1);
    float g[16];
    for (size_t k = 0; k < pieces.size(); k++) {
        const G3Model *m = pieces[k].first;
        if (!m->hasBounds)
            continue;
        RaceMatrix(pieces[k].second->matrix, g);
        float c[3], scale = 0;
        for (int i = 0; i < 3; i++) {
            c[i] = g[i] * m->center[0] + g[4 + i] * m->center[1] + g[8 + i] * m->center[2] + g[12 + i] - eye[i];
            float col = sqrtf(g[i * 4] * g[i * 4] + g[i * 4 + 1] * g[i * 4 + 1] + g[i * 4 + 2] * g[i * 4 + 2]);
            scale = col > scale ? col : scale;
        }
        // Margin: the fx32 camera basis is not exactly orthonormal.
        float r = m->radius * scale * 1.02f + 0.5f;
        float x = c[0] * s[0] + c[1] * s[1] + c[2] * s[2];
        float y = c[0] * u[0] + c[1] * u[1] + c[2] * u[2];
        float z = c[0] * f[0] + c[1] * f[1] + c[2] * f[2];
        if (z < nearZ - r || z > farZ + r || fabsf(x) * cx - z * sx > r || fabsf(y) * cy - z * sy > r)
            visible[k] = 0;
    }
}

void RaceSession::Frame(const dsCarInput &in)
{
    progress->Frame(in, 2);
    for (; shownEvents < progress->events.size(); shownEvents++) {
        static const char *const names[] = {"restart", "go", "checkpoint", "missed", "lap", "finish", "respawn"};
        const dsRaceEvent &ev = progress->events[shownEvents];
        printf("[%d.%02d] %s %d\n", ev.time >> 12, ((ev.time & 0xfff) * 100) >> 12, names[ev.type], ev.value);
    }
    if (resets != progress->events.size()) // restart / respawn: the camera starts over
        for (; resets < progress->events.size(); resets++) {
            dsRaceEvent::Type ty = progress->events[resets].type;
            if (ty == dsRaceEvent::RESTART || ty == dsRaceEvent::RESPAWN)
                cams[camIndex].Reset();
        }
    cams[camIndex].Update(*car); // camera vtable +0x1c, once per frame
    G3SetTexAnimFrame(++texAnimFrame); // material animations: one frame per 30 Hz frame (port choice)
    // The player's update (0x0210ed9c) sets the lights once per 30 Hz frame from the pad and the engine's gear.
    carLights.Update(in.accel > 0, in.brake > 0, car->engine ? car->engine->gear : 0);
    ApplyCarLights();
    hud.Update(*progress, car->speed, 2, recordSplits, recordTime); // end of dsRace_Update (0x02113048)
}

G2CellBank *const *RaceHudBanks()
{
    static G2CellBank *banks[HUD_BANK_COUNT];
    static bool loaded = false;
    if (!loaded) {
        loaded = true;
        for (int b = 0; b < HUD_BANK_COUNT; b++) {
            const char *ncer, *ncgr, *nclr;
            int cells;
            dsHud::BankFiles(b, &ncer, &ncgr, &nclr, &cells);
            std::vector<u8> e, g, c;
            G2CellBank *bank = new G2CellBank;
            if (RaceReadFile(ncer, &e) && RaceReadFile(ncgr, &g) && RaceReadFile(nclr, &c) &&
                bank->Load(g, e, c, cells)) {
                banks[b] = bank;
                printf("hud bank %d: %d cells, atlas %d x %d\n", b, (int)bank->cells.size(), bank->width,
                       bank->height);
            } else {
                printf("hud bank %d: %s missing\n", b, ncer);
                delete bank;
            }
        }
    }
    return banks;
}

void RaceHudSprites(int w, int h, const dsHud &hud, RaceSpriteFn sprite, void *user)
{
    G2CellBank *const *banks = RaceHudBanks();
    float k = h / 192.0f, ox = w / 2.0f - 128 * k;
    for (const dsHudSprite &s : hud.sprites) {
        const G2CellBank *b = banks[s.bank];
        if (!s.visible || !b || s.cell < 0 || s.cell >= (int)b->cells.size())
            continue;
        const G2Cell &c = b->cells[s.cell];
        if (!c.w)
            continue;
        // Wider than 4:3 (the PSP): the parts on the sides follow the screen edges.
        float x = ox + s.anchor * ox + (s.x / 4096.0f + c.x0) * k, y = (s.y / 4096.0f + c.y0) * k;
        sprite(*b, c, x, y, c.w * k, c.h * k, user);
    }
}

// Runs of equal alpha in the backing texture, rows with the same runs merged: {x0, x1, y0, y1, alpha} in texels.
struct BackingRun {
    u8 x0, x1, y0, y1, a;
};
static const std::vector<BackingRun> &BackingRuns()
{
    static std::vector<BackingRun> runs;
    static bool loaded = false;
    if (loaded)
        return runs;
    loaded = true;
    G3Model *m = RaceLoadModel("hud/backing", false);
    if (m && !m->textures.empty() && m->textures[0].w <= 256 && m->textures[0].h <= 256) {
        const G3Texture &t = m->textures[0];
        std::vector<BackingRun> prev;
        for (int y = 0; y < t.h; y++) {
            std::vector<BackingRun> row;
            for (int x = 0; x < t.w;) {
                u8 a = t.rgba[((size_t)y * t.w + x) * 4 + 3];
                int e = x;
                while (e + 1 < t.w && t.rgba[((size_t)y * t.w + e + 1) * 4 + 3] == a)
                    e++;
                if (a)
                    row.push_back({(u8)x, (u8)e, (u8)y, (u8)y, a});
                x = e + 1;
            }
            bool same = row.size() == prev.size();
            for (size_t i = 0; same && i < row.size(); i++)
                same = row[i].x0 == prev[i].x0 && row[i].x1 == prev[i].x1 && row[i].a == prev[i].a;
            if (same && !row.empty()) {
                for (size_t i = 0; i < row.size(); i++)
                    runs[runs.size() - row.size() + i].y1 = (u8)y;
            } else {
                runs.insert(runs.end(), row.begin(), row.end());
            }
            prev = row;
        }
    }
    delete m;
    return runs;
}

void RaceHudBacking(int w, int h, const dsHud &hud, RaceRectFn rect, void *user)
{
    const std::vector<BackingRun> &runs = BackingRuns();
    dsHudQuad q[4];
    float k = h / 192.0f, ox = w / 2.0f - 128 * k;
    for (int i = 0, n = hud.BackingQuads(q); i < n; i++) {
        // Texel (s, t) -> DS pixel: x across s0..s1, y (from the bottom) across t0..t1.
        float sx = (float)q[i].w / (q[i].s1 - q[i].s0), sy = (float)q[i].h / (q[i].t1 - q[i].t0);
        for (const BackingRun &r : runs) {
            float xa = q[i].x + (r.x0 - q[i].s0) * sx, xb = q[i].x + (r.x1 + 1 - q[i].s0) * sx;
            float ya = q[i].y + (r.y0 - q[i].t0) * sy, yb = q[i].y + (r.y1 + 1 - q[i].t0) * sy;
            float x0 = xa < xb ? xa : xb, x1 = xa < xb ? xb : xa;
            float top = 192 - (ya > yb ? ya : yb), bottom = 192 - (ya > yb ? yb : ya);
            rect(ox + q[i].anchor * ox + x0 * k, top * k, (x1 - x0) * k, (bottom - top) * k, 0, 0, 0, r.a / 255.0f,
                 user);
        }
    }
}

// ---- Text stand-in: a 5 x 7 bitmap font (menus and the HUD messages) ----
struct Glyph {
    char c;
    const char *rows[7];
};
static const Glyph kFont[] = {
    {'0', {"01110", "10001", "10011", "10101", "11001", "10001", "01110"}},
    {'1', {"00100", "01100", "00100", "00100", "00100", "00100", "01110"}},
    {'2', {"01110", "10001", "00001", "00010", "00100", "01000", "11111"}},
    {'3', {"11110", "00001", "00001", "01110", "00001", "00001", "11110"}},
    {'4', {"00010", "00110", "01010", "10010", "11111", "00010", "00010"}},
    {'5', {"11111", "10000", "11110", "00001", "00001", "10001", "01110"}},
    {'6', {"00110", "01000", "10000", "11110", "10001", "10001", "01110"}},
    {'7', {"11111", "00001", "00010", "00100", "01000", "01000", "01000"}},
    {'8', {"01110", "10001", "10001", "01110", "10001", "10001", "01110"}},
    {'9', {"01110", "10001", "10001", "01111", "00001", "00010", "01100"}},
    {':', {"00000", "00100", "00100", "00000", "00100", "00100", "00000"}},
    {'.', {"00000", "00000", "00000", "00000", "00000", "01100", "01100"}},
    {'/', {"00001", "00010", "00010", "00100", "01000", "01000", "10000"}},
    {'!', {"00100", "00100", "00100", "00100", "00100", "00000", "00100"}},
    {'A', {"01110", "10001", "10001", "11111", "10001", "10001", "10001"}},
    {'C', {"01110", "10001", "10000", "10000", "10000", "10001", "01110"}},
    {'E', {"11111", "10000", "10000", "11110", "10000", "10000", "11111"}},
    {'G', {"01110", "10001", "10000", "10111", "10001", "10001", "01111"}},
    {'I', {"01110", "00100", "00100", "00100", "00100", "00100", "01110"}},
    {'K', {"10001", "10010", "10100", "11000", "10100", "10010", "10001"}},
    {'M', {"10001", "11011", "10101", "10101", "10001", "10001", "10001"}},
    {'N', {"10001", "11001", "10101", "10011", "10001", "10001", "10001"}},
    {'O', {"01110", "10001", "10001", "10001", "10001", "10001", "01110"}},
    {'P', {"11110", "10001", "10001", "11110", "10000", "10000", "10000"}},
    {'Q', {"01110", "10001", "10001", "10001", "10101", "10010", "01101"}},
    {'R', {"11110", "10001", "10001", "11110", "10100", "10010", "10001"}},
    {'S', {"01111", "10000", "10000", "01110", "00001", "00001", "11110"}},
    {'T', {"11111", "00100", "00100", "00100", "00100", "00100", "00100"}},
    {'U', {"10001", "10001", "10001", "10001", "10001", "10001", "01110"}},
    {'V', {"10001", "10001", "10001", "10001", "10001", "01010", "00100"}},
    {'B', {"11110", "10001", "10001", "11110", "10001", "10001", "11110"}},
    {'D', {"11100", "10010", "10001", "10001", "10001", "10010", "11100"}},
    {'F', {"11111", "10000", "10000", "11110", "10000", "10000", "10000"}},
    {'H', {"10001", "10001", "10001", "11111", "10001", "10001", "10001"}},
    {'J', {"00111", "00010", "00010", "00010", "00010", "10010", "01100"}},
    {'L', {"10000", "10000", "10000", "10000", "10000", "10000", "11111"}},
    {'W', {"10001", "10001", "10001", "10101", "10101", "10101", "01010"}},
    {'X', {"10001", "10001", "01010", "00100", "01010", "10001", "10001"}},
    {'Y', {"10001", "10001", "01010", "00100", "00100", "00100", "00100"}},
    {'Z', {"11111", "00001", "00010", "00100", "01000", "10000", "11111"}},
    {'-', {"00000", "00000", "00000", "11111", "00000", "00000", "00000"}},
    {'>', {"01000", "00100", "00010", "00001", "00010", "00100", "01000"}},
    {'<', {"00010", "00100", "01000", "10000", "01000", "00100", "00010"}},
};

const char *const *RaceGlyph(char c)
{
    for (const Glyph &f : kFont)
        if (f.c == c)
            return f.rows;
    return nullptr;
}

void RaceHudLayout(int w, int h, float s, const dsRaceProgress &rp, RaceTextFn text, void *user)
{
    // The game's HUD (dsHud: countdown, speed, laps, splits, clock) is drawn as sprites; only its text messages
    // (+0x3c, localised strings, not ported) keep a stand-in here.
    char buf[64];
    const dsRaceEvent *last = rp.events.empty() ? nullptr : &rp.events.back();
    if (rp.finished) {
        text(w / 2 - 6 * 6 * 8 / 2 * s, h * 0.42f, 6 * s, "ARRIVEE!", 0.3f, 1, 0.3f, user);
    } else if (last && last->type == dsRaceEvent::MISSED && rp.clock - last->time < 0x2000) {
        snprintf(buf, sizeof buf, "%d CP MANQUE%s", last->value, last->value > 1 ? "S" : "");
        text(w / 2 - 6 * 5 * (int)strlen(buf) / 2 * s, h * 0.42f, 5 * s, buf, 1, 0.3f, 0.3f, user);
    }
}
