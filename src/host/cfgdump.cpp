// Host test driver for the first vertical slice: VFS + Lua config loading.
//   tm_cfgdump --root <dir> [--raw] [--cat <path>] [--carparams]
// --root   directory standing for the ROM's file system (extract/files with vfs.zip, or assets/vfs with --raw)
// --raw    do not enable the VFS: files are read from the directory (the game's "raw:" path)
// --cat    write one game file to stdout (through dsFileSystem::Open)
// --carparams  load Scripts/config/vehicles.luac like ov003 does and print every car's parameters
// --cameras    load Scripts/config/cameras like main 0x0208fa18 for both chase cameras of every environment,
//              and print the spring damping table lookups (main 0x020abc68)
#include <stdio.h>
#include <string.h>
#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif

#include "car/dsCarParams.h"
#include "race/dsChaseCamera.h"
#include "fs/dsFileSystem.h"
#include "nitro.h"

static int Cat(const char *path)
{
#ifdef _WIN32
    _setmode(_fileno(stdout), _O_BINARY);
#endif
    dsFile *f = dsFileSystem::Get()->Open(path);
    if (!f || !f->IsOpen()) {
        fprintf(stderr, "cannot open %s\n", path);
        delete f;
        return 1;
    }
    int size = f->GetSize();
    char buf[4096];
    int total = 0, n;
    while (total < size && (n = f->Read(buf, sizeof buf < (size_t)(size - total) ? sizeof buf : (u32)(size - total))) > 0) {
        fwrite(buf, 1, (size_t)n, stdout);
        total += n;
    }
    delete f;
    return total == size ? 0 : 1;
}

static int DumpCarParams()
{
    dsCarParamsTable table;
    table.Load();
    printf("SaveTime %s\n", table.SaveTime().c_str());
    for (const auto &kv : table.All()) {
        // Load() registers the descriptors on the stored object; print them in registration order.
        const dsCarParams &p = kv.second;
        printf("car %s name=%s fields=%d\n", kv.first.c_str(), p.name.c_str(), (int)p.params.size());
        for (const dsCarParamDesc &d : p.params)
            printf("  %s %d\n", d.name, d.loaded);
        printf("  FrontWheel %d %d %d\n", p.FrontWheel[0], p.FrontWheel[1], p.FrontWheel[2]);
        printf("  RearWheel %d %d %d\n", p.RearWheel[0], p.RearWheel[1], p.RearWheel[2]);
        printf("  derived SkidSpeedEffect=%d SteerFirmUp100=%d FlatAngCos=%d MagicGripSpeed=%d Inertia=%d"
               " TopSpeed=%d,%d,%d,%d,%d,%d,%d WheelTurnVisual=%d DonutSpeed=%d\n",
               p.d_SkidSpeedEffect, p.d_SteerFirmUp100, p.d_FlatAngCos, p.d_MagicGripSpeed, p.d_Inertia,
               p.d_TopSpeed[0], p.d_TopSpeed[1], p.d_TopSpeed[2], p.d_TopSpeed[3], p.d_TopSpeed[4],
               p.d_TopSpeed[5], p.d_TopSpeed[6], p.d_WheelTurnVisual, p.d_DonutSpeed);
    }
    return table.All().empty() ? 1 : 0;
}

static int DumpCameras()
{
    static const char *const envs[] = {"sta", "isl", "sno", "coa"};
    int rc = 0;
    for (const char *env : envs)
        for (int kind = 1; kind <= 2; kind++) {
            dsChaseCamera c(kind);
            if (!c.Load(env)) {
                printf("camera %s %d missing\n", env, kind);
                rc = 1;
                continue;
            }
            printf("camera %s %d Height=%d Distance=%d TiltTightness=%d LookAtHeight=%d FOV=%d DirTightness=%d"
                   " DistInc=%d DistDec=%d RollMode=%d DistTightness=%d damping=%d\n", env, kind, c.height, c.distance,
                   c.tiltTightness, c.lookAtHeight, c.fov, c.dirTightness, c.distInc, c.distDec, c.rollMode,
                   c.distSpring.k, c.distSpring.d);
        }
    for (fx32 x = -0x4000; x <= 0x160000; x += 0x1800)
        printf("damping %d %d\n", x, dsSpring::Damping(x));
    return rc;
}

int main(int argc, char **argv)
{
    const char *root = ".";
    const char *cat = nullptr;
    bool raw = false, carParams = false, cameras = false;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--root") && i + 1 < argc)
            root = argv[++i];
        else if (!strcmp(argv[i], "--raw"))
            raw = true;
        else if (!strcmp(argv[i], "--cat") && i + 1 < argc)
            cat = argv[++i];
        else if (!strcmp(argv[i], "--carparams"))
            carParams = true;
        else if (!strcmp(argv[i], "--cameras"))
            cameras = true;
        else {
            fprintf(stderr, "usage: %s --root <dir> [--raw] [--cat <path>] [--carparams] [--cameras]\n", argv[0]);
            return 2;
        }
    }
    FX_Init();
    FSh_SetRoot(root);
    FS_Init(3);
    dsFileSystem::Create();
    dsFileSystem::Get()->EnableVfs(!raw);
    if (!raw && !dsFileSystem::Get()->IsVfsEnabled()) {
        fprintf(stderr, "VFS not enabled\n");
        return 1;
    }
    int rc = 0;
    if (cat)
        rc |= Cat(cat);
    if (carParams)
        rc |= DumpCarParams();
    if (cameras)
        rc |= DumpCameras();
    return rc;
}
