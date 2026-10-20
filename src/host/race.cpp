// tm_race: drives the Coast car on a Coast track in an SDL2/OpenGL window.
//   tm_race --root <dir> [--track easy/0] [--frames N --shot out.bmp] [--auto [--brake-from F]] [--save file] [--screen menu|pause|result]
// Without --track, the track menu comes first (RaceApp, src/app/race_app.h; best times in tm_race_save.txt next to
// the program, or --save). Menus: arrows, Enter = choose, Esc = back / quit.
// Race keys: up = accelerate, down = brake/reverse, left/right = steer, Backspace = back to the last checkpoint,
// Enter or Esc = pause menu (continue, restart, next track, track menu), C = other chase camera.
//
// What runs is the ported game code: track scene, piece placement and terrain (slice 2), collision world (3a), car
// parameters, engine and the full dsCarDynamics step (3b, 3c), and the race progress (dsRaceProgress: countdown,
// checkpoints, laps, finish, respawn), two 1/60 s ticks per 30 Hz frame as on the DS. The drawing is the host G3D
// stand-in (src/shim/host/g3d_gl.cpp) fed with the game's own models and piece matrices, seen from the game's
// chase camera (config/cameras, dsChaseCamera); the game's HUD is still to port: the race state is shown in the window
// title and on stdout; a bitmap-font HUD stand-in draws the clock, checkpoints, laps and messages.
#include <math.h>
#include <stdio.h>
#include <string.h>

#include <string>
#include <vector>

#include <SDL.h>
#ifdef _WIN32
#include <windows.h>
#endif
#include <GL/gl.h>

#include "fs/dsFileSystem.h"
#include "g3d_gl.h"
#include "nitro.h"
#include "race_app.h"
#include "track/dsPieceData.h"

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

static bool topView = false, noCull = false, unlit = false;
static int drawnPieces = 0;

static void DrawRace(RaceSession &race, int w, int h)
{
    glViewport(0, 0, w, h);
    glClearColor(0.55f, 0.75f, 0.95f, 1);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    // The game camera's FOV (config/cameras, 70) is taken as the full vertical angle.
    const dsChaseCamera &cam = race.cams[race.camIndex];
    float fovy = cam.fov / 4096.0f;
    float n = 0.25f, f = 600, t = n * tanf(fovy * 0.5f * 3.14159265f / 180), r = t * w / h;
    // Sky first, around the eye, with a far plane beyond its radius (1,370 units on Coast), without depth.
    if (race.skyModel && race.lighting && !topView) {
        glFrustum(-r, r, -t, t, n, 4000);
        glMatrixMode(GL_MODELVIEW);
        glLoadIdentity();
        const MtxFx43 &cm = race.cams[race.camIndex].matrix;
        float e[3], a[3], u[3];
        for (int i = 0; i < 3; i++) {
            e[i] = cm.m[3][i] / 4096.0f;
            a[i] = e[i] + cm.m[2][i] / 4096.0f;
            u[i] = cm.m[1][i] / 4096.0f;
        }
        LookAt(e, a, u);
        glTranslatef(e[0], e[1], e[2]);
        glScalef(race.skyScale[0], race.skyScale[1], race.skyScale[2]);
        glDisable(GL_DEPTH_TEST);
        glDepthMask(GL_FALSE);
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
        G3SetDrawLight(nullptr);
        race.skyModel->Draw(nullptr);
        glDepthMask(GL_TRUE);
        glMatrixMode(GL_PROJECTION);
        glLoadIdentity();
    }
    glFrustum(-r, r, -t, t, n, f);
    if (race.lighting && race.lightEnv.fog) {
        u16 fc = race.lightEnv.fogColor;
        float col[4] = {(fc & 31) / 31.0f, ((fc >> 5) & 31) / 31.0f, ((fc >> 10) & 31) / 31.0f, 1};
        glFogi(GL_FOG_MODE, GL_LINEAR);
        glFogf(GL_FOG_START, race.lightEnv.fogStart);
        glFogf(GL_FOG_END, race.lightEnv.fogEnd);
        glFogfv(GL_FOG_COLOR, col);
    }
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();
    const MtxFx43 &cm = cam.matrix;
    float eye[3], at[3], up[3];
    for (int i = 0; i < 3; i++) {
        eye[i] = cm.m[3][i] / 4096.0f;
        at[i] = eye[i] + cm.m[2][i] / 4096.0f;
        up[i] = cm.m[1][i] / 4096.0f;
    }
    if (topView) {
        const MtxFx43 &m = race.car->matrix;
        float pos[3] = {m.m[3][0] / 4096.0f, m.m[3][1] / 4096.0f, m.m[3][2] / 4096.0f};
        float fwd[3] = {m.m[2][0] / 4096.0f, 0, m.m[2][2] / 4096.0f};
        float fl = sqrtf(fwd[0] * fwd[0] + fwd[2] * fwd[2]);
        if (fl > 0.01f) {
            fwd[0] /= fl;
            fwd[2] /= fl;
        }
        float e[3] = {pos[0] - fwd[0] * 40, pos[1] + 60, pos[2] - fwd[2] * 40};
        float u[3] = {0, 1, 0};
        LookAt(e, pos, u);
    } else {
        LookAt(eye, at, up);
    }

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
    race.Cull((float)w / h, n, f);
    drawnPieces = 0;
    for (size_t k = 0; k < race.pieces.size(); k++) {
        auto &pc = race.pieces[k];
        if (!race.visible[k] && !topView && !noCull)
            continue;
        drawnPieces++;
        RaceMatrix(pc.second->matrix, g);
        glPushMatrix();
        glMultMatrixf(g);
        G3DrawLight dl;
        G3SetDrawLight(race.PieceLight(k, &dl));
        pc.first->Draw(race.batch);
        glPopMatrix();
    }
    RaceSession::Draw cd[5];
    for (int i = 0, nd = race.CarDraws(cd); i < nd; i++) {
        glPushMatrix();
        glMultMatrixf(cd[i].matrix);
        G3SetDrawLight(cd[i].lit ? &cd[i].light : nullptr);
        cd[i].model->Draw(nullptr);
        glPopMatrix();
    }
    G3SetDrawLight(nullptr);
    // The car's shadow volume, after the opaque scene (it darkens what lies inside it).
    u16 shadowColor;
    int shadowAlpha;
    if (race.ShadowDraw(g, &shadowColor, &shadowAlpha)) {
        glPushMatrix();
        glMultMatrixf(g);
        race.shadowModel->DrawShadowVolume(shadowColor, shadowAlpha);
        glPopMatrix();
    }
}

// HUD stand-in text: 5 x 7 glyphs as GL quads, with a shadow.
static void DrawText(float x, float y, float px, const char *s, float r, float g, float b, void *)
{
    for (int pass = 0; pass < 2; pass++) { // shadow, then the text
        float o = pass == 0 ? px * 0.5f : 0;
        if (pass == 0)
            glColor3f(0, 0, 0);
        else
            glColor3f(r, g, b);
        glBegin(GL_QUADS);
        float cx = x;
        for (const char *p = s; *p; p++, cx += 6 * px) {
            const char *const *rows = RaceGlyph(*p);
            if (!rows)
                continue;
            for (int row = 0; row < 7; row++)
                for (int col = 0; col < 5; col++)
                    if (rows[row][col] == '1') {
                        float x0 = cx + col * px + o, y0 = y + row * px + o;
                        glVertex2f(x0, y0);
                        glVertex2f(x0 + px, y0);
                        glVertex2f(x0 + px, y0 + px);
                        glVertex2f(x0, y0 + px);
                    }
        }
        glEnd();
    }
}

static void DrawRect(float x, float y, float w, float h, float r, float g, float b, float a, void *)
{
    glColor4f(r, g, b, a);
    glBegin(GL_QUADS);
    glVertex2f(x, y);
    glVertex2f(x + w, y);
    glVertex2f(x + w, y + h);
    glVertex2f(x, y + h);
    glEnd();
}

// HUD sprite: one cell of a bank's atlas (uploaded on first use, kept for the program's life) as a textured quad.
static void DrawSprite(const G2CellBank &bank, const G2Cell &c, float x, float y, float w, float h, void *)
{
    G2CellBank &b = const_cast<G2CellBank &>(bank);
    if (!b.gl) {
        std::vector<u8> rgba((size_t)b.width * b.height * 4);
        for (size_t i = 0; i < b.pixels.size(); i++) {
            u16 p = b.pixels[i];
            rgba[i * 4] = (u8)((p & 31) * 255 / 31);
            rgba[i * 4 + 1] = (u8)((p >> 5 & 31) * 255 / 31);
            rgba[i * 4 + 2] = (u8)((p >> 10 & 31) * 255 / 31);
            rgba[i * 4 + 3] = p & 0x8000 ? 255 : 0;
        }
        glGenTextures(1, &b.gl);
        glBindTexture(GL_TEXTURE_2D, b.gl);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, b.width, b.height, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba.data());
    }
    glEnable(GL_TEXTURE_2D);
    glBindTexture(GL_TEXTURE_2D, b.gl);
    glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_REPLACE);
    float u0 = (float)c.u / b.width, v0 = (float)c.v / b.height;
    float u1 = (float)(c.u + c.w) / b.width, v1 = (float)(c.v + c.h) / b.height;
    glBegin(GL_QUADS);
    glTexCoord2f(u0, v0);
    glVertex2f(x, y);
    glTexCoord2f(u1, v0);
    glVertex2f(x + w, y);
    glTexCoord2f(u1, v1);
    glVertex2f(x + w, y + h);
    glTexCoord2f(u0, v1);
    glVertex2f(x, y + h);
    glEnd();
    glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
    glDisable(GL_TEXTURE_2D);
}

static void DrawHud(int w, int h, const RaceApp &app)
{
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    glOrtho(0, w, h, 0, -1, 1);
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_TEXTURE_2D);
    glDisable(GL_CULL_FACE);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    app.Overlay(w, h, 1, DrawText, DrawRect, DrawSprite, nullptr);
    glEnable(GL_DEPTH_TEST);
}

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
    const char *root = "extract/files", *trackName = nullptr, *shot = nullptr, *save = nullptr,
               *screen = nullptr;
    int frames = -1, finishAt = -1, brakeFrom = -1;
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
        else if (!strcmp(argv[i], "--no-cull")) // draw every piece (culling check)
            noCull = true;
        else if (!strcmp(argv[i], "--unlit")) // material colours, no DS lighting, fog or sky (as before phase 5)
            unlit = true;
        else if (!strcmp(argv[i], "--top"))
            topView = true;
        else if (!strcmp(argv[i], "--finish-at") && i + 1 < argc) // save-file test: a finish at that frame
            finishAt = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--screen") && i + 1 < argc)
            screen = argv[++i];
        else if (!strcmp(argv[i], "--save") && i + 1 < argc)
            save = argv[++i];
        else if (!strcmp(argv[i], "--auto"))
            autoDrive = true;
        else if (!strcmp(argv[i], "--brake-from") && i + 1 < argc) // auto drive: brake instead from that frame
            brakeFrom = atoi(argv[++i]);
        else {
            fprintf(stderr, "usage: tm_race --root <dir> [--track easy/0] [--frames N --shot out.bmp] [--auto [--brake-from F]] [--save file] [--screen menu|pause|result]\n");
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
    SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 8); // the car's shadow volume
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    const int W = 768, H = 576;
    SDL_Window *win = SDL_CreateWindow("TM2PSP host - Coast", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, W, H,
                                       SDL_WINDOW_OPENGL | (frames >= 0 ? SDL_WINDOW_HIDDEN : 0));
    if (!win || !SDL_GL_CreateContext(win)) {
        fprintf(stderr, "window: %s\n", SDL_GetError());
        return 1;
    }
    SDL_GL_SetSwapInterval(1);

    // Save file next to the program; none for the scripted runs (--frames), which also skip the menus.
    std::string savePath;
    if (save) {
        savePath = save;
    } else if (frames < 0) {
        if (char *base = SDL_GetBasePath()) {
            savePath = std::string(base) + "tm_race_save.txt";
            SDL_free(base);
        }
    }
    RaceApp app(savePath, "ENTREE", "ECHAP");
    app.menus = frames < 0;
    app.lighting = !unlit;
    bool menuShot = screen && !strcmp(screen, "menu"); // --frames N --screen menu: the track list
    if (trackName || (frames >= 0 && !menuShot)) {
        int index = app.FindTrack(trackName ? trackName : "easy/0");
        if (index < 0) {
            fprintf(stderr, "unknown track %s\n", trackName);
            return 1;
        }
        if (!app.Start(index))
            return 1;
    }
    const u32 STEP_MS = 1000 / 30;
    u32 next = SDL_GetTicks();
    bool quit = false;
    for (int frame = 0; !quit && (frames < 0 || frame < frames); frame++) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_QUIT)
                quit = true;
            if (e.type != SDL_KEYDOWN)
                continue;
            bool playing = app.screen == RaceApp::PLAYING;
            switch (e.key.keysym.sym) {
            case SDLK_ESCAPE:
                quit |= !app.Press(RaceApp::BACK);
                break;
            case SDLK_RETURN:
                app.Press(playing ? RaceApp::PAUSE : RaceApp::OK);
                break;
            case SDLK_UP:
            case SDLK_DOWN:
            case SDLK_LEFT:
            case SDLK_RIGHT:
                if (!playing)
                    app.Press(e.key.keysym.sym == SDLK_UP     ? RaceApp::UP
                              : e.key.keysym.sym == SDLK_DOWN ? RaceApp::DOWN
                              : e.key.keysym.sym == SDLK_LEFT ? RaceApp::LEFT
                                                              : RaceApp::RIGHT);
                break;
            case SDLK_BACKSPACE:
                if (playing)
                    app.race->Respawn();
                break;
            case SDLK_c:
                if (playing)
                    app.race->ToggleCamera();
                break;
            default:
                break;
            }
        }
        const u8 *k = SDL_GetKeyboardState(nullptr);
        dsCarInput in;
        bool autoBrake = autoDrive && brakeFrom >= 0 && frame >= brakeFrom;
        in.accel = (k[SDL_SCANCODE_UP] || (autoDrive && !autoBrake)) ? FX32_ONE : 0;
        in.brake = (k[SDL_SCANCODE_DOWN] || autoBrake) ? FX32_ONE : 0;
        // The game's steer is positive to the left (seen on a real PSP-3000, same code as here).
        in.steer = (k[SDL_SCANCODE_LEFT] ? FX32_ONE : 0) - (k[SDL_SCANCODE_RIGHT] ? FX32_ONE : 0);
        // --screen pause|result: that menu is opened for the last scripted frame (layout check only).
        if (frames >= 0 && frame == frames - 1 && app.race && screen && !menuShot)
            app.screen = !strcmp(screen, "result") ? RaceApp::RESULT : RaceApp::PAUSED;
        if (frame == finishAt && app.race) {
            app.race->progress->finished = true;
            app.race->progress->finishTime = app.race->progress->clock;
        }
        app.Frame(in);
        if (app.race) {
            UpdateTitle(win, *app.race->progress);
            DrawRace(*app.race, W, H);
        } else {
            SDL_SetWindowTitle(win, "TM2PSP host - Coast");
            glViewport(0, 0, W, H);
            glClearColor(0.05f, 0.1f, 0.25f, 1);
            glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        }
        DrawHud(W, H, app);
        if (frames >= 0 && frame == frames - 1 && shot && !app.race) {
            glFinish();
            SaveBmp(shot, W, H);
        } else if (frames >= 0 && frame == frames - 1 && shot) {
            RaceSession &race = *app.race;
            glFinish();
            SaveBmp(shot, W, H);
            const MtxFx43 &m = race.car->matrix;
            printf("pieces drawn %d of %d\n",drawnPieces, (int)race.pieces.size());
            printf("frame %d pos=%d,%d,%d speed=%d gear=%d\n", frame, m.m[3][0], m.m[3][1], m.m[3][2],
                   race.car->speed, race.car->engine->gear);
            const MtxFx43 &c = race.cams[race.camIndex].matrix;
            printf("camera pos=%d,%d,%d fwd=%d,%d,%d up=%d,%d,%d\n", c.m[3][0], c.m[3][1], c.m[3][2], c.m[2][0], c.m[2][1],
                   c.m[2][2], c.m[1][0], c.m[1][1], c.m[1][2]);
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
    app.Unload();
    SDL_Quit();
    return 0;
}
