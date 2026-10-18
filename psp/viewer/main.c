// TM2PSP model viewer: draws a TMPM model (tools/assets/psp_model.py) with sceGu.
// Loads <EBOOT dir>/model.tmpm, spins it, writes <EBOOT dir>/shot.bmp after SHOT_FRAME frames
// (so the rendering can be checked without a screen), then keeps running until START is pressed.
#include <pspkernel.h>
#include <pspdisplay.h>
#include <pspctrl.h>
#include <pspgu.h>
#include <pspgum.h>
#include <malloc.h>
#include <stdio.h>
#include <string.h>

PSP_MODULE_INFO("TM2PSP_VIEWER", 0, 1, 0);
PSP_MAIN_THREAD_ATTR(THREAD_ATTR_USER | THREAD_ATTR_VFPU);
PSP_HEAP_SIZE_KB(-1024);

#define BUF_WIDTH 512
#define SCR_WIDTH 480
#define SCR_HEIGHT 272
#define SHOT_FRAME 45

static unsigned int __attribute__((aligned(16))) list[262144];

typedef struct {
    char magic[4];
    unsigned short version, flags;
    unsigned short n_tex, n_mat, n_mesh, n_draw;
    float up_scale;
    float node[12];
    unsigned int off_tex, off_mat, off_mesh, off_draw;
} TmpmHeader;

typedef struct {
    unsigned short w, h;
    unsigned char psm, swizzled, clut_psm, pad;
    unsigned short clut_count, pad2;
    unsigned int data_off, data_size, clut_off;
} TmpmTexture;

typedef struct {
    short tex;
    unsigned char alpha, flags;
    unsigned int diffuse, ambient, specular, emission;
    float su, sv;
} TmpmMaterial;

typedef struct {
    unsigned int vtype;
    unsigned short vsize, pad;
    unsigned int n_verts, n_idx, vtx_off, idx_off;
} TmpmMesh;

enum { MAT_CULL_BACK = 1, MAT_CULL_FRONT = 2, MAT_REPEAT_S = 4, MAT_REPEAT_T = 8, MAT_TRANSLUCENT = 64 };

static unsigned char *load_file(const char *path, int *size)
{
    SceUID fd = sceIoOpen(path, PSP_O_RDONLY, 0);
    if (fd < 0)
        return NULL;
    *size = sceIoLseek(fd, 0, PSP_SEEK_END);
    sceIoLseek(fd, 0, PSP_SEEK_SET);
    unsigned char *buf = memalign(64, *size);
    sceIoRead(fd, buf, *size);
    sceIoClose(fd);
    sceKernelDcacheWritebackAll();
    return buf;
}

static void write_bmp(const char *path, const unsigned int *vram)
{
    static unsigned char row[SCR_WIDTH * 3];
    unsigned char hdr[54] = {'B', 'M'};
    int size = 54 + SCR_WIDTH * SCR_HEIGHT * 3;
    memcpy(hdr + 2, &size, 4);
    hdr[10] = 54; hdr[14] = 40;
    int w = SCR_WIDTH, h = SCR_HEIGHT;
    memcpy(hdr + 18, &w, 4);
    memcpy(hdr + 22, &h, 4);
    hdr[26] = 1; hdr[28] = 24;
    SceUID fd = sceIoOpen(path, PSP_O_WRONLY | PSP_O_CREAT | PSP_O_TRUNC, 0777);
    if (fd < 0)
        return;
    sceIoWrite(fd, hdr, 54);
    for (int y = SCR_HEIGHT - 1; y >= 0; y--) {
        for (int x = 0; x < SCR_WIDTH; x++) {
            unsigned int c = vram[y * BUF_WIDTH + x];
            row[x * 3 + 0] = (c >> 16) & 0xff;
            row[x * 3 + 1] = (c >> 8) & 0xff;
            row[x * 3 + 2] = c & 0xff;
        }
        sceIoWrite(fd, row, sizeof(row));
    }
    sceIoClose(fd);
}

static int exit_cb(int a, int b, void *c) { sceKernelExitGame(); return 0; }
static int cb_thread(SceSize args, void *argp)
{
    int cb = sceKernelCreateCallback("exit", exit_cb, NULL);
    sceKernelRegisterExitCallback(cb);
    sceKernelSleepThreadCB();
    return 0;
}

int main(int argc, char *argv[])
{
    SceUID th = sceKernelCreateThread("cb", cb_thread, 0x11, 0xFA0, 0, 0);
    if (th >= 0)
        sceKernelStartThread(th, 0, 0);

    char dir[256] = "", path[300];
    if (argc > 0) {
        strncpy(dir, argv[0], sizeof(dir) - 1);
        char *slash = strrchr(dir, '/');
        if (slash)
            slash[1] = 0;
    }
    snprintf(path, sizeof(path), "%smodel.tmpm", dir);
    int size = 0;
    unsigned char *file = load_file(path, &size);

    sceGuInit();
    sceGuStart(GU_DIRECT, list);
    sceGuDrawBuffer(GU_PSM_8888, (void *)0, BUF_WIDTH);
    sceGuDispBuffer(SCR_WIDTH, SCR_HEIGHT, (void *)0x88000, BUF_WIDTH);
    sceGuDepthBuffer((void *)0x110000, BUF_WIDTH);
    sceGuOffset(2048 - SCR_WIDTH / 2, 2048 - SCR_HEIGHT / 2);
    sceGuViewport(2048, 2048, SCR_WIDTH, SCR_HEIGHT);
    sceGuDepthRange(65535, 0);
    sceGuScissor(0, 0, SCR_WIDTH, SCR_HEIGHT);
    sceGuEnable(GU_SCISSOR_TEST);
    sceGuDepthFunc(GU_GEQUAL);
    sceGuEnable(GU_DEPTH_TEST);
    sceGuFrontFace(GU_CCW);
    sceGuShadeModel(GU_SMOOTH);
    sceGuEnable(GU_TEXTURE_2D);
    sceGuEnable(GU_LIGHTING);
    sceGuEnable(GU_LIGHT0);
    sceGuFinish();
    sceGuSync(0, 0);
    sceDisplayWaitVblankStart();
    sceGuDisplay(GU_TRUE);

    TmpmHeader *hdr = (TmpmHeader *)file;
    int ok = file && memcmp(hdr->magic, "TMPM", 4) == 0;
    float angle = 0.0f;
    int frame = 0;
    SceCtrlData pad;
    sceCtrlSetSamplingCycle(0);
    sceCtrlSetSamplingMode(PSP_CTRL_MODE_ANALOG);

    for (;;) {
        sceGuStart(GU_DIRECT, list);
        sceGuClearColor(ok ? 0xff604030 : 0xff0000c0);  // blue-grey, or red when the model failed to load
        sceGuClearDepth(0);
        sceGuClear(GU_COLOR_BUFFER_BIT | GU_DEPTH_BUFFER_BIT);

        if (ok) {
            ScePspFVector3 ldir = {0.4f, 0.8f, 0.6f};
            sceGuLight(0, GU_DIRECTIONAL, GU_DIFFUSE, &ldir);
            sceGuLightColor(0, GU_DIFFUSE, 0xffffffff);
            sceGuAmbient(0xff606060);

            sceGumMatrixMode(GU_PROJECTION);
            sceGumLoadIdentity();
            sceGumPerspective(45.0f, 16.0f / 9.0f, 0.1f, 100.0f);
            sceGumMatrixMode(GU_VIEW);
            sceGumLoadIdentity();
            ScePspFVector3 eye = {0.0f, 3.0f, 9.0f}, at = {0.0f, 0.5f, 0.0f}, up = {0.0f, 1.0f, 0.0f};
            sceGumLookAt(&eye, &at, &up);

            sceGumMatrixMode(GU_MODEL);
            sceGumLoadIdentity();
            sceGumRotateY(angle);
            // Positions are DS 1.3.12 int16; GU normalizes 16-bit positions to [-1, 1) (value / 32768),
            // so DS units are recovered with a factor of 32768 / 4096 = 8.
            float s = hdr->up_scale * 8.0f;
            ScePspFVector3 sc = {s, s, s};
            sceGumScale(&sc);

            TmpmTexture *tex = (TmpmTexture *)(file + hdr->off_tex);
            TmpmMaterial *mat = (TmpmMaterial *)(file + hdr->off_mat);
            TmpmMesh *mesh = (TmpmMesh *)(file + hdr->off_mesh);
            unsigned short *draws = (unsigned short *)(file + hdr->off_draw);
            for (int i = 0; i < hdr->n_draw; i++) {
                TmpmMaterial *m = &mat[draws[2 * i]];
                TmpmMesh *ms = &mesh[draws[2 * i + 1]];
                if (m->flags & MAT_CULL_BACK)
                    sceGuEnable(GU_CULL_FACE);
                else
                    sceGuDisable(GU_CULL_FACE);
                sceGuMaterial(GU_AMBIENT | GU_DIFFUSE, m->diffuse | 0xff000000);
                sceGuColor(0xffffffff);
                if (m->tex >= 0) {
                    TmpmTexture *t = &tex[m->tex];
                    sceGuEnable(GU_TEXTURE_2D);
                    if (t->clut_count) {
                        sceGuClutMode(t->clut_psm, 0, 0xff, 0);
                        sceGuClutLoad(t->clut_count / 8, file + t->clut_off);
                    }
                    sceGuTexMode(t->psm, 0, 0, t->swizzled);
                    sceGuTexImage(0, t->w, t->h, t->w, file + t->data_off);
                    sceGuTexFunc(GU_TFX_MODULATE, GU_TCC_RGBA);
                    sceGuTexFilter(GU_LINEAR, GU_LINEAR);
                    sceGuTexWrap(m->flags & MAT_REPEAT_S ? GU_REPEAT : GU_CLAMP,
                                 m->flags & MAT_REPEAT_T ? GU_REPEAT : GU_CLAMP);
                    sceGuTexScale(m->su, m->sv);
                    sceGuTexOffset(0.0f, 0.0f);
                    sceGuEnable(GU_ALPHA_TEST);
                    sceGuAlphaFunc(GU_GREATER, 0, 0xff);
                } else {
                    sceGuDisable(GU_TEXTURE_2D);
                }
                if (m->flags & MAT_TRANSLUCENT) {
                    sceGuEnable(GU_BLEND);
                    sceGuBlendFunc(GU_ADD, GU_SRC_ALPHA, GU_ONE_MINUS_SRC_ALPHA, 0, 0);
                } else {
                    sceGuDisable(GU_BLEND);
                }
                sceGumDrawArray(GU_TRIANGLES, ms->vtype | GU_INDEX_16BIT | GU_TRANSFORM_3D, ms->n_idx,
                                file + ms->idx_off, file + ms->vtx_off);
            }
        }
        sceGuFinish();
        sceGuSync(0, 0);

        if (++frame == SHOT_FRAME) {
            // The frame just drawn is in the draw buffer; read it through the uncached VRAM mirror.
            void *draw = sceGuSwapBuffers();
            snprintf(path, sizeof(path), "%sshot.bmp", dir);
            write_bmp(path, (const unsigned int *)(0x44000000 + (unsigned int)draw));
        } else {
            sceDisplayWaitVblankStart();
            sceGuSwapBuffers();
        }

        sceCtrlReadBufferPositive(&pad, 1);
        if (pad.Buttons & PSP_CTRL_START)
            break;
        angle += (pad.Lx - 128) / 2048.0f + 0.01f;
    }
    sceGuTerm();
    sceKernelExitGame();
    return 0;
}
