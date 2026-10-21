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
#include "sound/dsCarAudio.h"
#include "sound/dsGameAudio.h"
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
    out->set = 1; // track model (main 0x02055970, flag +0x71)
    RotationOf(pieces[k].second->matrix, out->rot);
    out->variant = pieces[k].second->rotation & 3;
    return out;
}

const G3DrawLight *RaceSession::SceneryLight(size_t i, G3DrawLight *out) const
{
    if (!lighting)
        return nullptr;
    *out = G3DrawLight();
    out->env = &lightEnv;
    out->set = 1; // misc model (main 0x02055970)
    RotationOf(scenery[i].matrix, out->rot);
    out->variant = scenery[i].variant;
    return out;
}

const G3DrawLight *RaceSession::CarLight(G3DrawLight *out) const
{
    if (!lighting)
        return nullptr;
    *out = G3DrawLight();
    out->env = &lightEnv;
    out->set = 0; // car model (main 0x02055970, flag +0x70)
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
    auto add = [&](const G3Model *m, const MtxFx43 &mtx, bool ghost = false) {
        if (!m)
            return;
        Draw &d = out[n++];
        d.model = m;
        d.ghost = ghost;
        RaceMatrix(mtx, d.matrix);
        d.lit = CarLight(&d.light) != nullptr;
        if (d.lit)
            RotationOf(mtx, d.light.rot);
    };
    if (car) {
        add(carModel, car->matrix);
        for (int i = 0; i < 4; i++)
            add(wheelModels[i == 0 || i == 2 ? 0 : 1], car->wheelMatrix[i]);
        // Ghosts: the body only, at their recorded matrix for the race clock's tick, until the end of their run.
        int tick = (int)(((s64)progress->clock * dsGhost::TICKS_PER_SECOND) >> FX32_SHIFT);
        for (size_t g = 0; g < ghosts.size() && g < MAX_GHOSTS; g++) {
            MtxFx43 m;
            if (tick < ghosts[g]->Ticks() && ghosts[g]->Matrix(tick, &m))
                add(carModel, m, true);
        }
    }
    for (size_t i = 0; i < editorDraws.size() && n < MAX_CAR_DRAWS; i++)
        out[n++] = editorDraws[i];
    return n;
}

// A ghost's look: every material of the body at polygon alpha 12 (the light shapes stay as they are), so that the
// DS's translucent pass draws it. The car's own overrides are put back after the ghost draws.
// The editor's block under the cursor (ghost 2, or 3 where it cannot go) gets the same alpha on every material of
// its model (the batch's for piece models), tinted red when it cannot be placed.
void RaceSession::GhostLook(const Draw &d, bool on)
{
    G3Model *model = d.ghost == 1 ? carModel : const_cast<G3Model *>(d.model);
    if (!model)
        return;
    if (!on) {
        model->overrides = savedOverrides;
        return;
    }
    savedOverrides = model->overrides;
    size_t count = model->batchMaterials && batch ? batch->materials.size() : model->materials.size();
    model->overrides.resize(count);
    for (size_t m = 0; m < model->overrides.size(); m++) {
        bool light = false;
        if (d.ghost == 1)
            for (int l : carLightMaterial)
                light |= l == (int)m;
        if (light) {
            model->overrides[m].hidden = true;
            continue;
        }
        model->overrides[m].alpha = d.ghost == 1 ? 12 : d.ghost == 4 ? 20 : 16;
        if (d.ghost == 3)
            model->overrides[m].color = 0x001f;
    }
}

RaceSession::Draw RaceSession::PieceDraw(const G3Model *m, const MtxFx43 &mtx, int rotation, int ghost) const
{
    Draw d;
    d.model = m;
    d.ghost = ghost;
    RaceMatrix(mtx, d.matrix);
    d.lit = false;
    if (lighting) {
        d.light = G3DrawLight();
        d.light.env = &lightEnv;
        d.light.set = 1;
        RotationOf(mtx, d.light.rot);
        d.light.variant = rotation & 3;
        d.lit = true;
    }
    return d;
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

// The start lights: a light's shapes are drawn while its level is not 0, with the level as polygon alpha and its
// colour (red 0x1f, amber 0x3ef, green 0x3e0: main 0x0206aaf4) as DIF_AMB and SPE_EMI.
void RaceSession::ApplyStartLights()
{
    static const u16 colors[3] = {0x1f, 0x3ef, 0x3e0};
    for (const StartLight &s : startLights) {
        G3Model::MatOverride &o = s.model->overrides[s.material];
        o.hidden = startLevel[s.light] == 0;
        o.alpha = startLevel[s.light] & 31;
        o.color = colors[s.light];
    }
}

// The car visual's update (0x020fc6b4) places the shadow model (+0x10, shown while the car's LOD is below 3,
// 0x020fbf20 area) at the car's display matrix, moved by the visual's vector +0x88 x 0x11f (never written in the
// race: zero) and lowered by 0x666 (0.4). The draw (itcm 0x01ffc7a4) sends the material "lambert1" with its light
// enables cleared (mask attr & 0xc0e0ff00 | 0x000e0070, shadow attr & 0xc0ffff30 | 0x020000c0); every vertex follows a
// NORMAL command, which with no light enabled sets the vertex colour to the emission (SPE_EMI bits 16-30: 0, black),
// not the diffuse 0x318c. Polygon alpha 0x0e.
bool RaceSession::ShadowDraw(float *matrix, u16 *rgb555, int *alpha) const
{
    if (!shadowModel || !car)
        return false;
    MtxFx43 m = car->matrix;
    m.m[3][1] -= 0x666;
    RaceMatrix(m, matrix);
    *rgb555 = shadowModel->materials.empty() ? 0 : (u16)((shadowModel->materials[0].specEmi >> 16) & 0x7fff);
    *alpha = 0x0e;
    return true;
}

RaceSession::~RaceSession()
{
    if (groundPlane)
        dsCollisionWorld::Get()->RemoveShape(groundPlane.get());
    pieces.clear();
    models.clear(); // G3Model's destructor frees the back-end textures and vertex arrays
}

void RaceUnloadWorld() { dsCollisionWorld::Get()->Clear(); }

// Positions of ov003 0x0211d37c (Stadium), 0x0211d02c (Island), 0x0211ccec (Snow): four nodes around the centre of the
// grid, the first two with name 0, the others with name 1; nodes 1 and 3 turned half a turn about y (MTX_RotY43_ by
// pi through FX_SinCosTable_, as a piece's two quarter turns). Coast (0x0211ca88): one landscape, turned likewise.
// The nodes take draw order 0x15 (after the models' 0x10), which the drivers' separate pass keeps.
// @port ov003:0211d37c done
// @port ov003:0211d02c done
// @port ov003:0211ccec done
// @port ov003:0211ca88 done
void RaceSession::LoadScenery(int env)
{
    struct Layout {
        const char *names[2];
        fx32 pos[4][3];
        fx32 centre, y;
        int count;
        float guardEdge; // PSP split: edges kept 2.5 times shorter than the scenery's distance from the track
    };
    static const Layout s_layout[ENV_COUNT] = {
        {{"Tracks/sta/StadiumSide", "Tracks/sta/StadiumScreen"},
         {{0x1e0000, 0, 0}, {(fx32)0xffe00001, 0, 0}, {0, 0, (fx32)0xffe28001}, {0, 0, 0x1d8000}}, 0x140000, 0, 4, 28},
        {{"misc/island/large", "misc/island/small"},
         {{0x3cf000, 0, 0}, {(fx32)0xffc31001, 0, 0}, {0, 0, (fx32)0xffc63001}, {0, 0, 0x39d000}}, 0x280000,
         (fx32)0xffffc001, 4, 36},
        {{"misc/snow/lg_snow", "misc/snow/sm_snow"},
         {{0x24e000, 0, 0}, {(fx32)0xffdb2001, 0, 0}, {0, 0, (fx32)0xffdbc001}, {0, 0, 0x244000}}, 0x140000, 0, 4, 28},
        {{"misc/coast/coastbackground", nullptr}, {{0x177000, 0, 0x12c000}}, 0, 0, 1, 90},
    };
    const Layout &l = s_layout[env];
    for (int i = 0; i < l.count; i++) {
        G3Model *m = RaceLoadModel(l.names[i >> 1]);
        if (!m) {
            fprintf(stderr, "no model %s\n", l.names[i >> 1]);
            continue;
        }
        models.emplace_back(m);
        m->guardEdge = l.guardEdge;
        Scenery s;
        s.model = m;
        s.variant = env == ENV_COAST || (i & 1) ? 2 : 0;
        if (s.variant)
            MTX_RotY43_(&s.matrix, dsSin(0x3244), dsCos(0x3244));
        else
            dsMtx43_Identity(&s.matrix);
        s.matrix.m[3][0] = l.pos[i][0] + l.centre;
        s.matrix.m[3][1] = l.pos[i][1] + l.y;
        s.matrix.m[3][2] = l.pos[i][2] + l.centre;
        scenery.push_back(s);
    }
}

bool RaceSession::Load(const char *sceneName)
{
    // "<env>/<difficulty>/<n>": the environment's short name picks the piece catalogue, the car and the rest;
    // "pla/...": platform mode, the environment from the scene's header.
    int env = -1;
    mode = !strncmp(sceneName, "pla/", 4) ? 8 : 1;
    if (mode == 8) {
        dsTrack header;
        if (header.LoadScene(sceneName, false))
            env = header.info.environment;
    } else {
        for (int e = 0; e < ENV_COUNT; e++)
            if (!strncmp(sceneName, dsRaceSetup::EnvironmentName(e), 3) && sceneName[3] == '/')
                env = e;
    }
    if (env < 0 || env >= ENV_COUNT) {
        fprintf(stderr, "unknown environment in %s\n", sceneName);
        return false;
    }
    dsRaceSetup *setup = dsRaceSetup::Get();
    setup->mode = mode;
    setup->SetEnvironment(env);
    dsPieceDataTable::Get()->Load(env);
    std::string scene = sceneName;
    if (!track.LoadScene(scene.c_str(), true)) {
        fprintf(stderr, "cannot load scene %s\n", scene.c_str());
        return false;
    }
    return Build(sceneName);
}

bool RaceSession::LoadTrack(int env, int mode_, const dsTrackInfo &info, const std::vector<dsTrackPiece *> &src)
{
    mode = mode_;
    dsRaceSetup *setup = dsRaceSetup::Get();
    setup->mode = mode;
    setup->SetEnvironment(env);
    track.info = info;
    for (const dsTrackPiece *p : src)
        if (!p->autoTerrain && p->id >= 0)
            track.pieces.push_back(new dsTrackPiece(p->id, p->pos, p->rotation));
    return Build("editor track");
}

bool RaceSession::LoadView(int env, int timeOfDay_)
{
    editorView = true;
    mode = 6;
    dsRaceSetup *setup = dsRaceSetup::Get();
    setup->mode = mode;
    setup->SetEnvironment(env);
    track.info.environment = env;
    track.info.timeOfDay = timeOfDay_;
    return Build("editor view");
}

bool RaceSession::Build(const char *sceneName)
{
    dsRaceSetup *setup = dsRaceSetup::Get();
    const std::string envName = setup->EnvironmentName(); // also the car's name: "coa", "isl", "sno", "sta"
    int terrain = track.BuildTerrain();
    printf("terrain: %d pieces\n", terrain);
    for (dsTrackPiece *p : track.pieces)
        p->CreateCollision();
    table.Load();
    if (table.All().find(envName) == table.All().end())
        return false;
    const dsCarParams &prm = table.Get(envName);
    if (!editorView) {
        if (!geo.Load(envName, 0, prm))
            return false;
        car.reset(new dsCarDynamics(envName, &geo));
        car->LoadParams(prm);
        car->pieceRotationAt = [this](const VecFx32 &p) { return track.PieceRotationAt(p); };

        // Start, triggers and countdown: the race mode's full reset (ov003 0x02115af4). The editor's test drive
        // (mode 6) may have no start: the application places the car.
        progress.reset(new dsRaceProgress(&track, car.get()));
        progress->Reset(true);
        if (!progress->startPiece && mode != 6) {
            fprintf(stderr, "no start piece\n");
            return false;
        }
    }
    for (dsChaseCamera &c : cams) {
        if (!c.Load(envName.c_str()))
            fprintf(stderr, "config/cameras: no %s entry\n", envName.c_str());
        c.Reset();
    }

    // material.bin replaces the batch's texture list: upload once, after it (an upload before leaked the first
    // list's back-end textures on every track load).
    batch = Own(RaceLoadModel("Tracks/" + envName + "/texturebatch", false));
    std::vector<u8> mb;
    if (batch) {
        if (RaceReadFile("graphics/Tracks/" + envName + "/material.bin", &mb))
            batch->LoadMaterialBin(mb);
        batch->Upload();
    }
    SyncPieces(track.pieces);
    if (!editorView)
        LoadCar(envName);
    // Lighting of the track's time of day (the race setup takes it from the track, as the game's menus do for the
    // challenges; the quick race's time of day menu picks it).
    setup->timeOfDay = timeOfDay >= 0 && timeOfDay < 3 ? timeOfDay : track.info.timeOfDay;
    if (!RaceBuildLighting(setup->EnvironmentName(), setup->TimeOfDayName(), &lightEnv))
        lighting = false;
    LoadEnvironment();
    // Back-end conversion ahead of the race (PSP: vertex arrays with the lit colours of each piece rotation).
    G3DrawLight dl;
    for (size_t i = 0; i < scenery.size(); i++) {
        G3SetDrawLight(SceneryLight(i, &dl));
        scenery[i].model->Prepare(batch);
    }
    for (size_t k = 0; k < pieces.size(); k++) {
        G3SetDrawLight(PieceLight(k, &dl));
        for (int l = 0; l < G3_DETAIL_LEVELS; l++) {
            G3SetDrawDetail(l);
            pieces[k].first->Prepare(batch);
        }
    }
    G3SetDrawDetail(0);
    G3SetDrawLight(nullptr);
    Draw cd[MAX_CAR_DRAWS];
    for (int i = 0, n = CarDraws(cd); i < n; i++) {
        G3SetDrawLight(cd[i].lit ? &cd[i].light : nullptr); // lit: geometry and normals cached, colours per draw
        cd[i].model->Prepare(nullptr);
    }
    G3SetDrawLight(nullptr);
    if (skyModel)
        skyModel->Prepare(nullptr);
    if (editorView) {
        printf("%s: %d pieces drawn, batch %s\n", sceneName, (int)pieces.size(), batch ? "ok" : "missing");
        return true;
    }
    hud.Init(progress->lapCount);
    // Race sounds (main 0x0208c04c from the race loader 0x02097aa0): the environment, horn 0 (the game takes the
    // player's number modulo 8), output mode 1 (settings +0x2c = 0).
    // The editor's test drive and validation (mode 6) play the environment's ambience (environment + 4).
    dsGameAudio::Get()->StartRace(setup->environment + (mode == 6 ? 4 : 0), 0, 1, envName, prm);
    soundEvents = progress->events.size();
    countdownSound = -1;
    printf("track %s: %d pieces drawn, batch %s, car model %s\n", sceneName, (int)pieces.size(),
           batch ? "ok" : "missing", carModel ? "ok" : "missing");
    return true;
}

G3Model *RaceSession::Own(G3Model *m)
{
    if (m)
        models.emplace_back(m);
    return m;
}

void RaceSession::SyncPieces(const std::vector<dsTrackPiece *> &list)
{
    pieces.clear();
    std::vector<const dsTrackPiece *> drawn; // the track's pieces, each followed by its pillars (Snow)
    for (const dsTrackPiece *p : list) {
        drawn.push_back(p);
        for (const dsTrackPiece *pl : p->pillars)
            if (pl)
                drawn.push_back(pl);
    }
    for (const dsTrackPiece *p : drawn) {
        G3Model *m = PieceModel(p->modelPath);
        if (m)
            pieces.push_back({m, p});
    }
    visible.assign(pieces.size(), 1);
    detail.assign(pieces.size(), 0);
}

G3Model *RaceSession::PieceModel(const std::string &path)
{
    auto it = modelCache.find(path);
    if (it == modelCache.end()) {
        G3Model *m = Own(RaceLoadModel(path));
        if (!m)
            fprintf(stderr, "no model %s\n", path.c_str());
        // The texture SRT animation next to the model (main 0x02055f78: the arrow signs' scrolling lights).
        std::vector<u8> na;
        if (m && batch && RaceReadFile("graphics/" + path + ".na", &na))
            m->LoadTexAnim(na, m->batchMaterials ? *batch : *m);
        it = modelCache.emplace(path, m).first;
        if (m && !m->hasBounds)
            m->ComputeBounds();
        if (m)
            FindStartLights(m);
    }
    return it->second;
}

// A new piece model with start lights (main 0x0206aaf4 makes their controllers when the model loads).
void RaceSession::FindStartLights(G3Model *m)
{
    if (!m->batchMaterials || !batch || !m->overrides.empty())
        return;
    static const char *const lightNames[3] = {"StartRed", "StartAmber", "StartGreen"};
    for (const G3Model::Op &op : m->ops)
        for (int l = 0; l < 3; l++)
            if (op.kind == 1 && op.a < (int)batch->materials.size() && batch->materials[op.a].name == lightNames[l]) {
                m->overrides.resize(batch->materials.size());
                bool known = false;
                for (const StartLight &s : startLights)
                    known |= s.model == m && s.material == op.a;
                if (!known)
                    startLights.push_back({m, op.a, l});
            }
    ApplyStartLights(); // levels 0 (the model's light struct starts cleared): hidden until the countdown
}

void RaceSession::LoadCar(const std::string &envName)
{
    auto own = [this](G3Model *m) { return Own(m); };
    // lod1: the body at the physics scale (lod0 adds the wheel, shadow and light-cone shapes that the game
    // places itself in the visual update 0x02126d10, not ported yet).
    carModel = own(RaceLoadModel("Cars/" + envName + "/" + std::to_string(skin >= 0 && skin < 10 ? skin : 0) + "/lod1"));
    shadowModel = own(RaceLoadModel("Cars/" + envName + "/shadow", false));
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
    G3Model *lw = RaceLoadModel("Cars/" + envName + "/lwheel", false);
    G3Model *rw = RaceLoadModel("Cars/" + envName + "/rwheel", false);
    if (lw && rw)
        rw->BorrowTextures(*lw);
    for (G3Model *w : {lw, rw})
        if (w)
            w->Upload();
    wheelModels[0] = own(lw);
    wheelModels[1] = own(rw);
}

// Sky, sea, ground plane and scenery of the environment and time of day in the race setup.
void RaceSession::LoadEnvironment()
{
    auto own = [this](G3Model *m) { return Own(m); };
    dsRaceSetup *setup = dsRaceSetup::Get();
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
    // The sea: the drawer of ov003 0x0213079c with colour 4 of the race environment and the texture of
    // misc/<long environment name>/<same> (ov003 0x0211de20); the file's own quad model is not drawn.
    std::string seaName = setup->EnvironmentLongName();
    seaModel = own(RaceLoadModel("misc/" + seaName + "/" + seaName, false));
    if (seaModel && !seaModel->textures.empty()) {
        G3Texture &t = seaModel->textures[0]; // itcm 0x01ffddd4: repeat S and T, no flip
        t.repeatS = t.repeatT = true;
        t.flipS = t.flipT = false;
        seaModel->Upload();
        dsTrackLighting tl;
        if (!tl.Load(setup->EnvironmentName(), setup->TimeOfDayName()))
            tl.colors[4] = {0, 0, 0};
        sea.Init(setup->environment, tl.colors[4]);
    } else {
        seaModel = nullptr;
    }
    // The ground (ov003 0x0211de20, collision world vtable +0x10 = 0x0205b4bc, normal (0, 1, 0), every mask): solid
    // ground at y = 0 with the rough material 1 on Stadium and Snow, the sea floor on Island (y = -16) and Coast
    // (y = -19) with material 3. Without it a car off the track fell for ever on Stadium and Snow.
    static const fx32 s_groundY[ENV_COUNT] = {0, (fx32)0xffff0001, 0, (fx32)0xfffed001};
    static const u8 s_groundMaterial[ENV_COUNT] = {1, 3, 1, 3};
    const VecFx32 groundNormal = {0, FX32_ONE, 0};
    groundPlane.reset(new dsCollisionPlaneShape(groundNormal, s_groundY[setup->environment], 0xffffffff,
                                                s_groundMaterial[setup->environment]));
    dsCollisionWorld::Get()->AddShape(groundPlane.get());
    LoadScenery(setup->environment);
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
    detail.assign(pieces.size(), 0);
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
        // A triangle of edges up to E whose vertices are all at least 2.5 E from the eye stays within about 70
        // degrees of the view axis when a part of it is in view: inside the GE's guard band (82 degrees at 480 x
        // 272 with this FOV).
        float d = sqrtf(c[0] * c[0] + c[1] * c[1] + c[2] * c[2]) - r;
        for (int l = G3_DETAIL_LEVELS - 1; l > 0; l--)
            if (d >= 2.5f * G3DetailEdge(l)) {
                detail[k] = (u8)l;
                break;
            }
    }
}

bool RaceSession::SeaDraw(G3QuadVertex *v, u16 *rgb555) const
{
    if (!seaModel)
        return false;
    // The drawer reads the scene camera's height (+0x50 of the camera matrix at +0x28: its position y).
    dsSeaBackdrop::Vertex q[dsSeaBackdrop::QUADS * 4];
    sea.Quads(cams[camIndex].matrix.m[3][1], q);
    for (int k = 0; k < dsSeaBackdrop::QUADS * 4; k++) {
        for (int i = 0; i < 3; i++)
            v[k].pos[i] = q[k].pos[i];
        v[k].s = q[k].s;
        v[k].t = q[k].t;
    }
    *rgb555 = sea.Color();
    return true;
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
    // Sounds of the frame: checkpoints (ov003 0x0210e9fc) and the finish (0x0210ecf0) play CHECKPOINT, a restart or
    // a respawn resets the car audio (0x020f7e6c), the countdown beeps (0x02114ee4: COUNTDOWN_1 above 2 s, above
    // 1 s and above 0, COUNTDOWN_2 at 0), then the player's skids and collisions (0x0210ed9c).
    dsGameAudio *ga = dsGameAudio::Get();
    bool go = false;
    for (; soundEvents < progress->events.size(); soundEvents++) {
        const dsRaceEvent &ev = progress->events[soundEvents];
        if (ev.type == dsRaceEvent::CHECKPOINT || ev.type == dsRaceEvent::FINISH)
            ga->PlaySe("CHECKPOINT");
        else if (ev.type == dsRaceEvent::RESTART || ev.type == dsRaceEvent::RESPAWN)
            dsCarAudio::Get()->RaceReset();
        if (ev.type == dsRaceEvent::RESTART)
            countdownSound = -1;
        if (ev.type == dsRaceEvent::GO)
            go = true;
    }
    if (progress->countdown || go) {
        fx32 c = progress->clock;
        int step = c > 0x2000 ? 0 : c > 0x1000 ? 1 : c > 0 ? 2 : c == 0 ? 3 : countdownSound;
        if (step != countdownSound) {
            ga->PlaySe(step == 3 ? "COUNTDOWN_2" : "COUNTDOWN_1");
            countdownSound = step;
        }
        // The start lights (same function): red above 1 s, amber above 0, green at 0; they stay as set after.
        if (c >= 0) {
            startLevel[0] = c > 0x1000 ? 0xd : 0;
            startLevel[1] = c > 0 && c <= 0x1000 ? 0xd : 0;
            startLevel[2] = c == 0 ? 0xd : 0;
            ApplyStartLights();
        }
    }
    dsCarAudio::Get()->PlayerUpdate(*car);
    cams[camIndex].Update(*car); // camera vtable +0x1c, once per frame
    G3SetTexAnimFrame(++texAnimFrame); // material animations: one frame per 30 Hz frame (port choice)
    // The sea's scroll advances at each of its draws (itcm 0x01fff8cc), once per 30 Hz frame in the race; here
    // once per frame, so that the image does not depend on how often a driver draws.
    sea.Advance();
    // The player's update (0x0210ed9c) sets the lights once per 30 Hz frame from the pad and the engine's gear.
    carLights.Update(in.accel > 0, in.brake > 0, car->engine ? car->engine->gear : 0);
    ApplyCarLights();
    // End of dsRace_Update (0x02113048); the HUD reads the free-wheel flag through ov003 0x0210e9f0.
    hud.Update(*progress, car->speed, car->noThrottle, 2, recordSplits, recordTime);
    hud.SetRetries(mode == 8 ? progress->retries : -1);
    // The run's ghost: recorded from the start (after the countdown) to the finish, two ticks per frame.
    if (progress->countdown) {
        recordStarted = false;
    } else if (!progress->finished) {
        if (!recordStarted) {
            recorder.BeginRecord();
            recordStarted = true;
        }
        recorder.Record(car->matrix);
    }
    if (hud.message != shownMessage) {
        shownMessage = hud.message;
        if (!shownMessage.empty())
            printf("message \"%s\"\n", shownMessage.c_str());
    }
}

void RaceSession::AudioUpdate() { dsGameAudio::Get()->Update(&cams[camIndex].matrix, car.get()); }

void RaceSession::Horn() { dsCarAudio::Get()->Horn(); }

G2CellBank *const *RaceHudBanks()
{
    static G2CellBank *banks[HUD_BANK_COUNT];
    static bool loaded = false;
    if (!loaded) {
        loaded = true;
        for (int b = 0; b < HUD_BANK_COUNT; b++) {
            const char *ncer, *ncgr, *nclr;
            int cells, palette;
            dsHud::BankFiles(b, &ncer, &ncgr, &nclr, &cells, &palette);
            std::vector<u8> e, g, c;
            G2CellBank *bank = new G2CellBank;
            if (RaceReadFile(ncer, &e) && RaceReadFile(ncgr, &g) && RaceReadFile(nclr, &c) &&
                bank->Load(g, e, c, cells, palette)) {
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
    std::vector<const dsHudSprite *> all;
    for (const dsHudSprite &s : hud.sprites)
        all.push_back(&s);
    for (const dsHudSprite &s : hud.retrySprites)
        all.push_back(&s);
    for (const dsHudSprite *sp : all) {
        const dsHudSprite &s = *sp;
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
    // The game's HUD (dsHud: countdown, speed, laps, splits, clock, messages) is drawn as sprites; only the
    // finish display (0x02104670, not ported) keeps a stand-in here.
    if (rp.finished)
        text(w / 2 - 6 * 6 * 8 / 2 * s, h * 0.42f, 6 * s, "ARRIVEE!", 0.3f, 1, 0.3f, user);
}
