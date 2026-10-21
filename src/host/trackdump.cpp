// Host test driver for the second vertical slice: track loading.
//   tm_trackdump --root <dir> [--raw] [--env coa] [--track <difficulty>/<n>] [--pieces] [--collision] [--lighting] [--terrain] [--triggers]
// Loads PieceData/<env>/PieceData like the race loader, then the scene <env>/<difficulty>/<n> (default easy/0)
// like ov003 0x0211b404, and prints the piece catalogue, the track header, every placed piece and, with
// --collision, the collision mesh of every model the track uses. tools/host/check_track.py checks the output.
#include <ctype.h>
#include <stdio.h>
#include <string.h>

#include <map>
#include <set>
#include <string>

#include "collision/dsCollisionMesh.h"
#include "fs/dsFileSystem.h"
#include "nitro.h"
#include "track/dsPieceData.h"
#include "track/dsRaceSetup.h"
#include "track/dsTrack.h"
#include "race/dsRaceProgress.h"
#include "track/dsTrackLighting.h"
#include "editor/dsEditorTrack.h"

static int EnvIndex(const char *name)
{
    for (int i = 0; i < ENV_COUNT; i++)
        if (strcmp(dsRaceSetup::EnvironmentName(i), name) == 0)
            return i;
    return -1;
}

static void DumpPieces(dsPieceDataTable *table)
{
    printf("piecedata %d\n", (int)table->All().size());
    for (const dsPieceInfo *p : table->All()) {
        printf("piece %u %s type=%u size=%u,%u,%u cost=%u,%u ground=%s air=%s helper=%s flags=%d%d%d%d%d%d%d%d%d%d"
               " initDir=%d,%d,%d posGround=%d,%d,%d posAir=%d,%d,%d trigger=%d,%d,%d notch=",
               p->id, p->name.empty() ? "-" : p->name.c_str(), p->type, p->width, p->height, p->depth, p->costMem, p->costPoly,
               p->ground.empty() ? "-" : p->ground.c_str(), p->air.empty() ? "-" : p->air.c_str(),
               p->helper.empty() ? "-" : p->helper.c_str(), p->hasGround, p->hasAir, p->hasNotches,
               p->belowGround, p->cullable, p->isPillar, p->hasPillars, p->designerTPV, p->looseTPV,
               p->disableNotch, p->initDir[0], p->initDir[1], p->initDir[2], p->initPosGround.x,
               p->initPosGround.y, p->initPosGround.z, p->initPosAir.x, p->initPosAir.y, p->initPosAir.z,
               p->triggerSize.x, p->triggerSize.y, p->triggerSize.z);
        for (u8 v : p->notchData)
            printf("%u.", v);
        printf(" columns=");
        for (u8 v : p->columnNotches)
            printf("%u.", v);
        printf("\n");
    }
}

static void DumpMesh(const std::string &model)
{
    dsCollisionMesh mesh;
    if (!mesh.Load(model.c_str())) {
        printf("mesh %s missing\n", model.c_str());
        return;
    }
    u32 sum = 0;
    for (fx16 n : mesh.normals)
        sum = sum * 31 + (u16)n;
    u32 lists = 0;
    for (u32 x = 0; x < mesh.grid.dim[0]; x++)
        for (u32 y = 0; y < mesh.grid.dim[1]; y++)
            for (u32 z = 0; z < mesh.grid.dim[2]; z++) {
                std::vector<u32> tris;
                u32 c[3] = {x, y, z};
                mesh.GatherCell(c, tris);
                for (u32 t : tris)
                    lists = lists * 31 + t + 1;
                lists = lists * 31;
            }
    printf("mesh %s verts=%d tris=%d materials=%d grid=%u,%u,%u min=%d,%d,%d size=%d,%d,%d cell=%d,%d,%d"
           " normals=%08x cells=%08x\n",
           model.c_str(), (int)mesh.verts.size(), (int)mesh.tris.size(), (int)mesh.materials.size(),
           mesh.grid.dim[0], mesh.grid.dim[1], mesh.grid.dim[2], mesh.grid.min[0], mesh.grid.min[1],
           mesh.grid.min[2], mesh.grid.size[0], mesh.grid.size[1], mesh.grid.size[2], mesh.grid.cellSize[0],
           mesh.grid.cellSize[1], mesh.grid.cellSize[2], sum, lists);
}

// Race triggers of the scene (ov003 0x02111280 / 0x021114e4), the start point (0x02114a6c), and a grid of probes
// through every gate: a point one unit behind the plane, then one unit in front, at local (u, v) on the rectangle.
static void DumpTriggers(dsTrack &t)
{
    dsRaceProgress rp(&t, nullptr);
    rp.CreateTriggers();
    VecFx32 sp, sd;
    rp.StartPoint(rp.startPiece, &sp, &sd);
    int si = -1;
    for (size_t i = 0; i < t.pieces.size(); i++)
        if (t.pieces[i] == rp.startPiece)
            si = (int)i;
    printf("race checkpoints=%d laps=%d start=%d startpos=%d,%d,%d startdir=%d,%d,%d\n", rp.checkpointCount,
           rp.lapCount, si, sp.x, sp.y, sp.z, sd.x, sd.y, sd.z);
    static const int uv[] = {-2662, -2048, -1229, 0, 1229, 2048, 2662}; // -0.65 -0.5 -0.3 0 0.3 0.5 0.65
    for (size_t k = 0; k < rp.Triggers().size(); k++) {
        dsTrigger *tr = rp.Triggers()[k];
        const MtxFx43 &m = tr->matrix;
        const dsTriggerGate &g = tr->gate;
        int order = tr->kind == dsTrigger::CHECKPOINT ? rp.checkpointOrder[tr->pieceIndex] : -1;
        printf("trigger %d %s piece=%d order=%d centre=%d,%d,%d normal=%d,%d,%d d=%d radius=%d mtx=", (int)k,
               tr->name.c_str(), tr->pieceIndex, order, g.centre.x, g.centre.y, g.centre.z, g.normal.x, g.normal.y,
               g.normal.z, g.d, g.radius);
        for (int r = 0; r < 4; r++)
            printf("%d,%d,%d%s", m.m[r][0], m.m[r][1], m.m[r][2], r < 3 ? ";" : "\n");
        for (int u : uv)
            for (int v : uv) {
                VecFx32 p;
                for (int c = 0; c < 3; c++)
                    (&p.x)[c] = (fx32)(((s64)u * m.m[0][c] + (s64)v * m.m[1][c]) >> 12) + m.m[3][c];
                VecFx32 a = {p.x - g.normal.x, p.y - g.normal.y, p.z - g.normal.z};
                VecFx32 b = {p.x + g.normal.x, p.y + g.normal.y, p.z + g.normal.z};
                dsTriggerGate gg = g;
                gg.ResetSide(0, a);
                bool fwd = gg.Test(b, 0);
                bool back = gg.Test(a, 0);
                bool again = gg.Test(a, 0);
                printf("probe %d u=%d v=%d fwd=%d back=%d again=%d\n", (int)k, u, v, fwd, back, again);
            }
    }
}

// --editor: the track editor's model on every race and puzzle track: bounds, overlapping notch masks, validation,
// the bit-packed format's round trip, budgets; and every block menu entry resolves to a piece.
static int EditorCheck()
{
    static const char *const diffs[] = {"practice", "easy", "medium", "hard", "extreme", "bonus"};
    static const int counts[] = {5, 5, 5, 5, 1, 5};
    int errors = 0;
    for (int env = 0; env < ENV_COUNT; env++) {
        dsRaceSetup::Get()->mode = 6;
        dsEditorTrack ed(env);
        dsBlockMenu menu;
        std::string layout = std::string("screens/") + (char)toupper(dsRaceSetup::EnvironmentName(env)[0]) +
                             (dsRaceSetup::EnvironmentName(env) + 1) + "/TrackPieceLayout";
        if (!menu.Load(layout.c_str())) {
            printf("ERROR no layout %s\n", layout.c_str());
            errors++;
        }
        int blocks = 0;
        for (auto &f : menu.folders)
            for (const dsBlockMenuEntry &b : f.second)
                if (!b.folder) {
                    blocks++;
                    if (!b.id) {
                        printf("ERROR %s: block %s not in PieceData\n", layout.c_str(), b.name.c_str());
                        errors++;
                    }
                } else if (!menu.Folder(b.name)) {
                    printf("ERROR %s: folder %s missing\n", layout.c_str(), b.name.c_str());
                    errors++;
                }
        printf("menu %s folders=%d blocks=%d grid=%u,%u,%u border=%d\n", dsRaceSetup::EnvironmentName(env),
               (int)menu.folders.size(), blocks, ed.gridSize[0], ed.gridSize[1], ed.gridSize[2], ed.border);
        for (int d = 0; d < 6; d++)
            for (int n = 0; n < counts[d]; n++) {
                char scene[64];
                snprintf(scene, sizeof scene, "%s/%s/%d", dsRaceSetup::EnvironmentName(env), diffs[d], n);
                dsRaceSetup::Get()->mode = 6;
                dsEditorTrack t(env);
                if (!t.LoadScene(scene, false)) {
                    printf("ERROR cannot load %s\n", scene);
                    errors++;
                    continue;
                }
                int out = 0, shared = 0, own = 0;
                for (dsTrackPiece *p : t.track.pieces) {
                    if (p->autoTerrain || !p->info)
                        continue;
                    own++;
                    if (!t.InBounds(*p->info, p->rotation, p->cell[0], p->cell[1], p->cell[2]))
                        out++;
                    std::vector<dsTrackPiece *> ov;
                    bool locked;
                    t.Overlaps(*p->info, p->rotation, p->cell[0], p->cell[1], p->cell[2], &ov, &locked);
                    for (dsTrackPiece *o : ov)
                        shared += o != p && !o->autoTerrain;
                }
                dsEditorCheck check = t.Validate();
                std::vector<u8> packed = t.SavePacked();
                dsEditorTrack back(env);
                bool same = back.LoadPacked(packed) && back.track.pieces.size() == t.track.pieces.size();
                for (size_t i = 0; same && i < t.track.pieces.size(); i++) {
                    const dsTrackPiece *a = t.track.pieces[i], *b = back.track.pieces[i];
                    same = a->id == b->id && a->rotation == b->rotation && a->pos.x == b->pos.x &&
                           a->pos.y == b->pos.y && a->pos.z == b->pos.z;
                }
                printf("track %s pieces=%d out=%d shared=%d check=%d laps=%d bytes=%d poly=%d mem=%d/%d roundtrip=%d\n",
                       scene, own, out, shared, (int)check, t.header.lapCount, (int)packed.size(), t.polyUsed,
                       t.memUsed, t.memLimit(), same);
                errors += out != 0 || check != CHECK_OK || !same;
            }
    }
    // Puzzles: Scenes/puz/<diff>/<n> (Stadium) and their block layouts.
    static const char *const pdiffs[] = {"practice", "easy", "medium", "hard", "extreme"};
    static const int pcounts[] = {4, 4, 4, 4, 1};
    for (int d = 0; d < 5; d++)
        for (int n = 0; n < pcounts[d]; n++) {
            char scene[64], lay[64];
            snprintf(scene, sizeof scene, "puz/%s/%d", pdiffs[d], n);
            snprintf(lay, sizeof lay, "puz/%s/%d-Layout", pdiffs[d], n);
            dsTrack h;
            dsRaceSetup::Get()->mode = 7;
            if (!h.LoadScene(scene, false)) {
                printf("ERROR cannot read %s\n", scene);
                errors++;
                continue;
            }
            dsEditorTrack t(h.info.environment);
            t.puzzle = true;
            t.LoadScene(scene, true);
            dsBlockMenu menu;
            menu.Load(lay);
            int blocks = 0, missing = 0;
            if (const std::vector<dsBlockMenuEntry> *top = menu.Folder("TopFolder"))
                for (const dsBlockMenuEntry &b : *top) {
                    blocks += b.count;
                    missing += !b.id;
                }
            dsEditorCheck check = t.Validate();
            printf("puzzle %s env=%d pieces=%d check=%d blocks=%d missing=%d\n", scene, h.info.environment,
                   (int)t.track.pieces.size(), (int)check, blocks, missing);
            errors += missing != 0;
        }
    // Free draw: a closed loop (4 corners, straights between), a stroke across it (two crosses), one ending on its
    // side (a T); every
    // road block's sides (its connection table entry two quarter turns on) must meet a neighbour's opposite side.
    static const u8 kMask[4][4] = {{5, 12, 13, 15}, {10, 6, 14, 15}, {5, 3, 7, 15}, {10, 9, 11, 15}};
    for (int env = 0; env < ENV_COUNT; env++) {
        dsRaceSetup::Get()->mode = 6;
        dsEditorTrack t(env);
        t.FreeDrawBegin();
        s32 y = dsPieceDataTable::Get()->GroundLevel();
        const int loop[][2] = {{5, 5}, {6, 5}, {7, 5}, {8, 5}, {8, 6}, {8, 7}, {8, 8}, {7, 8}, {6, 8}, {5, 8},
                               {5, 7}, {5, 6}, {5, 5}};
        for (const auto &c : loop)
            t.FreeDrawStep(c[0], y, c[1]);
        t.FreeDrawBegin();
        for (int z = 4; z <= 9; z++)
            t.FreeDrawStep(7, y, z);
        t.FreeDrawBegin();
        for (int x = 11; x >= 8; x--)
            t.FreeDrawStep(x, y, 6);
        int roads = 0, bad = 0, kinds[4] = {};
        for (const dsTrackPiece *p : t.track.pieces) {
            int k = -1;
            for (int i = 0; i < 4; i++)
                if ((u32)p->id == t.roadIds[i])
                    k = i;
            if (k < 0)
                continue;
            roads++;
            kinds[k]++;
            u8 sides = kMask[(p->rotation + 2) & 3][k];
            const struct {
                int dx, dz;
                u8 bit, back;
            } dirs[4] = {{0, 1, 1, 4}, {-1, 0, 2, 8}, {0, -1, 4, 1}, {1, 0, 8, 2}};
            for (const auto &d : dirs) {
                if (!(sides & d.bit))
                    continue;
                const dsTrackPiece *n = t.PieceAt(p->cell[0] + d.dx, p->cell[1], p->cell[2] + d.dz);
                int nk = -1;
                for (int i = 0; n && i < 4; i++)
                    if ((u32)n->id == t.roadIds[i])
                        nk = i;
                bool ends = (p->cell[0] == 7 && (p->cell[2] == 4 || p->cell[2] == 9)) ||
                            (p->cell[0] == 11 && p->cell[2] == 6); // the strokes' open ends
                if ((nk < 0 || !(kMask[(n->rotation + 2) & 3][nk] & d.back)) && !ends)
                    bad++;
            }
        }
        printf("freedraw %s: %d road blocks (straight %d, corner %d, T %d, cross %d), %d open sides\n",
               dsRaceSetup::EnvironmentName(env), roads, kinds[0], kinds[1], kinds[2], kinds[3], bad);
        errors += bad != 0 || kinds[1] != 4 || kinds[3] != 2 || kinds[2] != 1;
    }
    printf("editor check: %d errors\n", errors);
    return errors ? 1 : 0;
}

int main(int argc, char **argv)
{
    const char *root = "extract/files", *envName = "coa", *track = "easy/0";
    bool raw = false, editor = false, pieces = false, collision = false, lighting = false, terrain = false, triggers = false;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--root") && i + 1 < argc)
            root = argv[++i];
        else if (!strcmp(argv[i], "--raw"))
            raw = true;
        else if (!strcmp(argv[i], "--editor"))
            editor = true;
        else if (!strcmp(argv[i], "--env") && i + 1 < argc)
            envName = argv[++i];
        else if (!strcmp(argv[i], "--track") && i + 1 < argc)
            track = argv[++i];
        else if (!strcmp(argv[i], "--pieces"))
            pieces = true;
        else if (!strcmp(argv[i], "--collision"))
            collision = true;
        else if (!strcmp(argv[i], "--terrain"))
            terrain = true;
        else if (!strcmp(argv[i], "--triggers"))
            triggers = true;
        else if (!strcmp(argv[i], "--lighting"))
            lighting = true;
        else {
            fprintf(stderr, "usage: tm_trackdump --root <dir> [--raw] [--env coa] [--track easy/0] [--pieces]"
                            " [--collision] [--lighting]\n");
            return 2;
        }
    }
    int env = EnvIndex(envName);
    if (env < 0) {
        fprintf(stderr, "unknown environment %s\n", envName);
        return 2;
    }
    FX_Init();
    FSh_SetRoot(root);
    dsFileSystem::Create();
    dsFileSystem::Get()->EnableVfs(!raw);
    if (editor)
        return EditorCheck();

    dsRaceSetup *setup = dsRaceSetup::Get();
    setup->mode = 1;
    setup->SetEnvironment(env);
    dsPieceDataTable *table = dsPieceDataTable::Get();
    table->Load(env);
    if (pieces)
        DumpPieces(table);

    std::string scene = std::string(envName) + "/" + track;
    dsTrack t;
    if (!t.LoadScene(scene.c_str(), true)) {
        fprintf(stderr, "cannot load scene %s\n", scene.c_str());
        return 1;
    }
    u32 gx, gy, gz;
    setup->GridSize(&gx, &gy, &gz);
    printf("track %s env=%d tod=%d laps=%u author=%d gold=%d silver=%d bronze=%d name=%s grid=%u,%u,%u"
           " cell=%d,%d pieces=%d\n",
           scene.c_str(), t.info.environment, t.info.timeOfDay, t.info.lapCount, t.info.authorTime,
           t.info.goldTime, t.info.silverTime, t.info.bronzeTime, t.info.authorName, gx, gy, gz,
           setup->CellWidthFx(), setup->CellHeightFx(), (int)t.pieces.size());
    std::set<std::string> models;
    for (const dsTrackPiece *p : t.pieces) {
        printf("placed %d %s rot=%u type=%d air=%d cell=%d,%d,%d pos=%d,%d,%d model=%s mtx=", p->id,
               p->info ? p->info->name.c_str() : "?", p->rotation, p->type, p->air, p->cell[0], p->cell[1],
               p->cell[2], p->pos.x, p->pos.y, p->pos.z, p->modelPath.c_str());
        for (int r = 0; r < 4; r++)
            printf("%d,%d,%d%s", p->matrix.m[r][0], p->matrix.m[r][1], p->matrix.m[r][2], r < 3 ? ";" : "\n");
        models.insert(p->modelPath);
    }
    if (terrain) {
        size_t first = t.pieces.size();
        t.BuildTerrain();
        printf("terrain %d\n", (int)(t.pieces.size() - first));
        for (size_t i = first; i < t.pieces.size(); i++) {
            const dsTrackPiece *p = t.pieces[i];
            printf("terrainpiece %d %s rot=%u cell=%d,%d,%d pos=%d,%d,%d model=%s\n", p->id,
                   p->info ? p->info->name.c_str() : "?", p->rotation, p->cell[0], p->cell[1], p->cell[2], p->pos.x,
                   p->pos.y, p->pos.z, p->modelPath.c_str());
        }
        // Ground-level map (x across, z down): '.' empty, 't' terrain piece, '#' placed piece, 'a' empty with a
        // piece above it.
        printf("cellmap level 3\n");
        for (s32 z = 0; z < 22; z++) {
            for (s32 x = 0; x < 22; x++) {
                const dsTrackPiece *p = t.PieceAtCell(x, 3, z);
                char c = p ? (p->autoTerrain ? 't' : '#') : '.';
                for (s32 y = 4; c == '.' && y < 12; y++)
                    if (t.PieceAtCell(x, y, z))
                        c = 'a';
                putchar(c);
            }
            putchar('\n');
        }
    }
    if (triggers)
        DumpTriggers(t);
    if (collision)
        for (const std::string &m : models)
            DumpMesh(m);
    if (lighting) {
        dsTrackLighting l;
        static const char *const tods[3] = {"rise", "day", "set"};
        for (const char *tod : tods) {
            if (!l.Load(envName, tod)) {
                printf("lighting %s_%s missing\n", envName, tod);
                continue;
            }
            printf("lighting %s_%s", envName, tod);
            for (const VecFx32 &c : l.colors)
                printf(" %d,%d,%d", c.x, c.y, c.z);
            printf(" v24=%d v25=%d\n", l.value24, l.value25);
        }
    }
    return 0;
}
