// Host test driver for the physics slice.
//   tm_physics --root <dir> [--raw] [--track easy/0] --raycast | --engine
// --raycast  builds the collision world of a Coast track (one mesh shape per placed piece) and casts batches of
//            probes over every piece, vertical and slanted, like the car's wheel and body probes; prints every hit.
//            tools/host/check_raycast.py compares them with a float ray cast of the same triangles.
// --engine   gear tables and torque curves of the four cars, and the body boxes of their models
//            (tools/host/check_engine.py).
#include <stdio.h>
#include <string.h>

#include <string>
#include <vector>

#include "car/dsCarEngine.h"
#include "car/dsCarParams.h"
#include "collision/dsCollisionWorld.h"
#include "gfx/dsModelInfo.h"
#include "fs/dsFileSystem.h"
#include "math/dsMath.h"
#include "nitro.h"
#include "track/dsPieceData.h"
#include "track/dsRaceSetup.h"
#include "track/dsTrack.h"

static int ShapeIndex(const dsTrack &t, const dsCollisionShape *s)
{
    for (size_t i = 0; i < t.pieces.size(); i++)
        if (t.pieces[i]->collision == s)
            return (int)i;
    return -1;
}

static int Raycast(dsTrack &t)
{
    dsCollisionWorld *world = dsCollisionWorld::Get();
    for (dsTrackPiece *p : t.pieces)
        p->CreateCollision();
    printf("shapes %d\n", (int)world->Shapes().size());
    for (size_t i = 0; i < t.pieces.size(); i++) {
        const dsTrackPiece *p = t.pieces[i];
        const MtxFx43 &m = p->matrix;
        printf("piece %d %s mtx=", (int)i, p->modelPath.c_str());
        for (int r = 0; r < 4; r++)
            printf("%d,%d,%d%s", m.m[r][0], m.m[r][1], m.m[r][2], r < 3 ? ";" : "\n");
    }
    // Probe directions: straight down, and two slanted unit vectors (normalised like the game does).
    VecFx32 dirs[3] = {{0, -FX32_ONE, 0}, {0x800, -0x3000, 0x400}, {-0x600, -0x2800, -0x900}};
    for (int k = 1; k < 3; k++)
        dsVec_Normalize(&dirs[k], &dirs[k]);
    int rays = 0, hits = 0;
    std::vector<std::vector<dsCollisionHit>> results(10);
    struct Probe { VecFx32 o, d; fx32 len; };
    std::vector<Probe> batch;
    auto flush = [&]() {
        world->SetMasks(1, 0);
        for (size_t i = 0; i < batch.size(); i++) {
            results[i].clear();
            world->AddSegment(batch[i].o, batch[i].d, batch[i].len, &results[i]);
        }
        world->TestSegments();
        for (size_t i = 0; i < batch.size(); i++) {
            printf("ray %d,%d,%d %d,%d,%d %d hits=%d", batch[i].o.x, batch[i].o.y, batch[i].o.z, batch[i].d.x,
                   batch[i].d.y, batch[i].d.z, batch[i].len, (int)results[i].size());
            for (const dsCollisionHit &h : results[i])
                printf(" [%d %d,%d,%d %d,%d,%d %u %d]", h.t, h.point.x, h.point.y, h.point.z, h.normal.x,
                       h.normal.y, h.normal.z, h.material, ShapeIndex(t, h.shape));
            printf("\n");
            rays++;
            hits += results[i].empty() ? 0 : 1;
        }
        batch.clear();
    };
    for (const dsTrackPiece *p : t.pieces) {
        int w = p->info ? p->info->width : 1, d = p->info ? p->info->depth : 1;
        if (p->rotation & 1)
            std::swap(w, d);
        fx32 cw = dsRaceSetup::Get()->CellWidthFx();
        // footprint: x from cell - (w-1) to cell, z from cell to cell + d - 1, each cell centred on its index * 16
        for (int a = 0; a < w * 3; a++)
            for (int b = 0; b < d * 3; b++) {
                fx32 x = (p->cell[0] - (w - 1)) * cw - cw / 2 + (a * cw) / 3 + cw / 6 + 37;
                fx32 z = p->cell[2] * cw - cw / 2 + (b * cw) / 3 + cw / 6 - 53;
                const VecFx32 &dir = dirs[(a + b) % 3];
                fx32 len = 0x40000 + 0x5000 * ((a * 7 + b) % 5);
                VecFx32 o = {x - FX_MUL(dir.x, len / 2), p->pos.y + 0x20000, z - FX_MUL(dir.z, len / 2)};
                batch.push_back({o, dir, len});
                if (batch.size() == 10)
                    flush();
            }
    }
    if (!batch.empty())
        flush();
    printf("rays %d hit %d\n", rays, hits);
    return 0;
}

// Gear table and torque curve of every car, plus the car body box of each skin's lod1 model.
static int EngineDump()
{
    dsCarParamsTable table;
    table.Load();
    for (const auto &kv : table.All()) {
        const dsCarParams &p = kv.second;
        dsCarEngine e(p);
        printf("engine %s rpm=%d,%d,%d gears=%d max=%d gap=%d\n", kv.first.c_str(), e.rpmMin, e.rpmMax, e.rpmShiftUp,
               (int)e.gears.size(), e.MaxSpeed(), e.gearChangeGap);
        for (size_t i = 0; i < e.gears.size(); i++) {
            const dsCarGear &g = e.gears[i];
            printf("gear %s %d ratio=%d torque=%d start=%d down=%d\n", kv.first.c_str(), (int)i, g.ratio, g.torque,
                   g.startRpm, g.downRpm);
        }
        for (s32 gear = -1; gear < (s32)e.gears.size(); gear++) {
            e.gear = gear;
            printf("torque %s %d", kv.first.c_str(), gear);
            for (fx32 r = 0; r <= e.rpmMax + 0x100000; r += 0x80000)
                printf(" %d", e.Torque(r));
            printf("\n");
        }
        for (int skin = 0; skin < 10; skin++) {
            char name[64];
            snprintf(name, sizeof name, "Cars/%s/%d/lod1", kv.first.c_str(), skin);
            VecFx32 size, centre;
            if (dsModel_GetBox(name, &size, &centre))
                printf("box %s %d %d,%d,%d %d,%d,%d\n", kv.first.c_str(), skin, size.x, size.y, size.z, centre.x,
                       centre.y, centre.z);
        }
    }
    return 0;
}

int main(int argc, char **argv)
{
    const char *root = "extract/files", *track = "easy/0";
    bool raw = false, raycast = false, engine = false;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--root") && i + 1 < argc)
            root = argv[++i];
        else if (!strcmp(argv[i], "--raw"))
            raw = true;
        else if (!strcmp(argv[i], "--track") && i + 1 < argc)
            track = argv[++i];
        else if (!strcmp(argv[i], "--raycast"))
            raycast = true;
        else if (!strcmp(argv[i], "--engine"))
            engine = true;
        else {
            fprintf(stderr, "usage: tm_physics --root <dir> [--raw] [--track easy/0] --raycast\n");
            return 2;
        }
    }
    FX_Init();
    FSh_SetRoot(root);
    dsFileSystem::Create();
    dsFileSystem::Get()->EnableVfs(!raw);
    if (engine)
        return EngineDump();
    dsRaceSetup *setup = dsRaceSetup::Get();
    setup->mode = 1;
    setup->SetEnvironment(ENV_COAST);
    dsPieceDataTable::Get()->Load(ENV_COAST);
    dsTrack t;
    std::string scene = std::string("coa/") + track;
    if (!t.LoadScene(scene.c_str(), true)) {
        fprintf(stderr, "cannot load scene %s\n", scene.c_str());
        return 1;
    }
    if (raycast)
        return Raycast(t);
    return 0;
}
