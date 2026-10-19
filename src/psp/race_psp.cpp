// tm_race for the PSP: the Coast race (RaceSession, src/app) drawn with sceGu at 480 x 272, driven with sceCtrl.
// Data next to the EBOOT: files/ = extract/files. Optional track.txt: "easy/0" (default).
// Controls: d-pad or analog = steer, X = accelerate, square = brake/reverse, triangle = back to the last checkpoint,
// start = restart, select = other chase camera, R = timing overlay.
// Auto test: when auto.txt holds "<track> <frames>", the car accelerates on its own, the frame <frames> is saved to
// shot.bmp, race_log.txt gets the same "frame/camera" lines as `tm_race --frames N --auto` on the host plus timings
// and memory, and the program exits (tools/psp/race_shot.ps1).
#include <malloc.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#include <string>

#include <pspctrl.h>
#include <pspdmac.h>
#include <pspdisplay.h>
#include <pspgu.h>
#include <pspgum.h>
#include <pspkernel.h>

#include "fs/dsFileSystem.h"
#include "nitro.h"
#include "race_session.h"

PSP_MODULE_INFO("TM2PSP", PSP_MODULE_USER, 1, 0);
PSP_MAIN_THREAD_ATTR(PSP_THREAD_ATTR_USER | PSP_THREAD_ATTR_VFPU);
PSP_MAIN_THREAD_STACK_SIZE_KB(512);
PSP_HEAP_SIZE_KB(-1024);

size_t G3GuTextureBytes();
size_t G3GuMeshBytes();

namespace {

const int BUF_W = 512, SCR_W = 480, SCR_H = 272;
unsigned int __attribute__((aligned(16))) g_list[512 * 1024 / 4];
bool g_quit = false;

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

struct SeaVertex {
    u32 color;
    float x, y, z;
};

// Sea at the Coast sea level (2.5), as on the host, but as a grid of 24-unit tiles around the car: the GE drops
// triangles that leave its guard band, which a single 1300-unit quad does near the camera.
void DrawSea(float cx, float cz)
{
    const int N = 24;
    const float T = 24;
    float x0 = floorf(cx / T) * T - N / 2 * T, z0 = floorf(cz / T) * T - N / 2 * T;
    SeaVertex *v = (SeaVertex *)sceGuGetMemory(sizeof(SeaVertex) * N * N * 6);
    u32 c = 0xff000000 | (u32)(0.6f * 255) << 16 | (u32)(0.35f * 255) << 8 | (u32)(0.15f * 255);
    int k = 0;
    for (int i = 0; i < N; i++)
        for (int j = 0; j < N; j++) {
            float ax = x0 + i * T, az = z0 + j * T, bx = ax + T, bz = az + T;
            SeaVertex q[6] = {{c, ax, 2.5f, az}, {c, ax, 2.5f, bz}, {c, bx, 2.5f, bz},
                              {c, ax, 2.5f, az}, {c, bx, 2.5f, bz}, {c, bx, 2.5f, az}};
            memcpy(&v[k], q, sizeof q);
            k += 6;
        }
    sceGuDisable(GU_TEXTURE_2D);
    sceGumDrawArray(GU_TRIANGLES, GU_COLOR_8888 | GU_VERTEX_32BITF | GU_TRANSFORM_3D, k, nullptr, v);
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
    sceGumPerspective(cam.fov / 4096.0f, (float)SCR_W / SCR_H, 0.5f, 600.0f);
    sceGumMatrixMode(GU_VIEW);
    sceGumLoadIdentity();
    const MtxFx43 &cm = cam.matrix;
    ScePspFVector3 eye, at, up;
    eye = {cm.m[3][0] / 4096.0f, cm.m[3][1] / 4096.0f, cm.m[3][2] / 4096.0f};
    at = {eye.x + cm.m[2][0] / 4096.0f, eye.y + cm.m[2][1] / 4096.0f, eye.z + cm.m[2][2] / 4096.0f};
    up = {cm.m[1][0] / 4096.0f, cm.m[1][1] / 4096.0f, cm.m[1][2] / 4096.0f};
    sceGumLookAt(&eye, &at, &up);
    sceGumMatrixMode(GU_MODEL);
    sceGumLoadIdentity();
    DrawSea(eye.x, eye.z);
    float g[16];
    for (auto &pc : race.pieces) {
        RaceMatrix(pc.second->matrix, g);
        sceGumPushMatrix();
        sceGumMultMatrix((const ScePspFMatrix4 *)g);
        pc.first->Draw(race.batch);
        sceGumPopMatrix();
    }
    if (race.carModel) {
        RaceMatrix(race.car->matrix, g);
        sceGumPushMatrix();
        sceGumMultMatrix((const ScePspFMatrix4 *)g);
        race.carModel->Draw(nullptr);
        sceGumPopMatrix();
    }
}

} // namespace

int main(int argc, char **argv)
{
    SceUID th = sceKernelCreateThread("callbacks", CallbackThread, 0x11, 0xfa0, 0, nullptr);
    if (th >= 0)
        sceKernelStartThread(th, 0, nullptr);
    std::string dir = argc > 0 ? argv[0] : "ms0:/PSP/GAME/TM2PSP/EBOOT.PBP";
    dir = dir.substr(0, dir.find_last_of('/'));
    char trackName[64] = "easy/0", buf[128];
    int autoFrames = -1;
    if (ReadText(dir + "/auto.txt", buf, sizeof buf))
        sscanf(buf, "%63s %d", trackName, &autoFrames);
    else if (ReadText(dir + "/track.txt", buf, sizeof buf))
        sscanf(buf, "%63s", trackName);
    FILE *log = fopen((dir + "/race_log.txt").c_str(), "wb");
    if (log)
        stdout = log; // race events and the auto test lines (newlib's stdout is assignable)

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

    FX_Init();
    FSh_SetRoot((dir + "/files").c_str());
    dsFileSystem::Create();
    dsFileSystem::Get()->EnableVfs(true);
    u64 t0 = Us();
    RaceSession race;
    if (!race.Load(trackName)) {
        printf("cannot load %s\n", trackName);
        fclose(log);
        sceKernelExitGame();
        return 1;
    }
    printf("load %u ms, heap %u bytes, textures %u bytes\n", (unsigned)((Us() - t0) / 1000),
           (unsigned)mallinfo().uordblks, (unsigned)G3GuTextureBytes());
    fflush(stdout);

    u32 prevButtons = 0;
    bool overlay = false;
    void *drawBuf = (void *)0;
    u64 sumUpdate = 0, sumDraw = 0, sumGpu = 0, maxFrame = 0;
    int frames = 0, late = 0;
    u32 vcount = sceDisplayGetVcount();
    char timing[64] = "";
    for (int frame = 0; !g_quit; frame++) {
        SceCtrlData pad;
        sceCtrlPeekBufferPositive(&pad, 1);
        u32 b = pad.Buttons, pressed = b & ~prevButtons;
        prevButtons = b;
        if (pressed & PSP_CTRL_TRIANGLE)
            race.Respawn();
        if (pressed & PSP_CTRL_START)
            race.Restart();
        if (pressed & PSP_CTRL_SELECT)
            race.ToggleCamera();
        if (pressed & PSP_CTRL_RTRIGGER)
            overlay = !overlay;
        dsCarInput in;
        in.accel = (b & PSP_CTRL_CROSS) || autoFrames >= 0 ? FX32_ONE : 0;
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

        u64 a = Us();
        race.Frame(in);
        u64 m = Us();
        sceGuStart(GU_DIRECT, g_list);
        DrawRace(race);
        sceGuDisable(GU_DEPTH_TEST);
        sceGuDisable(GU_TEXTURE_2D);
        sceGuDisable(GU_CULL_FACE);
        RaceHudLayout(SCR_W, SCR_H, 0.5f, *race.progress, DrawText, nullptr);
        if (overlay)
            DrawText(4, SCR_H - 12, 1.5f, timing, 1, 1, 1, nullptr);
        sceGuEnable(GU_DEPTH_TEST);
        sceGuFinish();
        u64 d = Us();
        sceGuSync(0, 0);
        u64 e = Us();
        frames++;
        sumUpdate += m - a;
        sumDraw += d - m;
        sumGpu += e - d;
        if (e - a > maxFrame)
            maxFrame = e - a;
        snprintf(timing, sizeof timing, "%u.%u %u.%u %u.%u", (unsigned)((m - a) / 1000), (unsigned)((m - a) / 100 % 10),
                 (unsigned)((d - m) / 1000), (unsigned)((d - m) / 100 % 10), (unsigned)((e - d) / 1000),
                 (unsigned)((e - d) / 100 % 10));
        if (autoFrames >= 0 && frame == autoFrames - 1) {
            SaveBmp((dir + "/shot.bmp").c_str(), (const u8 *)0x04000000 + (uintptr_t)drawBuf);
            const MtxFx43 &mm = race.car->matrix;
            printf("frame %d pos=%d,%d,%d speed=%d gear=%d\n", frame, mm.m[3][0], mm.m[3][1], mm.m[3][2],
                   race.car->speed, race.car->engine->gear);
            const MtxFx43 &c = race.cams[race.camIndex].matrix;
            printf("camera pos=%d,%d,%d fwd=%d,%d,%d up=%d,%d,%d\n", c.m[3][0], c.m[3][1], c.m[3][2], c.m[2][0],
                   c.m[2][1], c.m[2][2], c.m[1][0], c.m[1][1], c.m[1][2]);
            printf("timing frames=%d update=%u us draw=%u us gpu=%u us (averages) max=%u us late=%d\n", frames,
                   (unsigned)(sumUpdate / frames), (unsigned)(sumDraw / frames), (unsigned)(sumGpu / frames),
                   (unsigned)maxFrame, late);
            printf("memory heap=%u bytes textures=%u meshes=%u\n", (unsigned)mallinfo().uordblks,
                   (unsigned)G3GuTextureBytes(), (unsigned)G3GuMeshBytes());
            fflush(stdout);
            g_quit = true;
        }
        // 30 Hz: the frame is shown at the second VBlank after the previous one (later when the frame is late).
        u32 now = sceDisplayGetVcount();
        if (now - vcount > 2)
            late++;
        while (sceDisplayGetVcount() - vcount < 2)
            sceDisplayWaitVblankStart();
        vcount = sceDisplayGetVcount();
        drawBuf = sceGuSwapBuffers();
    }
    fflush(stdout);
    if (log)
        fclose(log);
    sceGuTerm();
    sceKernelExitGame();
    return 0;
}
