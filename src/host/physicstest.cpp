// Host test driver for the physics slice.
//   tm_physics --root <dir> [--raw] [--env coa] [--track easy/0] --raycast | --engine | --drop | --progress | --camera
// --env picks the environment (its piece catalogue, car and cameras): coa (default), isl, sno, sta.
// --raycast  builds the collision world of a Coast track (one mesh shape per placed piece) and casts batches of
//            probes over every piece, vertical and slanted, like the car's wheel and body probes; prints every hit.
//            tools/host/check_raycast.py compares them with a float ray cast of the same triangles.
// --engine   gear tables and torque curves of the four cars, and the body boxes of their models
//            (tools/host/check_engine.py).
// --drop     Coast car dropped on the start platform, throttle then brake (tools/host/check_drop.py).
// --progress race progress scenario: countdown, gates, checkpoints, laps, respawn (tools/host/check_progress.py).
// --camera   chase camera behind the car: standing, accelerating, braking, reversing (tools/host/check_camera.py).
#include <stdio.h>
#include <string.h>

#include <string>
#include <vector>

#include "car/dsCarDynamics.h"
#include "car/dsCarEngine.h"
#include "car/dsCarParams.h"
#include "core/dsTime.h"
#include "collision/dsCollisionWorld.h"
#include "gfx/dsModelInfo.h"
#include "fs/dsFileSystem.h"
#include "math/dsMath.h"
#include "nitro.h"
#include "track/dsPieceData.h"
#include "track/dsRaceSetup.h"
#include "track/dsTrack.h"
#include "race/dsChaseCamera.h"
#include "race/dsRaceProgress.h"

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
        dsCarModelGeometry geo;
        if (geo.Load(kv.first, 0, p)) {
            dsCarDynamics dyn(kv.first, &geo);
            dyn.LoadParams(p);
            printf("car %s half=%d,%d,%d centre=%d,%d,%d radius=%d,%d raise=%d max=%d\n", kv.first.c_str(), dyn.halfSize.x,
                   dyn.halfSize.y, dyn.halfSize.z, dyn.bodyCentre.x, dyn.bodyCentre.y, dyn.bodyCentre.z,
                   dyn.invFrontRadius, dyn.invRearRadius, dyn.matrix.m[3][1], dyn.maxSpeed);
            for (int i = 0; i < 4; i++) {
                const dsCarWheel *w = dyn.wheels[i];
                printf("wheel %s %d pos=%d,%d,%d probe=%d,%d,%d travel=%d radius=%d range=%d,%d\n", kv.first.c_str(), i,
                       w->pos.x, w->pos.y, w->pos.z, w->probe.x, w->probe.y, w->probe.z, w->travel, w->radius,
                       w->travelMin, w->travelMax);
            }
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

// Height of the ground under (x, z): a vertical probe from 40 units up.
static bool GroundAt(fx32 x, fx32 top, fx32 z, fx32 *y)
{
    std::vector<dsCollisionHit> hits;
    dsCollisionWorld *w = dsCollisionWorld::Get();
    w->SetMasks(1, 0);
    w->AddSegment({x, top, z}, {0, -FX32_ONE, 0}, 0xa000, &hits);
    w->TestSegments();
    if (hits.empty())
        return false;
    fx32 best = hits[0].t;
    *y = hits[0].point.y;
    for (const auto &h : hits)
        if (h.t < best) {
            best = h.t;
            *y = h.point.y;
        }
    return true;
}

// --drop: the Coast car is placed 1 unit above the start piece, left to settle with no input (90 steps), then
// driven with full throttle (150 steps) and braked (60 steps). One line per step.
static int Drop(dsTrack &t, const char *carName)
{
    dsCarParamsTable table;
    table.Load();
    if (table.All().find(carName) == table.All().end()) {
        fprintf(stderr, "no parameters for %s (vehicles.luac needs the VFS)\n", carName);
        return 1;
    }
    const dsCarParams &prm = table.Get(carName);
    static dsCarModelGeometry geo;
    if (!geo.Load(carName, 0, prm)) {
        fprintf(stderr, "no model for %s\n", carName);
        return 1;
    }
    for (dsTrackPiece *pc : t.pieces)
        pc->CreateCollision();
    dsTrackPiece *start = nullptr;
    for (dsTrackPiece *pc : t.pieces)
        if (!start && pc->info && pc->info->type == PIECE_START)
            start = pc;
    if (!start) {
        fprintf(stderr, "no start piece\n");
        return 1;
    }
    // Two units from the start piece's position along x: inside the flat start platform of the Coast start
    // pieces (found by scanning the collision; the piece's own footprint rules are in dsTrack.cpp).
    VecFx32 c = {start->pos.x + 0x2000, 0, start->pos.z};
    fx32 gy = 0;
    bool found = GroundAt(c.x, start->matrix.m[3][1] + 0x4000, c.z, &gy);
    if (!found) {
        fprintf(stderr, "no ground under the start piece\n");
        return 1;
    }
    dsCarDynamics car(carName, &geo);
    car.LoadParams(prm);
    car.pieceRotationAt = [&t](const VecFx32 &p) { return t.PieceRotationAt(p); };
    car.Reset();
    MtxFx43 m;
    MTX_RotY43_(&m, dsSin(start->rotation * 6434), dsCos(start->rotation * 6434));
    m.m[3][0] = c.x;
    m.m[3][1] = gy + FX32_ONE;
    m.m[3][2] = c.z;
    car.Place(m, false, false);
    printf("start piece=%d rot=%d ground=%d pos=%d,%d,%d\n", start->id, start->rotation, gy, c.x, m.m[3][1], c.z);
    g_dsFrameTime = 0x44; // the game's tick, 1/60 s
    for (int step = 0; step < 600; step++) {
        dsCarInput in;
        if (180 <= step && step < 480)
            in.accel = FX32_ONE;
        else if (480 <= step)
            in.brake = FX32_ONE;
        car.SetInput(in);
        car.Step();
        printf("step %d pos=%d,%d,%d up=%d,%d,%d v=%d,%d,%d speed=%d fwd=%d wheels=%d gear=%d rpm=%d hits=%d\n", step,
               car.matrix.m[3][0], car.matrix.m[3][1], car.matrix.m[3][2], car.matrix.m[1][0], car.matrix.m[1][1],
               car.matrix.m[1][2], car.velocity.x, car.velocity.y, car.velocity.z, car.speed, car.forwardSpeed,
               car.groundedWheels, car.engine->gear, car.engine->rpm, car.bodyHitCount);
    }
    return 0;
}

static const char *const kEventNames[] = {"restart", "go", "checkpoint", "missed", "lap", "finish", "respawn"};

// Race progress scenario (dsRaceProgress): the real countdown with the car settling on the start, then gates
// crossed by moving the car (5 units before the plane, then 1 unit before it, moving along the gate normal: the
// test point is 3 units ahead), a respawn at the last checkpoint, and a car left under the sea.
static int Progress(dsTrack &t, const char *carName)
{
    dsCarParamsTable table;
    table.Load();
    if (table.All().find(carName) == table.All().end())
        return 1;
    const dsCarParams &prm = table.Get(carName);
    static dsCarModelGeometry geo;
    if (!geo.Load(carName, 0, prm))
        return 1;
    t.BuildTerrain();
    for (dsTrackPiece *pc : t.pieces)
        pc->CreateCollision();
    dsCarDynamics car(carName, &geo);
    car.LoadParams(prm);
    car.pieceRotationAt = [&t](const VecFx32 &p) { return t.PieceRotationAt(p); };
    g_dsFrameTime = 0x44;
    dsRaceProgress rp(&t, &car);
    rp.Reset(true);
    printf("race checkpoints=%d laps=%d multilap=%d%s", rp.checkpointCount, rp.lapCount,
           rp.startPiece && rp.startPiece->type == PIECE_MULTILAP ? 1 : 0, "\n");
    size_t shown = 0;
    auto flush = [&](int frame) {
        for (; shown < rp.events.size(); shown++) {
            const dsRaceEvent &e = rp.events[shown];
            printf("event %d %s %d time=%d%s", frame, kEventNames[e.type], e.value, e.time, "\n");
        }
    };
    int frame = 0;
    dsCarInput none;
    for (; frame < 200 && !rp.started; frame++) {
        rp.Frame(none, 2);
        flush(frame);
    }
    for (int i = 0; i < 30; i++, frame++) {
        rp.Frame(none, 2);
        flush(frame);
    }
    const MtxFx43 &m = car.matrix;
    printf("settled frame=%d pos=%d,%d,%d up=%d wheels=%d start=%d,%d,%d%s", frame, m.m[3][0], m.m[3][1], m.m[3][2],
           m.m[1][1], car.groundedWheels, rp.respawnPos.x, rp.respawnPos.y, rp.respawnPos.z, "\n");
    auto cross = [&](const dsTrigger *tr) {
        const VecFx32 &c = tr->gate.centre, &n = tr->gate.normal;
        for (int k : {5, 1}) {
            car.matrix.m[3][0] = c.x - n.x * k;
            car.matrix.m[3][1] = c.y - n.y * k;
            car.matrix.m[3][2] = c.z - n.z * k;
            car.velocity = {n.x * 8, n.y * 8, n.z * 8};
            rp.AfterCarStep(2);
            flush(frame++);
        }
        printf("crossed %s%s", tr->name.c_str(), "\n");
    };
    const dsTrigger *finish = nullptr;
    std::vector<const dsTrigger *> cps;
    for (const dsTrigger *tr : rp.Triggers()) {
        if (tr->kind == dsTrigger::FINISH && !finish)
            finish = tr;
        if (tr->kind == dsTrigger::CHECKPOINT)
            cps.push_back(tr);
    }
    if (!finish)
        return 1;
    cross(finish); // too early
    for (int lap = 0; lap < rp.lapCount; lap++) {
        for (size_t i = 0; i < cps.size(); i++) {
            cross(cps[i]);
            if (lap == 0 && i == 0) {
                cross(cps[i]); // a second pass does not count
                rp.Reset(false);
                flush(frame++);
                printf("respawned pos=%d,%d,%d expected=%d,%d,%d%s", car.matrix.m[3][0], car.matrix.m[3][1],
                       car.matrix.m[3][2], rp.respawnPos.x, rp.respawnPos.y, rp.respawnPos.z, "\n");
            }
        }
        cross(finish);
    }
    printf("finished=%d time=%d%s", rp.finished, rp.finishTime, "\n");
    // Under the sea: 180 ticks later the car comes back to the start (nothing passed after the restart).
    rp.Reset(true);
    flush(frame++);
    while (!rp.started && frame < 2000) {
        rp.Frame(none, 2);
        flush(frame++);
    }
    int sunk = frame;
    for (int i = 0; i < 120; i++, frame++) {
        if (i < 100) {
            car.matrix.m[3][1] = car.minHeight - 0x3000;
            car.velocity = {};
        }
        rp.Frame(none, 2);
        flush(frame);
    }
    printf("sunk at frame %d%s", sunk, "\n");
    return 0;
}

// Chase camera behind the Coast car of a race: countdown (standing), 90 frames of throttle, then the brake held
// until the car has reversed faster than 2 m/s for 40 frames; one camera update per frame of two ticks. tools/host/check_camera.py.
static int Camera(dsTrack &t, const char *carName)
{
    dsCarParamsTable table;
    table.Load();
    if (table.All().find(carName) == table.All().end())
        return 1;
    const dsCarParams &prm = table.Get(carName);
    static dsCarModelGeometry geo;
    if (!geo.Load(carName, 0, prm))
        return 1;
    t.BuildTerrain();
    for (dsTrackPiece *pc : t.pieces)
        pc->CreateCollision();
    dsCarDynamics car(carName, &geo);
    car.LoadParams(prm);
    car.pieceRotationAt = [&t](const VecFx32 &p) { return t.PieceRotationAt(p); };
    g_dsFrameTime = 0x44;
    dsRaceProgress rp(&t, &car);
    rp.Reset(true);
    dsChaseCamera cam(1);
    if (!cam.Load(carName))
        return 1;
    cam.Reset();
    printf("camera Height=%d Distance=%d LookAtHeight=%d DistInc=%d DistDec=%d\n", cam.height, cam.distance,
           cam.lookAtHeight, cam.distInc, cam.distDec);
    int phase = 0, go = -1, reversing = 0;
    for (int f = 0; f < 600 && reversing < 40; f++) {
        dsCarInput in;
        if (rp.started && go < 0)
            go = f;
        if (go >= 0 && f < go + 90)
            in.accel = FX32_ONE;
        else if (go >= 0)
            in.brake = FX32_ONE;
        rp.Frame(in, 2);
        cam.Update(car);
        phase = !rp.started ? 0 : (f < go + 90 ? 1 : 2);
        if (phase == 2 && car.forwardSpeed < -2 * FX32_ONE)
            reversing++;
        const MtxFx43 &m = car.prevMatrix, &c = cam.matrix;
        printf("frame %d phase=%d fwdspeed=%d car=%d,%d,%d fwd=%d,%d,%d up=%d,%d,%d cam=%d,%d,%d r=%d,%d,%d"
               " u=%d,%d,%d f=%d,%d,%d rev=%d spring=%d\n", f, phase, car.forwardSpeed, m.m[3][0], m.m[3][1],
               m.m[3][2], m.m[2][0], m.m[2][1], m.m[2][2], m.m[1][0], m.m[1][1], m.m[1][2], c.m[3][0], c.m[3][1],
               c.m[3][2], c.m[0][0], c.m[0][1], c.m[0][2], c.m[1][0], c.m[1][1], c.m[1][2], c.m[2][0], c.m[2][1],
               c.m[2][2], cam.reverse, cam.distSpring.pos);
    }
    return 0;
}

int main(int argc, char **argv)
{
    const char *root = "extract/files", *track = "easy/0", *envName = "coa";
    bool raw = false, raycast = false, engine = false, drop = false, progress = false, camera = false;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--root") && i + 1 < argc)
            root = argv[++i];
        else if (!strcmp(argv[i], "--raw"))
            raw = true;
        else if (!strcmp(argv[i], "--track") && i + 1 < argc)
            track = argv[++i];
        else if (!strcmp(argv[i], "--env") && i + 1 < argc)
            envName = argv[++i];
        else if (!strcmp(argv[i], "--raycast"))
            raycast = true;
        else if (!strcmp(argv[i], "--engine"))
            engine = true;
        else if (!strcmp(argv[i], "--drop"))
            drop = true;
        else if (!strcmp(argv[i], "--progress"))
            progress = true;
        else if (!strcmp(argv[i], "--camera"))
            camera = true;
        else {
            fprintf(stderr, "usage: tm_physics --root <dir> [--raw] [--env coa] [--track easy/0] --raycast | --engine"
                            " | --drop | --progress | --camera\n");
            return 2;
        }
    }
    FX_Init();
    FSh_SetRoot(root);
    dsFileSystem::Create();
    dsFileSystem::Get()->EnableVfs(!raw);
    if (engine)
        return EngineDump();
    int env = -1;
    for (int e = 0; e < ENV_COUNT; e++)
        if (!strcmp(envName, dsRaceSetup::EnvironmentName(e)))
            env = e;
    if (env < 0) {
        fprintf(stderr, "unknown environment %s\n", envName);
        return 2;
    }
    dsRaceSetup *setup = dsRaceSetup::Get();
    setup->mode = 1;
    setup->SetEnvironment(env);
    dsPieceDataTable::Get()->Load(env);
    dsTrack t;
    std::string scene = std::string(envName) + "/" + track;
    if (!t.LoadScene(scene.c_str(), true)) {
        fprintf(stderr, "cannot load scene %s\n", scene.c_str());
        return 1;
    }
    if (raycast)
        return Raycast(t);
    if (drop)
        return Drop(t, envName);
    if (progress)
        return Progress(t, envName);
    if (camera)
        return Camera(t, envName);
    return 0;
}
