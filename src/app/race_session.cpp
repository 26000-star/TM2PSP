// Race session shared by the host and PSP race drivers (see race_session.h).
#include "race_session.h"

#include <stdio.h>
#include <string.h>

#include <map>
#include <string>

#include "collision/dsCollisionWorld.h"
#include "core/dsTime.h"
#include "fs/dsFileSystem.h"
#include "math/dsMath.h"
#include "nitro.h"
#include "track/dsPieceData.h"
#include "track/dsRaceSetup.h"

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

RaceSession::~RaceSession()
{
    pieces.clear();
    models.clear(); // G3Model's destructor frees the back-end textures and vertex arrays
}

void RaceUnloadWorld() { dsCollisionWorld::Get()->Clear(); }

bool RaceSession::Load(const char *trackName)
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
    for (dsChaseCamera &c : cams) {
        if (!c.Load("coa"))
            fprintf(stderr, "config/cameras: no coa entry\n");
        c.Reset();
    }

    auto own = [this](G3Model *m) {
        if (m)
            models.emplace_back(m);
        return m;
    };
    // material.bin replaces the batch's texture list: upload once, after it (an upload before leaked the first
    // list's back-end textures on every track load).
    batch = own(RaceLoadModel("Tracks/coa/texturebatch", false));
    std::vector<u8> mb;
    if (batch) {
        if (RaceReadFile("graphics/Tracks/coa/material.bin", &mb))
            batch->LoadMaterialBin(mb);
        batch->Upload();
    }
    std::map<std::string, G3Model *> cache;
    for (const dsTrackPiece *p : track.pieces) {
        auto it = cache.find(p->modelPath);
        if (it == cache.end())
            it = cache.emplace(p->modelPath, own(RaceLoadModel(p->modelPath))).first;
        if (it->second)
            pieces.push_back({it->second, p});
    }
    // lod1: the body at the physics scale (lod0 adds the wheel, shadow and light-cone shapes that the game
    // places itself in the visual update 0x02126d10, not ported yet).
    carModel = own(RaceLoadModel("Cars/coa/0/lod1"));
    printf("track %s: %d pieces drawn, batch %s, car model %s\n", trackName, (int)pieces.size(),
           batch ? "ok" : "missing", carModel ? "ok" : "missing");
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
    cams[camIndex].Update(*car); // camera vtable +0x1c, once per frame
}

// ---- HUD stand-in: a 5 x 7 bitmap font (the game's HUD sprites are not ported) ----
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
    fx32 t = rp.finished ? rp.finishTime : rp.clock;
    int cs = (int)(((s64)(t & 0xfff) * 100) >> 12), sec = t >> 12;
    char buf[64];
    if (!rp.countdown) {
        snprintf(buf, sizeof buf, "%d:%02d.%02d", sec / 60, sec % 60, cs);
        text(w - (6 * 4 * 9 + 12) * s, 12 * s, 4 * s, buf, 1, 1, 1, user);
    }
    int cps = rp.checkpointCount - (rp.startPiece && rp.startPiece->type == PIECE_MULTILAP ? 1 : 0);
    int passed = 0;
    for (const dsTrigger *tr : rp.Triggers())
        if (tr->kind == dsTrigger::CHECKPOINT && !tr->pending)
            passed++;
    snprintf(buf, sizeof buf, "CP %d/%d", passed, cps);
    text(12 * s, 12 * s, 4 * s, buf, 1, 0.9f, 0.3f, user);
    if (rp.lapCount > 1) {
        snprintf(buf, sizeof buf, "TOUR %d/%d", rp.finished ? rp.lapCount : rp.lapsDone + 1, rp.lapCount);
        text(12 * s, 50 * s, 4 * s, buf, 1, 0.9f, 0.3f, user);
    }
    const dsRaceEvent *last = rp.events.empty() ? nullptr : &rp.events.back();
    if (rp.countdown) {
        snprintf(buf, sizeof buf, "%d", sec + 1);
        text(w / 2 - 6 * 12 / 2 * s, h / 3, 12 * s, buf, sec >= 1 ? 1.0f : 1.0f, sec >= 1 ? 0.25f : 0.8f, 0.2f, user);
    } else if (rp.finished) {
        text(w / 2 - 6 * 8 * 8 / 2 * s, h / 3, 8 * s, "ARRIVEE!", 0.3f, 1, 0.3f, user);
    } else if (last && rp.clock - last->time < 0x2000) {
        if (last->type == dsRaceEvent::GO)
            text(w / 2 - 6 * 12 * 3 / 2 * s, h / 3, 12 * s, "GO!", 0.3f, 1, 0.3f, user);
        else if (last->type == dsRaceEvent::MISSED) {
            snprintf(buf, sizeof buf, "%d CP MANQUE%s", last->value, last->value > 1 ? "S" : "");
            text(w / 2 - 6 * 5 * (int)strlen(buf) / 2 * s, h / 3, 5 * s, buf, 1, 0.3f, 0.3f, user);
        } else if (last->type == dsRaceEvent::CHECKPOINT || last->type == dsRaceEvent::LAP) {
            int ls = last->time >> 12, lc = (int)(((s64)(last->time & 0xfff) * 100) >> 12);
            snprintf(buf, sizeof buf, "%d:%02d.%02d", ls / 60, ls % 60, lc);
            text(w / 2 - 6 * 6 * (int)strlen(buf) / 2 * s, h / 3, 6 * s, buf, 0.4f, 0.8f, 1, user);
        }
    }
}
