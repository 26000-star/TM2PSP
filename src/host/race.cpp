// tm_race: drives the Coast car on a Coast track in an SDL2/OpenGL window.
//   tm_race --root <dir> [--track easy/0] [--frames N --shot out.bmp] [--auto]
// Keys: up = accelerate, down = brake/reverse, left/right = steer, Backspace = back to the last checkpoint,
// Enter = restart the race, Esc = quit.
//
// What runs is the ported game code: track scene, piece placement and terrain (slice 2), collision world (3a), car
// parameters, engine and the full dsCarDynamics step (3b, 3c), and the race progress (dsRaceProgress: countdown,
// checkpoints, laps, finish, respawn), two 1/60 s ticks per 30 Hz frame as on the DS. The drawing is the host G3D
// stand-in (src/shim/host/g3d_gl.cpp) fed with the game's own models and piece matrices; cameras
// (config/cameras) and the HUD are still to port: the camera is a simple chase view and the race state is shown
// in the window title and on stdout.
#include <math.h>
#include <stdio.h>
#include <string.h>

#include <map>
#include <memory>
#include <string>
#include <vector>

#include <SDL.h>
#ifdef _WIN32
#include <windows.h>
#endif
#include <GL/gl.h>

#include "car/dsCarDynamics.h"
#include "car/dsCarParams.h"
#include "collision/dsCollisionWorld.h"
#include "core/dsTime.h"
#include "fs/dsFileSystem.h"
#include "g3d_gl.h"
#include "math/dsMath.h"
#include "nitro.h"
#include "track/dsPieceData.h"
#include "track/dsRaceSetup.h"
#include "track/dsTrack.h"
#include "race/dsRaceProgress.h"

static bool ReadFile(const std::string &path, std::vector<u8> *out)
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

static G3Model *LoadModel(const std::string &name)
{
    std::vector<u8> d;
    if (!ReadFile("graphics/" + name + ".n", &d))
        return nullptr;
    G3Model *m = new G3Model;
    if (!m->Load(d)) {
        delete m;
        return nullptr;
    }
    m->Upload();
    return m;
}

// DS 4x3 matrix (row vectors, fx32) to a GL column-major matrix.
static void GlMatrix(const MtxFx43 &m, float *g)
{
    for (int r = 0; r < 4; r++)
        for (int c = 0; c < 3; c++)
            g[r * 4 + c] = m.m[r][c] / 4096.0f;
    g[3] = g[7] = g[11] = 0;
    g[15] = 1;
}

static void LookAt(const float *eye, const float *at, const float *up)
{
    float f[3] = {at[0] - eye[0], at[1] - eye[1], at[2] - eye[2]};
    float l = sqrtf(f[0] * f[0] + f[1] * f[1] + f[2] * f[2]);
    for (float &v : f)
        v /= l;
    float s[3] = {f[1] * up[2] - f[2] * up[1], f[2] * up[0] - f[0] * up[2], f[0] * up[1] - f[1] * up[0]};
    l = sqrtf(s[0] * s[0] + s[1] * s[1] + s[2] * s[2]);
    for (float &v : s)
        v /= l;
    float u[3] = {s[1] * f[2] - s[2] * f[1], s[2] * f[0] - s[0] * f[2], s[0] * f[1] - s[1] * f[0]};
    float m[16] = {s[0], u[0], -f[0], 0, s[1], u[1], -f[1], 0, s[2], u[2], -f[2], 0, 0, 0, 0, 1};
    glMultMatrixf(m);
    glTranslatef(-eye[0], -eye[1], -eye[2]);
}

static bool SaveBmp(const char *path, int w, int h)
{
    std::vector<u8> px((size_t)w * h * 3);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, w, h, GL_BGR_EXT, GL_UNSIGNED_BYTE, px.data());
    FILE *f = fopen(path, "wb");
    if (!f)
        return false;
    int row = (w * 3 + 3) & ~3, size = 54 + row * h;
    u8 hd[54] = {'B', 'M'};
    auto w32 = [&](int o, u32 v) { hd[o] = (u8)v; hd[o + 1] = (u8)(v >> 8); hd[o + 2] = (u8)(v >> 16); hd[o + 3] = (u8)(v >> 24); };
    w32(2, size);
    w32(10, 54);
    w32(14, 40);
    w32(18, w);
    w32(22, h);
    hd[26] = 1;
    hd[28] = 24;
    w32(34, row * h);
    fwrite(hd, 1, 54, f);
    std::vector<u8> pad(row - w * 3, 0);
    for (int y = 0; y < h; y++) {
        fwrite(&px[(size_t)y * w * 3], 1, w * 3, f);
        fwrite(pad.data(), 1, pad.size(), f);
    }
    fclose(f);
    return true;
}

static bool topView = false;

struct Race {
    dsTrack track;
    dsCarParamsTable table;
    dsCarModelGeometry geo;
    std::unique_ptr<dsCarDynamics> car;
    std::unique_ptr<dsRaceProgress> progress;
    G3Model *batch = nullptr, *carModel = nullptr;
    std::vector<std::pair<G3Model *, const dsTrackPiece *>> pieces;

    bool Load(const char *trackName);
    void Draw(int w, int h);
};

bool Race::Load(const char *trackName)
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

    batch = LoadModel("Tracks/coa/texturebatch");
    std::vector<u8> mb;
    if (batch && ReadFile("graphics/Tracks/coa/material.bin", &mb) && batch->LoadMaterialBin(mb))
        batch->Upload();
    std::map<std::string, G3Model *> cache;
    for (const dsTrackPiece *p : track.pieces) {
        auto it = cache.find(p->modelPath);
        if (it == cache.end())
            it = cache.emplace(p->modelPath, LoadModel(p->modelPath)).first;
        if (it->second)
            pieces.push_back({it->second, p});
    }
    // lod1: the body at the physics scale (lod0 adds the wheel, shadow and light-cone shapes that the game
    // places itself in the visual update 0x02126d10, not ported yet).
    carModel = LoadModel("Cars/coa/0/lod1");
    printf("track %s: %d pieces drawn, batch %s, car model %s\n", trackName, (int)pieces.size(),
           batch ? "ok" : "missing", carModel ? "ok" : "missing");
    return true;
}

void Race::Draw(int w, int h)
{
    glViewport(0, 0, w, h);
    glClearColor(0.55f, 0.75f, 0.95f, 1);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    float n = 0.25f, f = 600, t = n * tanf(30.0f * 3.14159265f / 180), r = t * w / h;
    glFrustum(-r, r, -t, t, n, f);
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();
    const MtxFx43 &m = car->matrix;
    float pos[3] = {m.m[3][0] / 4096.0f, m.m[3][1] / 4096.0f, m.m[3][2] / 4096.0f};
    float fwd[3] = {m.m[2][0] / 4096.0f, 0, m.m[2][2] / 4096.0f};
    float fl = sqrtf(fwd[0] * fwd[0] + fwd[2] * fwd[2]);
    if (fl > 0.01f) {
        fwd[0] /= fl;
        fwd[2] /= fl;
    }
    float eye[3] = {pos[0] - fwd[0] * 7, pos[1] + 2.8f, pos[2] - fwd[2] * 7};
    if (topView) {
        eye[0] = pos[0] - fwd[0] * 40;
        eye[1] = pos[1] + 60;
        eye[2] = pos[2] - fwd[2] * 40;
    }
    float at[3] = {pos[0] + fwd[0] * 2, pos[1] + 0.8f, pos[2] + fwd[2] * 2};
    float up[3] = {0, 1, 0};
    LookAt(eye, at, up);

    glEnable(GL_DEPTH_TEST);
    glEnable(GL_ALPHA_TEST);
    glAlphaFunc(GL_GREATER, 0.02f);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
    // Sea at the Coast sea level (2.5).
    glDisable(GL_TEXTURE_2D);
    glColor3f(0.15f, 0.35f, 0.6f);
    glBegin(GL_QUADS);
    glVertex3f(-500, 2.5f, -500);
    glVertex3f(-500, 2.5f, 800);
    glVertex3f(800, 2.5f, 800);
    glVertex3f(800, 2.5f, -500);
    glEnd();
    float g[16];
    for (auto &pc : pieces) {
        GlMatrix(pc.second->matrix, g);
        glPushMatrix();
        glMultMatrixf(g);
        pc.first->Draw(batch);
        glPopMatrix();
    }
    if (carModel) {
        GlMatrix(car->matrix, g);
        glPushMatrix();
        glMultMatrixf(g);
        carModel->Draw(nullptr);
        glPopMatrix();
    }
}

// Stand-in for the HUD: checkpoints, laps, clock, countdown and messages in the window title.
static void UpdateTitle(SDL_Window *win, const dsRaceProgress &rp)
{
    fx32 t = rp.finished ? rp.finishTime : rp.clock;
    int cs = (int)(((s64)(t & 0xfff) * 100) >> 12), sec = t >> 12;
    char title[200];
    int cps = rp.checkpointCount - (rp.startPiece && rp.startPiece->type == PIECE_MULTILAP ? 1 : 0);
    int passed = 0;
    for (const dsTrigger *tr : rp.Triggers())
        if (tr->kind == dsTrigger::CHECKPOINT && !tr->pending)
            passed++;
    const char *msg = "";
    if (rp.countdown)
        msg = sec >= 2 ? "  [ 3 ]" : (sec >= 1 ? "  [ 2 ]" : "  [ 1 ]");
    else if (rp.finished)
        msg = "  ARRIVEE !";
    else if (!rp.events.empty() && rp.events.back().type == dsRaceEvent::MISSED && rp.clock - rp.events.back().time < 0x3000)
        msg = "  checkpoint(s) manque(s)";
    snprintf(title, sizeof title, "TM2PSP host - Coast | CP %d/%d | tour %d/%d | %d:%02d.%02d%s", passed, cps,
             rp.finished ? rp.lapCount : rp.lapsDone + 1, rp.lapCount, sec / 60, sec % 60, cs, msg);
    static std::string last;
    if (last != title) {
        last = title;
        SDL_SetWindowTitle(win, title);
    }
}

int main(int argc, char **argv)
{
    const char *root = "extract/files", *trackName = "easy/0", *shot = nullptr;
    int frames = -1;
    bool autoDrive = false;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--root") && i + 1 < argc)
            root = argv[++i];
        else if (!strcmp(argv[i], "--track") && i + 1 < argc)
            trackName = argv[++i];
        else if (!strcmp(argv[i], "--frames") && i + 1 < argc)
            frames = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--shot") && i + 1 < argc)
            shot = argv[++i];
        else if (!strcmp(argv[i], "--top"))
            topView = true;
        else if (!strcmp(argv[i], "--auto"))
            autoDrive = true;
        else {
            fprintf(stderr, "usage: tm_race --root <dir> [--track easy/0] [--frames N --shot out.bmp] [--auto]\n");
            return 2;
        }
    }
    FX_Init();
    FSh_SetRoot(root);
    dsFileSystem::Create();
    dsFileSystem::Get()->EnableVfs(true);

    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_TIMER) != 0) {
        fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        return 1;
    }
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    const int W = 768, H = 576;
    SDL_Window *win = SDL_CreateWindow("TM2PSP host - Coast", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, W, H,
                                       SDL_WINDOW_OPENGL | (frames >= 0 ? SDL_WINDOW_HIDDEN : 0));
    if (!win || !SDL_GL_CreateContext(win)) {
        fprintf(stderr, "window: %s\n", SDL_GetError());
        return 1;
    }
    SDL_GL_SetSwapInterval(1);

    Race race;
    if (!race.Load(trackName))
        return 1;
    size_t shownEvents = 0;
    const u32 STEP_MS = 1000 / 30;
    u32 next = SDL_GetTicks();
    bool quit = false;
    for (int frame = 0; !quit && (frames < 0 || frame < frames); frame++) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_QUIT)
                quit = true;
            if (e.type == SDL_KEYDOWN && e.key.keysym.sym == SDLK_ESCAPE)
                quit = true;
            if (e.type == SDL_KEYDOWN && e.key.keysym.sym == SDLK_BACKSPACE)
                race.progress->Reset(false);
            if (e.type == SDL_KEYDOWN && e.key.keysym.sym == SDLK_RETURN)
                race.progress->Reset(true);
        }
        const u8 *k = SDL_GetKeyboardState(nullptr);
        dsCarInput in;
        in.accel = (k[SDL_SCANCODE_UP] || autoDrive) ? FX32_ONE : 0;
        in.brake = k[SDL_SCANCODE_DOWN] ? FX32_ONE : 0;
        in.steer = (k[SDL_SCANCODE_RIGHT] ? FX32_ONE : 0) - (k[SDL_SCANCODE_LEFT] ? FX32_ONE : 0);
        race.progress->Frame(in, 2);
        for (; shownEvents < race.progress->events.size(); shownEvents++) {
            static const char *const names[] = {"restart", "go", "checkpoint", "missed", "lap", "finish", "respawn"};
            const dsRaceEvent &ev = race.progress->events[shownEvents];
            printf("[%d.%02d] %s %d\n", ev.time >> 12, ((ev.time & 0xfff) * 100) >> 12, names[ev.type], ev.value);
        }
        UpdateTitle(win, *race.progress);
        race.Draw(W, H);
        if (frames >= 0 && frame == frames - 1 && shot) {
            glFinish();
            SaveBmp(shot, W, H);
            const MtxFx43 &m = race.car->matrix;
            printf("frame %d pos=%d,%d,%d speed=%d gear=%d\n", frame, m.m[3][0], m.m[3][1], m.m[3][2],
                   race.car->speed, race.car->engine->gear);
        }
        SDL_GL_SwapWindow(win);
        if (frames < 0) {
            next += STEP_MS;
            u32 now = SDL_GetTicks();
            if (next > now)
                SDL_Delay(next - now);
            else
                next = now;
        }
    }
    SDL_Quit();
    return 0;
}
