// tm_race for the PSP: the track menu (RaceApp, src/app/race_app.h) and the Coast race (RaceSession) drawn with
// sceGu at 480 x 272, driven with sceCtrl. Data next to the EBOOT: files/ = extract/files; save.txt = best times and
// the last track (written by the program); track.txt ("easy/0") places the menu cursor when there is no save yet.
// Menus: d-pad, X = choose, O = back (quits from the track list), START = pause.
// Race: d-pad or analog = steer, X = accelerate, square = brake/reverse, triangle = back to the last checkpoint,
// START = pause menu (continue, restart, next track, track menu), select = other chase camera, L = horn,
// R = timing overlay (race update / sound rendering / GE list build / GE wait, ms).
// Track editor (src/app/editor.cpp): d-pad = cursor, L / R = height, analog = camera, X = place / take a block,
// square = rotate, O = delete, triangle = block menu, START = editor menu, select = help.
// Sound: the game's sound code and the DS sound driver, rendered after each frame (SndStream) and played by an audio
// thread on a 32 kHz SRC channel (sceAudio); an empty mute.txt next to the EBOOT turns it off.
// Auto test: when auto.txt holds "<track> <frames>", the race starts without the menu, the car accelerates on its
// own, the frame <frames> is saved to shot.bmp, race_log.txt gets the same "frame/camera" lines as
// `tm_race --frames N --auto` on the host plus timings, memory and the hash of the rendered sound (kRate / 30 samples
// per frame, as `tm_race --wav`; also written to race.wav when wav.txt exists), and the program exits
// (tools/psp/race_shot.ps1).
// Editor captures: auto.txt "editor:<sta|isl|sno|coa|puz<n>> <frames> <pad presses>" (RaceApp::EditorScript).
// Frame-time log: an (empty) perf.txt makes the program append per-second timings to frametimes.txt.
// Reload test: reload.txt "<count>" loads the tracks in menu order, 20 frames each, and logs the heap after each
// unload (tools/psp/reload_test.ps1).
#include <malloc.h>
#include <stdlib.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#include <string>
#include <vector>

#include <pspaudio.h>
#include <pspctrl.h>
#include <pspdmac.h>
#include <pspdisplay.h>
#include <pspge.h>
#include <pspgu.h>
#include <pspgum.h>
#include <pspkernel.h>
#include <psppower.h>
#include <psputility.h>

#include "fs/dsFileSystem.h"
#include "nitro.h"
#include "race_app.h"
#include "snd_driver.h"
#include "snd_stream.h"
#include "text/dsStringTable.h"
#include "track/dsRaceSetup.h"

PSP_MODULE_INFO("TM2PSP", PSP_MODULE_USER, 1, 0);
PSP_MAIN_THREAD_ATTR(PSP_THREAD_ATTR_USER | PSP_THREAD_ATTR_VFPU);
PSP_MAIN_THREAD_STACK_SIZE_KB(512);
PSP_HEAP_SIZE_KB(-1024);

size_t G3GuTextureBytes();
size_t G3GuMeshBytes();
size_t G3GuVramBytes();
int G3GuClampedVertices();
int G3GuTakeDrawnVertices();
int G3GuQuadVertices();
void G3GuSetVram(void *base, size_t size);
void G3GuSetDebug(unsigned flags);

namespace {

const int BUF_W = 512, SCR_W = 480, SCR_H = 272;
unsigned int __attribute__((aligned(16))) g_list[512 * 1024 / 4];
bool g_quit = false;
int g_drawnPieces = 0;
// view.txt "ex ey ez ax ay az [detail]" next to the EBOOT: a fixed eye and look-at point (units) instead of the race
// camera, every piece drawn at that detail level (default 0, the finest; inspection, as tm_race --view).
bool g_freeView = false;
float g_viewEye[3], g_viewAt[3];
int g_viewDetail = 0;

int ExitCallback(int, int, void *)
{
    g_quit = true;
    return 0;
}

int CallbackThread(SceSize, void *)
{
    int cb = sceKernelCreateCallback("exit", ExitCallback, nullptr);
    sceKernelRegisterExitCallback(cb);
    sceKernelSleepThreadCB();
    return 0;
}

u64 Us() { return sceKernelGetSystemTimeWide(); }

// The audio thread: 512-frame blocks (16 ms) from SndStream to a 32 kHz stereo SRC channel; silence while the ring
// is empty. Higher priority than the main thread, which renders the sound after each game frame.
volatile bool g_audioQuit = false;
const int AUDIO_BLOCK = 512;

int AudioThread(SceSize, void *)
{
    static s16 __attribute__((aligned(64))) block[2][AUDIO_BLOCK * 2];
    if (sceAudioSRCChReserve(AUDIO_BLOCK, SndDriver::kRate, 2) < 0)
        return 1;
    for (int i = 0; !g_audioQuit; i ^= 1) {
        SndStream::Consume(block[i], AUDIO_BLOCK);
        sceAudioSRCOutputBlocking(PSP_AUDIO_VOLUME_MAX, block[i]);
    }
    while (sceAudioOutput2GetRestSample() > 0)
        sceKernelDelayThread(1000);
    sceAudioSRCChRelease();
    return 0;
}

void WriteWav(const std::string &path, const std::vector<s16> &v)
{
    FILE *f = fopen(path.c_str(), "wb");
    if (!f)
        return;
    u8 h[44] = {'R', 'I', 'F', 'F', 0, 0, 0, 0, 'W', 'A', 'V', 'E', 'f', 'm', 't', ' ', 16, 0, 0, 0, 1, 0, 2, 0};
    auto w32 = [&](int o, u32 x) { h[o] = (u8)x; h[o + 1] = (u8)(x >> 8); h[o + 2] = (u8)(x >> 16); h[o + 3] = (u8)(x >> 24); };
    u32 bytes = (u32)(v.size() * 2);
    w32(4, 36 + bytes);
    w32(24, SndDriver::kRate);
    w32(28, SndDriver::kRate * 4);
    h[32] = 4;
    h[34] = 16;
    memcpy(h + 36, "data", 4);
    w32(40, bytes);
    fwrite(h, 1, 44, f);
    fwrite(v.data(), 2, v.size(), f);
    fclose(f);
}

bool ReadText(const std::string &path, char *buf, int n)
{
    FILE *f = fopen(path.c_str(), "rb");
    if (!f)
        return false;
    int got = (int)fread(buf, 1, n - 1, f);
    fclose(f);
    buf[got > 0 ? got : 0] = 0;
    return true;
}

// The frame is copied out of VRAM with the DMA controller first: PPSSPP renders on the host GPU and writes the
// frame back to emulated VRAM when it sees a DMA (or GE) copy from it, not on plain CPU reads.
void SaveBmp(const char *path, const void *frame)
{
    static u32 __attribute__((aligned(64))) copy[BUF_W * SCR_H];
    sceKernelDcacheWritebackInvalidateAll();
    sceDmacMemcpy(copy, frame, sizeof copy);
    sceKernelDcacheWritebackInvalidateAll();
    const u32 *vram = copy;
    FILE *f = fopen(path, "wb");
    if (!f)
        return;
    u8 hd[54] = {'B', 'M'};
    auto w32 = [&](int o, u32 v) { hd[o] = (u8)v; hd[o + 1] = (u8)(v >> 8); hd[o + 2] = (u8)(v >> 16); hd[o + 3] = (u8)(v >> 24); };
    w32(2, 54 + SCR_W * SCR_H * 3);
    w32(10, 54);
    w32(14, 40);
    w32(18, SCR_W);
    w32(22, SCR_H);
    hd[26] = 1;
    hd[28] = 24;
    fwrite(hd, 1, 54, f);
    static u8 row[SCR_W * 3];
    for (int y = SCR_H - 1; y >= 0; y--) {
        for (int x = 0; x < SCR_W; x++) {
            u32 c = vram[y * BUF_W + x];
            row[x * 3] = (u8)(c >> 16);
            row[x * 3 + 1] = (u8)(c >> 8);
            row[x * 3 + 2] = (u8)c;
        }
        fwrite(row, 1, sizeof row, f);
    }
    fclose(f);
}

struct SpriteVertex {
    u32 color;
    s16 x, y, z;
};

// HUD text: every glyph pixel is a GU sprite (two vertices), with a shadow pass.
void DrawText(float x, float y, float px, const char *s, float r, float g, float b, void *)
{
    int n = 0;
    for (const char *p = s; *p; p++)
        if (const char *const *rows = RaceGlyph(*p))
            for (int k = 0; k < 7; k++)
                for (int c = 0; c < 5; c++)
                    n += rows[k][c] == '1';
    if (!n)
        return;
    SpriteVertex *v = (SpriteVertex *)sceGuGetMemory(sizeof(SpriteVertex) * 4 * n);
    SpriteVertex *o = v;
    u32 col = 0xff000000 | (u32)(b * 255) << 16 | (u32)(g * 255) << 8 | (u32)(r * 255);
    for (int pass = 0; pass < 2; pass++) {
        float sh = pass == 0 ? px * 0.5f : 0;
        u32 c = pass == 0 ? 0xff000000 : col;
        float cx = x;
        for (const char *p = s; *p; p++, cx += 6 * px) {
            const char *const *rows = RaceGlyph(*p);
            if (!rows)
                continue;
            for (int k = 0; k < 7; k++)
                for (int cc = 0; cc < 5; cc++)
                    if (rows[k][cc] == '1') {
                        float x0 = cx + cc * px + sh, y0 = y + k * px + sh;
                        o[0] = {c, (s16)lrintf(x0), (s16)lrintf(y0), 0};
                        o[1] = {c, (s16)lrintf(x0 + px), (s16)lrintf(y0 + px), 0};
                        if (o[1].x == o[0].x)
                            o[1].x++;
                        if (o[1].y == o[0].y)
                            o[1].y++;
                        o += 2;
                    }
        }
    }
    sceGuDrawArray(GU_SPRITES, GU_COLOR_8888 | GU_VERTEX_16BIT | GU_TRANSFORM_2D, (int)(o - v), nullptr, v);
}

void DrawRace(RaceSession &race)
{
    sceGuClearColor(0xff000000 | (u32)(0.95f * 255) << 16 | (u32)(0.75f * 255) << 8 | (u32)(0.55f * 255));
    sceGuClearDepth(0);
    sceGuClear(GU_COLOR_BUFFER_BIT | GU_DEPTH_BUFFER_BIT);
    const dsChaseCamera &cam = race.cams[race.camIndex];
    sceGumMatrixMode(GU_PROJECTION);
    sceGumLoadIdentity();
    // FOV (config/cameras, 70) as the full vertical angle, as on the host; near plane 0.5 (host 0.25) for the
    // 16-bit depth buffer.
    // Camera-relative: the GE computes in float24 (16 significant bits) and folds model x view itself, so with world
    // translations of hundreds of units the vertices moved by a few hundredths of a unit from frame to frame
    // (z-fighting and objects showing through each other on the console). The view has no translation and the eye is
    // taken off every model matrix (and the sea's vertices) here, in float32.
    const MtxFx43 &cm = cam.matrix;
    ScePspFVector3 eye, at, up, origin = {0, 0, 0};
    eye = {cm.m[3][0] / 4096.0f, cm.m[3][1] / 4096.0f, cm.m[3][2] / 4096.0f};
    at = {cm.m[2][0] / 4096.0f, cm.m[2][1] / 4096.0f, cm.m[2][2] / 4096.0f};
    up = {cm.m[1][0] / 4096.0f, cm.m[1][1] / 4096.0f, cm.m[1][2] / 4096.0f};
    if (g_freeView) {
        eye = {g_viewEye[0], g_viewEye[1], g_viewEye[2]};
        at = {g_viewAt[0] - eye.x, g_viewAt[1] - eye.y, g_viewAt[2] - eye.z};
        up = {0, 1, 0};
    }
    auto rebase = [&](float *m) {
        m[12] -= eye.x;
        m[13] -= eye.y;
        m[14] -= eye.z;
    };
    // Sky first, around the eye, with a far plane beyond its radius (1,370 units on Coast), without depth.
    if (race.skyModel && race.lighting) {
        sceGumPerspective(cam.fov / 4096.0f, (float)SCR_W / SCR_H, 0.5f, 4000.0f);
        sceGumMatrixMode(GU_VIEW);
        sceGumLoadIdentity();
        sceGumLookAt(&origin, &at, &up);
        sceGumMatrixMode(GU_MODEL);
        sceGumLoadIdentity();
        ScePspFVector3 sc = {race.skyScale[0], race.skyScale[1], race.skyScale[2]};
        sceGumScale(&sc);
        sceGuDisable(GU_DEPTH_TEST);
        sceGuDepthMask(GU_TRUE);
        G3SetDrawLight(nullptr);
        race.skyModel->Draw(nullptr);
        sceGuDepthMask(GU_FALSE);
        sceGuEnable(GU_DEPTH_TEST);
        sceGumMatrixMode(GU_PROJECTION);
        sceGumLoadIdentity();
    }
    sceGumPerspective(cam.fov / 4096.0f, (float)SCR_W / SCR_H, 0.5f, 600.0f);
    sceGumMatrixMode(GU_VIEW);
    sceGumLoadIdentity();
    sceGumLookAt(&origin, &at, &up);
    sceGumMatrixMode(GU_MODEL);
    sceGumLoadIdentity();
    if (race.lighting && race.lightEnv.fog) {
        u16 fc = race.lightEnv.fogColor;
        u32 col = 0xff000000 | ((fc >> 10) & 31) * 255 / 31 << 16 | ((fc >> 5) & 31) * 255 / 31 << 8 |
                  (fc & 31) * 255 / 31;
        sceGuFog(race.lightEnv.fogStart, race.lightEnv.fogEnd, col);
    }
    // The scenery around the track (RaceSession::scenery) with the sky's far plane, depth tested among themselves,
    // then the depth is cleared for the track: the scenery lies outside the track's cells.
    if (!race.scenery.empty()) {
        sceGumMatrixMode(GU_PROJECTION);
        sceGumPushMatrix();
        sceGumLoadIdentity();
        sceGumPerspective(cam.fov / 4096.0f, (float)SCR_W / SCR_H, 0.5f, 4000.0f);
        sceGumMatrixMode(GU_MODEL);
        float sg[16];
        for (int pass = 0; pass < 2; pass++)
            for (size_t i = 0; i < race.scenery.size(); i++) {
                RaceMatrix(race.scenery[i].matrix, sg);
                rebase(sg);
                sceGumPushMatrix();
                sceGumMultMatrix((const ScePspFMatrix4 *)sg);
                G3DrawLight dl;
                G3SetDrawLight(race.SceneryLight(i, &dl));
                race.scenery[i].model->Draw(race.batch, pass);
                sceGumPopMatrix();
            }
        G3SetDrawLight(nullptr);
        sceGuClear(GU_DEPTH_BUFFER_BIT);
        sceGumMatrixMode(GU_PROJECTION);
        sceGumPopMatrix();
        sceGumMatrixMode(GU_MODEL);
    }
    float g[16];
    race.Cull((float)SCR_W / SCR_H, 0.5f, 600.0f);
    // As the DS: every opaque polygon of the frame, then the translucent ones (foliage, nets, glass, the sea), so
    // that a piece drawn later cannot cover the translucent parts of a nearer one.
    g_drawnPieces = 0;
    auto drawPieces = [&](int pass) {
        float pg[16];
        for (size_t k = 0; k < race.pieces.size(); k++) {
            auto &pc = race.pieces[k];
            if (!race.visible[k] && !g_freeView)
                continue;
            g_drawnPieces += pass == 0;
            RaceMatrix(pc.second->matrix, pg);
            rebase(pg);
            sceGumPushMatrix();
            sceGumMultMatrix((const ScePspFMatrix4 *)pg);
            G3DrawLight dl;
            G3SetDrawLight(race.PieceLight(k, &dl));
            G3SetDrawDetail(g_freeView ? g_viewDetail : race.detail[k]);
            pc.first->Draw(race.batch, pass);
            sceGumPopMatrix();
        }
        G3SetDrawDetail(0);
        G3SetDrawLight(nullptr);
    };
    RaceSession::Draw cd[RaceSession::MAX_CAR_DRAWS];
    int nd = race.CarDraws(cd);
    for (int i = 0; i < nd; i++)
        rebase(cd[i].matrix);
    auto drawCars = [&](int pass) {
        for (int i = 0; i < nd; i++) {
            sceGumPushMatrix();
            sceGumMultMatrix((const ScePspFMatrix4 *)cd[i].matrix);
            G3SetDrawLight(cd[i].lit ? &cd[i].light : nullptr);
            if (cd[i].ghost)
                race.GhostLook(cd[i], true);
            cd[i].model->Draw(cd[i].model->batchMaterials ? race.batch : nullptr, pass);
            if (cd[i].ghost)
                race.GhostLook(cd[i], false);
            sceGumPopMatrix();
        }
        G3SetDrawLight(nullptr);
    };
    drawPieces(0);
    // The car's shadow volume, after the track and before the car: the game gives the car models polygon ID 2
    // (main 0x02055970), the shadow's, so the DS draws the shadow on the track only.
    u16 shadowColor;
    int shadowAlpha;
    if (race.ShadowDraw(g, &shadowColor, &shadowAlpha)) {
        rebase(g);
        sceGumPushMatrix();
        sceGumMultMatrix((const ScePspFMatrix4 *)g);
        race.shadowModel->DrawShadowVolume(shadowColor, shadowAlpha);
        sceGumPopMatrix();
    }
    drawCars(0);
    // The editor's lines (building area, cursor column), over the opaque pieces; camera-relative as the rest.
    if (!race.lines.empty()) {
        struct LineVertex {
            u32 color;
            float x, y, z;
        };
        LineVertex *lv = (LineVertex *)sceGuGetMemory(sizeof(LineVertex) * 2 * race.lines.size());
        for (size_t i = 0; i < race.lines.size(); i++) {
            const RaceSession::Line &l = race.lines[i];
            lv[2 * i] = {l.rgba, l.a[0] - eye.x, l.a[1] - eye.y, l.a[2] - eye.z};
            lv[2 * i + 1] = {l.rgba, l.b[0] - eye.x, l.b[1] - eye.y, l.b[2] - eye.z};
        }
        sceGuDisable(GU_TEXTURE_2D);
        sceGuDisable(GU_FOG);
        sceGumLoadIdentity();
        sceGumDrawArray(GU_LINES, GU_COLOR_8888 | GU_VERTEX_32BITF | GU_TRANSFORM_3D, 2 * (int)race.lines.size(),
                        nullptr, lv);
    }
    drawPieces(1);
    // The sea (itcm 0x01fff8cc): translucent, over the track, under the car's translucent parts; cut around the eye
    // for the guard band.
    G3QuadVertex sea[dsSeaBackdrop::QUADS * 4];
    u16 seaColor;
    if (race.SeaDraw(sea, &seaColor)) {
        for (G3QuadVertex &q : sea) {
            q.pos[0] -= eye.x;
            q.pos[1] -= eye.y;
            q.pos[2] -= eye.z;
        }
        const float e[3] = {0, 0, 0};
        race.seaModel->DrawQuads(0, sea, dsSeaBackdrop::QUADS, seaColor, race.lighting && race.lightEnv.fog, e);
    }
    drawCars(1);
}

// Flat rectangle (menu panels), alpha blended.
void DrawRect(float x, float y, float w, float h, float r, float g, float b, float a, void *)
{
    SpriteVertex *v = (SpriteVertex *)sceGuGetMemory(sizeof(SpriteVertex) * 2);
    u32 c = (u32)(a * 255) << 24 | (u32)(b * 255) << 16 | (u32)(g * 255) << 8 | (u32)(r * 255);
    v[0] = {c, (s16)lrintf(x), (s16)lrintf(y), 0};
    v[1] = {c, (s16)lrintf(x + w), (s16)lrintf(y + h), 0};
    sceGuDrawArray(GU_SPRITES, GU_COLOR_8888 | GU_VERTEX_16BIT | GU_TRANSFORM_2D, 2, nullptr, v);
}

struct TexSpriteVertex {
    float u, v;
    float x, y, z;
};

// HUD sprite: one cell of a bank's 5551 atlas (moved once to an aligned buffer the GE reads, kept for the program's
// life), scaled with bilinear filtering (h / 192 = 1.42 on the PSP).
bool UploadBank(G2CellBank &b)
{
    if (!b.gu) {
        size_t n = b.pixels.size() * sizeof(u16);
        void *p = memalign(16, n);
        if (!p)
            return false;
        memcpy(p, b.pixels.data(), n);
        sceKernelDcacheWritebackRange(p, n);
        b.gu = p;
        std::vector<u16>().swap(b.pixels); // the GE's copy is the only one needed
    }
    return true;
}

void DrawSprite(const G2CellBank &bank, const G2Cell &c, float x, float y, float w, float h, void *)
{
    G2CellBank &b = const_cast<G2CellBank &>(bank);
    if (!UploadBank(b))
        return;
    sceGuEnable(GU_TEXTURE_2D);
    sceGuTexMode(GU_PSM_5551, 0, 0, GU_FALSE);
    sceGuTexImage(0, b.width, b.height, b.width, b.gu);
    sceGuTexFunc(GU_TFX_REPLACE, GU_TCC_RGBA);
    sceGuTexFilter(GU_LINEAR, GU_LINEAR);
    sceGuTexWrap(GU_CLAMP, GU_CLAMP);
    TexSpriteVertex *v = (TexSpriteVertex *)sceGuGetMemory(sizeof(TexSpriteVertex) * 2);
    v[0] = {(float)c.u, (float)c.v, x, y, 0};
    v[1] = {(float)(c.u + c.w), (float)(c.v + c.h), x + w, y + h, 0};
    sceGuDrawArray(GU_SPRITES, GU_TEXTURE_32BITF | GU_VERTEX_32BITF | GU_TRANSFORM_2D, 2, nullptr, v);
    sceGuTexFilter(GU_NEAREST, GU_NEAREST);
    sceGuTexFunc(GU_TFX_MODULATE, GU_TCC_RGBA);
    sceGuDisable(GU_TEXTURE_2D);
}

struct ColorTexVertex {
    float u, v;
    u32 color;
    float x, y, z;
};

// Front end picture (FeImage, in RAM: 5650 / 4444 / 8888 as converted by tools/fe/convert.py), modulated by the
// colour. The pixels are written back from the data cache once, on first use.
void DrawFeImage(const FeImage &img, float x, float y, float w, float h, float u0, float v0, float u1, float v1,
                 u32 c, void *)
{
    FeImage &im = const_cast<FeImage &>(img);
    if (!im.gu) {
        sceKernelDcacheWritebackRange(im.pixels, im.Bytes());
        im.gu = im.pixels;
    }
    sceGuEnable(GU_TEXTURE_2D);
    sceGuTexMode(im.format, 0, 0, GU_FALSE);
    sceGuTexImage(0, im.stride, im.texH, im.stride, im.pixels);
    sceGuTexFunc(GU_TFX_MODULATE, GU_TCC_RGBA);
    sceGuTexFilter(GU_LINEAR, GU_LINEAR);
    sceGuTexWrap(GU_CLAMP, GU_CLAMP);
    ColorTexVertex *v = (ColorTexVertex *)sceGuGetMemory(sizeof(ColorTexVertex) * 2);
    v[0] = {u0, v0, c, x, y, 0};
    v[1] = {u1, v1, c, x + w, y + h, 0};
    sceGuDrawArray(GU_SPRITES, GU_TEXTURE_32BITF | GU_COLOR_8888 | GU_VERTEX_32BITF | GU_TRANSFORM_2D, 2, nullptr, v);
    sceGuTexFilter(GU_NEAREST, GU_NEAREST);
    sceGuDisable(GU_TEXTURE_2D);
}

// Front end cell (DS font glyph or icon) of a 5551 bank atlas, modulated by the colour.
void DrawFeCell(const G2CellBank &bank, const G2Cell &cl, float x, float y, float w, float h, u32 c, void *)
{
    G2CellBank &b = const_cast<G2CellBank &>(bank);
    if (!UploadBank(b))
        return;
    sceGuEnable(GU_TEXTURE_2D);
    sceGuTexMode(GU_PSM_5551, 0, 0, GU_FALSE);
    sceGuTexImage(0, b.width, b.height, b.width, b.gu);
    sceGuTexFunc(GU_TFX_MODULATE, GU_TCC_RGBA);
    sceGuTexFilter(GU_LINEAR, GU_LINEAR);
    sceGuTexWrap(GU_CLAMP, GU_CLAMP);
    ColorTexVertex *v = (ColorTexVertex *)sceGuGetMemory(sizeof(ColorTexVertex) * 2);
    v[0] = {(float)cl.u, (float)cl.v, c, x, y, 0};
    v[1] = {(float)(cl.u + cl.w), (float)(cl.v + cl.h), c, x + w, y + h, 0};
    sceGuDrawArray(GU_SPRITES, GU_TEXTURE_32BITF | GU_COLOR_8888 | GU_VERTEX_32BITF | GU_TRANSFORM_2D, 2, nullptr, v);
    sceGuTexFilter(GU_NEAREST, GU_NEAREST);
    sceGuDisable(GU_TEXTURE_2D);
}

// Console diagnosis (diag.txt next to the EBOOT, one view per line: "<track> ex ey ez ax ay az"): each track is
// loaded and drawn from that fixed eye at its start, once per variant of the rendering state, and every image is
// saved as diag_<line>_<variant>.bmp; then diag.txt is deleted and the program goes on to the track menu. Variants (hex): bit 0 no face culling, bit 1 no fog,
// bit 2 no texture (G3GuSetDebug), bit 3 no alpha test, bit 4 texture colour only (GU_TFX_REPLACE), bits 5-6 the
// detail level of every piece (G3SetDrawDetail: 0, 1 or 2, the levels the race gives pieces beyond 20 and 60 units).
void RunDiag(RaceApp &app, const std::string &dir)
{
    static const unsigned kVariants[] = {0x00, 0x20, 0x40, 0x41, 0x44, 0x48, 0x5f};
    FILE *f = fopen((dir + "/diag.txt").c_str(), "rb");
    if (!f)
        return;
    char line[160], track[64];
    int n = 0, selected = app.selected;
    while (fgets(line, sizeof line, f)) {
        if (sscanf(line, "%63s %f %f %f %f %f %f", track, &g_viewEye[0], &g_viewEye[1], &g_viewEye[2], &g_viewAt[0],
                   &g_viewAt[1], &g_viewAt[2]) != 7)
            continue;
        int index = app.FindTrack(track);
        if (index < 0 || !app.Start(index)) {
            printf("diag %d: cannot load %s\n", n, track);
            continue;
        }
        g_freeView = true;
        for (unsigned v : kVariants) {
            G3GuSetDebug(v & 7);
            g_viewDetail = (v >> 5) & 3;
            sceGuStart(GU_DIRECT, g_list);
            if (v & 8)
                sceGuDisable(GU_ALPHA_TEST);
            else
                sceGuEnable(GU_ALPHA_TEST);
            sceGuTexFunc(v & 0x10 ? GU_TFX_REPLACE : GU_TFX_MODULATE, GU_TCC_RGBA);
            DrawRace(*app.race);
            sceGuFinish();
            sceGuSync(0, 0);
            char name[64];
            snprintf(name, sizeof name, "/diag_%d_%02x.bmp", n, v);
            SaveBmp((dir + name).c_str(), (const u8 *)0x04000000);
            printf("diag %d %s variant %02x saved\n", n, track, v);
            fflush(stdout);
        }
        G3GuSetDebug(0);
        g_viewDetail = 0;
        sceGuStart(GU_DIRECT, g_list);
        sceGuEnable(GU_ALPHA_TEST);
        sceGuTexFunc(GU_TFX_MODULATE, GU_TCC_RGBA);
        sceGuFinish();
        sceGuSync(0, 0);
        app.ToTracks();
        n++;
    }
    fclose(f);
    remove((dir + "/diag.txt").c_str());
    app.selected = selected;
    g_freeView = false;
}

// Frame-time log (perf.txt next to the EBOOT turns it on): one line per second of racing in frametimes.txt with the
// averages and maxima of the race update, the sound rendering, the GE list build and the GE wait (ms), the frames
// that missed their VBlank and the pieces drawn. Lines are kept in memory and appended to the file only outside the race (pause,
// menus, exit) so that the memory stick writes do not disturb the timings.
struct FrameLog {
    bool on = false;
    std::string path, pending;
    int frames = 0, late = 0, pieces = 0;
    u64 sum[4] = {}, max[4] = {};
    void Add(const char *track, u64 update, u64 sound, u64 build, u64 wait, bool wasLate, int drawn)
    {
        if (!on)
            return;
        u64 v[4] = {update, build, wait, sound};
        for (int i = 0; i < 4; i++) {
            sum[i] += v[i];
            if (v[i] > max[i])
                max[i] = v[i];
        }
        late += wasLate;
        pieces += drawn;
        if (++frames < 30)
            return;
        char line[192];
        snprintf(line, sizeof line, "%s upd %.1f/%.1f snd %.1f/%.1f build %.1f/%.1f ge %.1f/%.1f late %d pieces %d\n",
                 track, sum[0] / 30000.0, max[0] / 1000.0, sum[3] / 30000.0, max[3] / 1000.0, sum[1] / 30000.0,
                 max[1] / 1000.0, sum[2] / 30000.0, max[2] / 1000.0, late, pieces / 30);
        pending += line;
        frames = late = pieces = 0;
        for (int i = 0; i < 4; i++)
            sum[i] = max[i] = 0;
    }
    void Flush()
    {
        if (!on || pending.empty())
            return;
        if (FILE *f = fopen(path.c_str(), "ab")) {
            fwrite(pending.data(), 1, pending.size(), f);
            fclose(f);
        }
        pending.clear();
    }
};

void LogMemory(const char *what)
{
    printf("%s heap=%u textures=%u vram=%u meshes=%u files=%d\n", what, (unsigned)mallinfo().uordblks,
           (unsigned)G3GuTextureBytes(), (unsigned)G3GuVramBytes(), (unsigned)G3GuMeshBytes(), FSh_OpenFiles());
    fflush(stdout);
}

} // namespace

int main(int argc, char **argv)
{
    // The official maximum clock (CPU 333 MHz, bus 166; a homebrew starts at 222 / 111), first thing, while the GE
    // and the memory stick are idle. A custom firmware clock setting may turn the call into a no-op.
    int clockResult = scePowerSetClockFrequency(333, 333, 166);
    SceUID th = sceKernelCreateThread("callbacks", CallbackThread, 0x11, 0xfa0, 0, nullptr);
    if (th >= 0)
        sceKernelStartThread(th, 0, nullptr);
    std::string dir = argc > 0 ? argv[0] : "ms0:/PSP/GAME/TM2PSP/EBOOT.PBP";
    dir = dir.substr(0, dir.find_last_of('/'));
    char trackName[64] = "", buf[256], autoScreen[200] = "";
    int autoFrames = -1, reloads = 0;
    if (ReadText(dir + "/auto.txt", buf, sizeof buf))
        sscanf(buf, "%63s %d %199s", trackName, &autoFrames, autoScreen);
    else if (ReadText(dir + "/reload.txt", buf, sizeof buf))
        sscanf(buf, "%d", &reloads);
    else if (ReadText(dir + "/track.txt", buf, sizeof buf))
        sscanf(buf, "%63s", trackName);
    bool scripted = autoFrames >= 0 || reloads > 0;
    if (ReadText(dir + "/view.txt", buf, sizeof buf))
        g_freeView = sscanf(buf, "%f %f %f %f %f %f %d", &g_viewEye[0], &g_viewEye[1], &g_viewEye[2], &g_viewAt[0],
                            &g_viewAt[1], &g_viewAt[2], &g_viewDetail) >= 6;
    FILE *log = fopen((dir + "/race_log.txt").c_str(), "wb");
    if (log)
        stdout = log; // race events and the auto test lines (newlib's stdout is assignable)
    int cpuMhz = scePowerGetCpuClockFrequencyInt(), busMhz = scePowerGetBusClockFrequencyInt();
    printf("clock cpu %d MHz bus %d MHz (set 333/166: %08x)\n", cpuMhz, busMhz, (unsigned)clockResult);

    sceGuInit();
    sceGuStart(GU_DIRECT, g_list);
    sceGuDrawBuffer(GU_PSM_8888, (void *)0, BUF_W);
    sceGuDispBuffer(SCR_W, SCR_H, (void *)0x88000, BUF_W);
    sceGuDepthBuffer((void *)0x110000, BUF_W);
    sceGuOffset(2048 - SCR_W / 2, 2048 - SCR_H / 2);
    sceGuViewport(2048, 2048, SCR_W, SCR_H);
    sceGuDepthRange(65535, 0);
    sceGuScissor(0, 0, SCR_W, SCR_H);
    sceGuEnable(GU_SCISSOR_TEST);
    sceGuDepthFunc(GU_GEQUAL);
    sceGuEnable(GU_DEPTH_TEST);
    sceGuFrontFace(GU_CCW);
    sceGuShadeModel(GU_SMOOTH);
    sceGuEnable(GU_CLIP_PLANES);
    sceGuEnable(GU_BLEND);
    sceGuBlendFunc(GU_ADD, GU_SRC_ALPHA, GU_ONE_MINUS_SRC_ALPHA, 0, 0);
    sceGuEnable(GU_ALPHA_TEST);
    sceGuAlphaFunc(GU_GREATER, 5, 0xff); // host: alpha > 0.02
    sceGuTexFunc(GU_TFX_MODULATE, GU_TCC_RGBA);
    sceGuTexFilter(GU_NEAREST, GU_NEAREST);
    sceGuTexScale(1, 1);
    sceGuTexOffset(0, 0);
    sceGuFinish();
    sceGuSync(0, 0);
    sceDisplayWaitVblankStart();
    sceGuDisplay(GU_TRUE);
    sceCtrlSetSamplingCycle(0);
    sceCtrlSetSamplingMode(PSP_CTRL_MODE_ANALOG);

    // VRAM: draw and display buffers (8888, 0x88000 bytes each) and the 16-bit depth buffer end at 0x154000; the
    // rest (688 KB) holds textures.
    G3GuSetVram((u8 *)sceGeEdramGetAddr() + 0x154000, sceGeEdramGetSize() - 0x154000);
    FX_Init();
    FSh_SetRoot((dir + "/files").c_str());
    dsFileSystem::Create();
    dsFileSystem::Get()->EnableVfs(true);
    // The race's strings in the console's language (the DS takes the firmware's), English as gb.
    int sysLang = PSP_SYSTEMPARAM_LANGUAGE_ENGLISH, language = dsStringTable::LANG_GB;
    sceUtilityGetSystemParamInt(PSP_SYSTEMPARAM_ID_INT_LANGUAGE, &sysLang);
    if (sysLang == PSP_SYSTEMPARAM_LANGUAGE_FRENCH)
        language = dsStringTable::LANG_FR;
    else if (sysLang == PSP_SYSTEMPARAM_LANGUAGE_GERMAN)
        language = dsStringTable::LANG_DE;
    else if (sysLang == PSP_SYSTEMPARAM_LANGUAGE_ITALIAN)
        language = dsStringTable::LANG_IT;
    else if (sysLang == PSP_SYSTEMPARAM_LANGUAGE_SPANISH)
        language = dsStringTable::LANG_ES;

    // Track menu first (RaceApp); best times and the last track in save.txt next to the EBOOT. track.txt only
    // places the cursor when there is no save file yet. The scripted runs (auto.txt, reload.txt) write no save.
    std::string savePath = dir + "/save.txt";
    bool hadSave = false;
    if (FILE *f = fopen(savePath.c_str(), "rb")) {
        hadSave = true;
        fclose(f);
    }
    FeSetDataDir(dir + "/fe"); // the front end's pictures (tools/fe/convert.py)
    RaceApp app(scripted ? "" : savePath, "X:", "O:");
    app.menus = !scripted;
    app.SetLanguage(language); // strings of the race (HUD messages) and of the front end
    char nick[128] = "";
    if (sceUtilityGetSystemParamString(PSP_SYSTEMPARAM_ID_STRING_NICKNAME, nick, sizeof nick) == 0)
        app.SetPlayerName(nick); // the profile's name: the console's nickname
    if (!hadSave && trackName[0] && app.FindTrack(trackName) >= 0)
        app.selected = app.FindTrack(trackName);
    // Sound: the archive is read in every run; the scripted runs render it for the log's hash (and race.wav), the
    // others play it on the audio thread.
    std::vector<s16> capture;
    bool wantWav = ReadText(dir + "/wav.txt", buf, sizeof buf);
    if (autoFrames >= 0)
        SndStream::SetCapture(&capture);
    SceUID audioTh = -1;
    // auto.txt "<track> <frames> audio": the audio thread plays during the auto run (underruns and drops logged).
    bool audioTest = !strcmp(autoScreen, "audio");
    if (app.InitAudio() && (!scripted || audioTest) && !ReadText(dir + "/mute.txt", buf, sizeof buf)) {
        audioTh = sceKernelCreateThread("audio", AudioThread, 0x12, 0x4000, PSP_THREAD_ATTR_USER, nullptr);
        if (audioTh >= 0)
            sceKernelStartThread(audioTh, 0, nullptr);
    }
    // Ring fill kept by the producer: two game frames plus an audio block (about 80 ms).
    const int kTargetFill = 2 * SndDriver::kRate / 30 + AUDIO_BLOCK;
    if (reloads > 0)
        LogMemory("start");
    bool menuShot = !strcmp(trackName, "menu"); // auto.txt "menu <frames> [keys]": a front end page
    if (menuShot && autoScreen[0])
        app.PressScript(autoScreen); // keys as tm_race --keys (U D L R, O = OK, B = back, P = pause)
    RunDiag(app, dir);
    bool editorShot = !strncmp(trackName, "editor:", 7);
    if (editorShot) {
        const char *what = trackName + 7;
        int env = -1, puzzle = !strncmp(what, "puz", 3) ? atoi(what + 3) : -1;
        for (int e = 0; e < ENV_COUNT; e++)
            if (!strcmp(what, dsRaceSetup::EnvironmentName(e)))
                env = e;
        if (!app.OpenEditor(env, puzzle))
            printf("cannot open the editor (%s)\n", what);
        app.EditorScript(autoScreen);
        menuShot = true; // no race to start, the shot is taken as a menu's
    }
    if ((autoFrames >= 0 && !menuShot) || reloads > 0) {
        int index = autoFrames >= 0 ? app.FindTrack(trackName) : 0;
        u64 t0 = Us();
        if (!app.Start(index)) {
            printf("cannot load %s\n", trackName);
            fclose(log);
            sceKernelExitGame();
            return 1;
        }
        printf("load %u ms, heap %u bytes, textures %u bytes\n", (unsigned)((Us() - t0) / 1000),
               (unsigned)mallinfo().uordblks, (unsigned)G3GuTextureBytes());
        fflush(stdout);
    }

    u32 prevButtons = 0;
    int held = 0; // frames a menu direction has been held (auto-repeat)
    bool overlay = false;
    FrameLog frameLog;
    if (ReadText(dir + "/perf.txt", buf, sizeof buf)) {
        frameLog.on = true;
        frameLog.path = dir + "/frametimes.txt";
        char head[224];
        snprintf(head, sizeof head, "# per second: average/max ms of update, sound, GE list build, GE wait (the GE "
                 "draws the previous frame during update and sound); late frames; pieces. cpu %d MHz bus %d MHz\n",
                 cpuMhz, busMhz);
        frameLog.pending = head;
    }
    void *drawBuf = (void *)0;
    u64 sumUpdate = 0, sumDraw = 0, sumGpu = 0, sumSound = 0, maxFrame = 0, sumVerts = 0;
    int maxVerts = 0;
    int frames = 0, late = 0, reloadsDone = 0;
    u32 vcount = sceDisplayGetVcount();
    u64 prevSound = 0;
    char timing[64] = "";
    for (int frame = 0; !g_quit; frame++) {
        SceCtrlData pad;
        sceCtrlPeekBufferPositive(&pad, 1);
        u32 b = pad.Buttons, pressed = b & ~prevButtons;
        prevButtons = b;
        bool playing = app.screen == RaceApp::PLAYING;
        if (app.Editing()) {
            static const struct {
                u32 psp, pad;
            } kPad[] = {{PSP_CTRL_UP, RaceApp::PAD_UP},         {PSP_CTRL_DOWN, RaceApp::PAD_DOWN},
                        {PSP_CTRL_LEFT, RaceApp::PAD_LEFT},     {PSP_CTRL_RIGHT, RaceApp::PAD_RIGHT},
                        {PSP_CTRL_CROSS, RaceApp::PAD_CROSS},   {PSP_CTRL_CIRCLE, RaceApp::PAD_CIRCLE},
                        {PSP_CTRL_SQUARE, RaceApp::PAD_SQUARE}, {PSP_CTRL_TRIANGLE, RaceApp::PAD_TRIANGLE},
                        {PSP_CTRL_LTRIGGER, RaceApp::PAD_L},    {PSP_CTRL_RTRIGGER, RaceApp::PAD_R},
                        {PSP_CTRL_START, RaceApp::PAD_START},   {PSP_CTRL_SELECT, RaceApp::PAD_SELECT}};
            u32 held = 0;
            for (const auto &k : kPad)
                if (b & k.psp)
                    held |= k.pad;
            app.EditorPad(held, (pad.Lx - 128) / 128.0f, (pad.Ly - 128) / 128.0f);
            pressed &= ~PSP_CTRL_RTRIGGER; // R is the editor's, not the timing overlay's
        } else if (playing) {
            if (pressed & PSP_CTRL_TRIANGLE)
                app.race->Respawn();
            if (pressed & PSP_CTRL_SELECT)
                app.race->ToggleCamera();
            if (pressed & PSP_CTRL_START)
                app.Press(RaceApp::PAUSE);
            if (pressed & PSP_CTRL_LTRIGGER)
                app.Horn();
        } else {
            // Menus: d-pad with auto-repeat after 12 frames, every 3 frames.
            const u32 dirs = PSP_CTRL_UP | PSP_CTRL_DOWN | PSP_CTRL_LEFT | PSP_CTRL_RIGHT;
            held = b & dirs ? held + 1 : 0;
            u32 dirKeys = pressed | (held > 12 && held % 3 == 0 ? b : 0);
            if (dirKeys & PSP_CTRL_UP)
                app.Press(RaceApp::UP);
            if (dirKeys & PSP_CTRL_DOWN)
                app.Press(RaceApp::DOWN);
            if (dirKeys & PSP_CTRL_LEFT)
                app.Press(RaceApp::LEFT);
            if (dirKeys & PSP_CTRL_RIGHT)
                app.Press(RaceApp::RIGHT);
            if (pressed & PSP_CTRL_CROSS)
                app.Press(RaceApp::OK);
            if (pressed & PSP_CTRL_START)
                app.Press(RaceApp::PAUSE);
            if ((pressed & PSP_CTRL_CIRCLE) && !app.Press(RaceApp::BACK))
                g_quit = true;
        }
        if (pressed & PSP_CTRL_RTRIGGER)
            overlay = !overlay;
        dsCarInput in;
        in.accel = (b & PSP_CTRL_CROSS) || scripted ? FX32_ONE : 0;
        in.brake = b & PSP_CTRL_SQUARE ? FX32_ONE : 0;
        // D-pad: full lock like the DS; analog: proportional outside a dead zone (the DS has no analog input).
        int ax = pad.Lx - 128;
        fx32 steer = 0;
        if (ax > 24 || ax < -24)
            steer = (fx32)((ax > 0 ? ax - 24 : ax + 24) * FX32_ONE / 103);
        if (steer > FX32_ONE)
            steer = FX32_ONE;
        if (steer < -FX32_ONE)
            steer = -FX32_ONE;
        if (b & PSP_CTRL_RIGHT)
            steer = FX32_ONE;
        if (b & PSP_CTRL_LEFT)
            steer = -FX32_ONE;
        in.steer = -steer; // the game's steer is positive to the left (seen on a real PSP-3000)

        // auto.txt "<track> <frames> pause|result": that menu is opened for the last frame (layout check only);
        // "missed" / "freewheel": a HUD message 10 frames before the shot (as tm_race --missed-at / --free-wheel-at).
        bool msgTest = !strcmp(autoScreen, "missed") || !strcmp(autoScreen, "freewheel");
        if (autoFrames >= 0 && frame == autoFrames - 10 && app.race && msgTest) {
            if (autoScreen[0] == 'm')
                app.race->progress->events.push_back({dsRaceEvent::MISSED, 2, app.race->progress->clock});
            else
                app.race->car->noThrottle = true;
        }
        if (autoFrames >= 0 && frame == autoFrames - 1 && app.race && autoScreen[0] && !msgTest && !audioTest &&
            !menuShot)
            app.screen = !strcmp(autoScreen, "result") ? RaceApp::RESULT : RaceApp::PAUSED;
        u64 a = Us();
        app.Frame(in);
        u64 sa = Us();
        // Played: the time this frame lasts on the device (33 ms, 50 when late; at most 100 ms, the first frame
        // after a load). Scripted: kRate / 30, the same samples as the host.
        int frameUs = -1;
        if (audioTh >= 0) {
            frameUs = prevSound ? (int)(sa - prevSound) : 1000000 / 30;
            if (frameUs > 100000)
                frameUs = 100000;
        }
        prevSound = sa;
        SndStream::Produce(SndStream::FrameSamples(audioTh >= 0 ? kTargetFill : -1, frameUs));
        u64 m = Us();
        // The GE has drawn the previous frame while the CPU ran the update and the sound: wait for its end and show
        // it at the second VBlank after the previous one (30 Hz; later when the frame is late), then build this
        // frame's list, which the GE draws during the next update. Nothing the GE reads by pointer changes in the
        // update but for unloads (~G3Model waits for the GE).
        sceGuSync(0, 0);
        u64 e = Us();
        u32 now = sceDisplayGetVcount();
        bool wasLate = frame > 0 && now - vcount > 2;
        late += wasLate;
        if (frame > 0) {
            while (sceDisplayGetVcount() - vcount < 2)
                sceDisplayWaitVblankStart();
            drawBuf = sceGuSwapBuffers();
        }
        vcount = sceDisplayGetVcount();
        u64 b0 = Us();
        sceGuStart(GU_DIRECT, g_list);
        if (app.race) {
            DrawRace(*app.race);
        } else {
            sceGuClearColor(0xff401a0d);
            sceGuClearDepth(0);
            sceGuClear(GU_COLOR_BUFFER_BIT | GU_DEPTH_BUFFER_BIT);
        }
        sceGuDisable(GU_DEPTH_TEST);
        sceGuDisable(GU_TEXTURE_2D);
        sceGuDisable(GU_CULL_FACE);
        FeCanvas fe;
        fe.image = DrawFeImage;
        fe.cell = DrawFeCell;
        fe.rect = DrawRect;
        fe.Fit(SCR_W, SCR_H);
        app.Overlay(fe, SCR_W, SCR_H, 0.5f, DrawText, DrawRect, DrawSprite, nullptr);
        if (overlay)
            DrawText(4, SCR_H - 12, 1.5f, timing, 1, 1, 1, nullptr);
        sceGuEnable(GU_DEPTH_TEST);
        sceGuFinish();
        u64 d = Us();
        frames++;
        int verts = G3GuTakeDrawnVertices();
        sumVerts += verts;
        if (verts > maxVerts)
            maxVerts = verts;
        sumUpdate += sa - a;
        sumSound += m - sa;
        sumDraw += d - b0;
        sumGpu += e - m;
        if (e - a + d - b0 > maxFrame)
            maxFrame = e - a + d - b0;
        snprintf(timing, sizeof timing, "%u.%u %u.%u %u.%u %u.%u", (unsigned)((sa - a) / 1000),
                 (unsigned)((sa - a) / 100 % 10), (unsigned)((m - sa) / 1000), (unsigned)((m - sa) / 100 % 10),
                 (unsigned)((d - b0) / 1000), (unsigned)((d - b0) / 100 % 10), (unsigned)((e - m) / 1000),
                 (unsigned)((e - m) / 100 % 10));
        if (autoFrames >= 0 && frame == autoFrames - 1)
            sceGuSync(0, 0); // the shot reads the frame just built
        if (autoFrames >= 0 && frame == autoFrames - 1 && !app.race) {
            SaveBmp((dir + "/shot.bmp").c_str(), (const u8 *)0x04000000 + (uintptr_t)drawBuf);
            printf("memory heap=%u bytes (menu)\n",(unsigned)mallinfo().uordblks);
            fflush(stdout);
            g_quit = true;
        } else if (autoFrames >= 0 && frame == autoFrames - 1 && !app.race->car) {
            SaveBmp((dir + "/shot.bmp").c_str(), (const u8 *)0x04000000 + (uintptr_t)drawBuf);
            printf("editor view: pieces drawn %d of %d, lines %d\n", g_drawnPieces, (int)app.race->pieces.size(),
                   (int)app.race->lines.size());
            printf("memory heap=%u bytes (sound capture %u) textures=%u (vram %u) meshes=%u\n",
                   (unsigned)mallinfo().uordblks, (unsigned)(capture.capacity() * sizeof(s16)),
                   (unsigned)G3GuTextureBytes(), (unsigned)G3GuVramBytes(), (unsigned)G3GuMeshBytes());
            fflush(stdout);
            g_quit = true;
        } else if (autoFrames >= 0 && frame == autoFrames - 1) {
            RaceSession &race = *app.race;
            SaveBmp((dir + "/shot.bmp").c_str(), (const u8 *)0x04000000 + (uintptr_t)drawBuf);
            const MtxFx43 &mm = race.car->matrix;
            printf("frame %d pos=%d,%d,%d speed=%d gear=%d\n", frame, mm.m[3][0], mm.m[3][1], mm.m[3][2],
                   race.car->speed, race.car->engine->gear);
            const MtxFx43 &c = race.cams[race.camIndex].matrix;
            printf("camera pos=%d,%d,%d fwd=%d,%d,%d up=%d,%d,%d\n", c.m[3][0], c.m[3][1], c.m[3][2], c.m[2][0],
                   c.m[2][1], c.m[2][2], c.m[1][0], c.m[1][1], c.m[1][2]);
            printf("timing frames=%d update=%u us sound=%u us draw=%u us gpu=%u us (averages) max=%u us late=%d\n",
                   frames, (unsigned)(sumUpdate / frames), (unsigned)(sumSound / frames), (unsigned)(sumDraw / frames),
                   (unsigned)(sumGpu / frames), (unsigned)maxFrame, late);
            printf("vertices per frame %u average, %d max\n", (unsigned)(sumVerts / frames), maxVerts);
            if (audioTest)
                printf("audio thread %s underruns=%d dropped=%d fill=%d\n", audioTh >= 0 ? "on" : "off",
                       SndStream::Underruns(), SndStream::Dropped(), SndStream::Fill());
            printf("memory heap=%u bytes textures=%u (vram %u) meshes=%u (clamped %d) pieces drawn=%d of %d "
                   "sea vertices=%d\n",
                   (unsigned)mallinfo().uordblks, (unsigned)G3GuTextureBytes(), (unsigned)G3GuVramBytes(),
                   (unsigned)G3GuMeshBytes(), G3GuClampedVertices(), g_drawnPieces, (int)race.pieces.size(),
                   G3GuQuadVertices());
            fflush(stdout);
            g_quit = true;
        }
        // Reload test (reload.txt "<count>"): every 20 frames the next track of the menu is loaded; the heap, the
        // back-end texture and vertex bytes and the open files are logged with no session alive.
        if (reloads > 0 && frame % 20 == 19) {
            int trackCount = 0;
            RaceTracks(&trackCount);
            int next = (app.track + 1) % trackCount;
            app.Unload();
            snprintf(buf, sizeof buf, "reload %d", ++reloadsDone);
            LogMemory(buf);
            if (reloadsDone >= reloads || !app.Start(next)) {
                printf("reload test done\n");
                fflush(stdout);
                g_quit = true;
            }
        }
        if (app.screen == RaceApp::PLAYING && app.race)
            frameLog.Add(RaceTracks(nullptr)[app.track].id, sa - a, m - sa, d - b0, e - m, wasLate, g_drawnPieces);
        else
            frameLog.Flush();
    }
    sceGuSync(0, 0);
    if (audioTh >= 0) {
        g_audioQuit = true;
        sceKernelWaitThreadEnd(audioTh, nullptr);
        sceKernelDeleteThread(audioTh);
    }
    app.Unload();
    if (autoFrames >= 0) {
        SndStream::SetCapture(nullptr);
        u32 h = 2166136261u; // FNV-1a of the samples, as tm_race --wav
        for (s16 v : capture)
            h = (h ^ (u16)v) * 16777619u;
        printf("sound %d frames hash %08x\n", (int)(capture.size() / 2), (unsigned)h);
        if (wantWav)
            WriteWav(dir + "/race.wav", capture);
    }
    frameLog.Flush();
    fflush(stdout);
    if (log)
        fclose(log);
    sceGuTerm();
    sceKernelExitGame();
    return 0;
}
