#include "race/dsRaceProgress.h"

#include <stdio.h>

#include "car/dsCarDynamics.h"
#include "collision/dsCollisionWorld.h"
#include "core/dsTime.h"
#include "math/dsMath.h"
#include "track/dsPieceData.h"
#include "track/dsRaceSetup.h"
#include "track/dsTrack.h"

dsRaceProgress::dsRaceProgress(dsTrack *track, dsCarDynamics *car) : m_track(track), m_car(car)
{
    lapCount = track->info.lapCount;
}

dsRaceProgress::~dsRaceProgress()
{
    for (dsTrigger *t : m_triggers)
        delete t;
}

int dsRaceProgress::PieceIndex(const dsTrackPiece *p) const
{
    for (size_t i = 0; i < m_track->pieces.size(); i++)
        if (m_track->pieces[i] == p)
            return (int)i;
    return 0; // main 0x0208d38c returns 0 when absent
}

// The pieces are visited in the order of the track's piece list (the grid's piece slots, +0x10): finish and
// multilap pieces get a "finish" trigger (main 0x0208cd28 collects them), checkpoints a "cp<n>" trigger numbered in
// that order; the start piece (main 0x0208cdcc: first start or multilap piece) of a lap track takes the next
// checkpoint number too, so checkpointCount counts it.
// @port ov003:021213f8 partial (deletes every trigger; the list bookkeeping is a std::vector)
// @port ov003:02111280 done
// @port ov003:021114e4 partial (the Stadium piece 0x10 turn is not applied: Coast only)
// @port main:0208cd28 done / main:0208cdcc done / main:0208d38c done
void dsRaceProgress::CreateTriggers()
{
    for (dsTrigger *t : m_triggers)
        delete t;
    m_triggers.clear();
    const std::vector<dsTrackPiece *> &ps = m_track->pieces;
    for (size_t i = 0; i < ps.size(); i++)
        if (ps[i]->info && (ps[i]->type == PIECE_FINISH || ps[i]->type == PIECE_MULTILAP))
            m_triggers.push_back(new dsTrigger(dsTrigger::FINISH, "finish", dsTrigger::PieceMatrix(*ps[i], *ps[i]->info),
                                               (int)i));
    checkpointCount = 0;
    checkpointOrder.assign(ps.size(), -1);
    for (size_t i = 0; i < ps.size(); i++) {
        if (!ps[i]->info || ps[i]->type != PIECE_CHECKPOINT)
            continue;
        checkpointOrder[i] = checkpointCount++;
        char name[20];
        snprintf(name, sizeof name, "cp%d", checkpointCount - 1);
        m_triggers.push_back(new dsTrigger(dsTrigger::CHECKPOINT, name, dsTrigger::PieceMatrix(*ps[i], *ps[i]->info),
                                           (int)i));
    }
    startPiece = nullptr;
    for (const dsTrackPiece *p : ps)
        if (!startPiece && (p->type == PIECE_START || p->type == PIECE_MULTILAP))
            startPiece = p;
    if (startPiece && startPiece->type == PIECE_MULTILAP)
        checkpointOrder[PieceIndex(startPiece)] = checkpointCount++;
}

// Ground probe from 2 units above pos, 10 units long (world vtable +0x20, masks 1/0); the direction components
// are snapped to -4095 / 0 / 4096 around +-0.4 (exactly -1637 and 1638 are kept as they are).
// @port ov003:0211c184 partial (the camera/respawn matrix holder *0x027e0e58 +0xac; the scene part 0x0211e608 is
// not ported; the nearest hit of the batch query stands in for the single-segment query)
MtxFx43 dsRaceProgress::PlacementMatrix(const VecFx32 &pos, const VecFx32 &dir) const
{
    VecFx32 p = pos;
    std::vector<dsCollisionHit> hits;
    dsCollisionWorld *w = dsCollisionWorld::Get();
    w->SetMasks(1, 0);
    w->AddSegment({pos.x, pos.y + 0x2000, pos.z}, {0, (fx32)0xfffff001, 0}, 0xa000, &hits);
    w->TestSegments();
    if (!hits.empty()) {
        size_t b = 0;
        for (size_t i = 1; i < hits.size(); i++)
            if (hits[i].t < hits[b].t)
                b = i;
        p = hits[b].point;
    }
    auto snap = [](fx32 v) {
        fx32 r = v;
        if (-1637 < v && v < 1638)
            r = 0;
        if (1638 < v)
            r = FX32_ONE;
        return -1637 <= v ? r : (fx32)0xfffff001;
    };
    VecFx32 f = {snap(dir.x), snap(dir.y), snap(dir.z)};
    VecFx32 up = {0, FX32_ONE, 0};
    MtxFx43 m;
    dsMtx43_FromAxes(&m, &p, &f, &up);
    return m;
}

// Piece position + InitPos (ground or air) and InitDir, both turned by the piece rotation; on Coast a 2-cell wide
// piece turned 2 or 3 quarters moves 16 units along x first (Island: 32 along x, and along z for 2-cell deep pieces
// turned 0 or 3).
// @port ov003:02114a6c partial (the camera reset 0x0211c184 is PlacementMatrix)
void dsRaceProgress::StartPoint(const dsTrackPiece *piece, VecFx32 *pos, VecFx32 *dir) const
{
    VecFx32 d = {0, 0, (fx32)0xfffff001}, o = {};
    int rot = 0;
    if (piece && piece->info) {
        const dsPieceInfo *in = piece->info;
        d = {in->initDir[0], in->initDir[1], in->initDir[2]};
        o = piece->air ? in->initPosAir : in->initPosGround;
        rot = piece->rotation;
        s32 env = dsRaceSetup::Get()->environment;
        if (env == ENV_ISLAND) {
            if (in->width == 2 && (u8)(rot - 2) < 2)
                o.x += 0x20000;
            if (in->depth == 2 && (rot == 0 || rot == 3))
                o.z += 0x20000;
        } else if (env == ENV_COAST && in->width == 2 && (u8)(rot - 2) < 2) {
            o.x += 0x10000;
        }
    }
    MtxFx43 r;
    MTX_RotY43_(&r, dsSin(rot * 6434), dsCos(rot * 6434));
    dsVec_MulMtx33(&d, &r, &d);
    dsVec_MulMtx33(&o, &r, &o);
    VecFx32 base = piece ? piece->pos : VecFx32{};
    *pos = {base.x + o.x, base.y + o.y, base.z + o.z};
    *dir = d;
}

// Player reset: the car takes the placement matrix, the dynamics are reset, and every trigger records on which side
// of its plane the car stands so the respawn itself is no crossing.
// @port ov003:0210f3b4 partial (car, flags and triggers; camera, sounds and ghosts are not ported)
// @port ov003:0210e124 partial / ov003:02121ac8 done
void dsRaceProgress::PlaceCar(const MtxFx43 &m)
{
    m_car->Reset();
    m_car->Place(m, false, true);
    VecFx32 p = {m.m[3][0], m.m[3][1], m.m[3][2]};
    for (dsTrigger *t : m_triggers)
        t->ResetPlayer(0, p);
    respawnRequest = false;
    m_car->respawnRequest = false;
}

// @port ov003:02115af4 partial (HUD, camera, sounds and the network/replay modes are left out)
void dsRaceProgress::Reset(bool full)
{
    if (finished)
        full = true;
    if (!full && checkpointsPassed == 0)
        full = true;
    if (full) {
        started = false;
        countdown = true;               // +0x14: race+0x140 == 0 (local race)
        clock = 0x3000;                 // 3 s countdown (0x02104a6c: 0x19a when *0x020d6a70 is set)
        clockDown = true;
        clockRunning = true;
    } else {
        started = true;                 // 0x0210ffb4: players keep driving
    }
    finished = false;
    leftStart = false;
    sinking = false;
    sinkTicks = 180;
    if (!full) {
        PlaceCar(PlacementMatrix(respawnPos, respawnDir));
        Log(dsRaceEvent::RESPAWN, checkpointsPassed);
        return;
    }
    checkpointsPassed = 0;
    lapsLeft = lapCount;
    lapsDone = 0;
    finishTime = -1;
    CreateTriggers();
    currentPiece = nullptr;
    VecFx32 pos, dir;
    StartPoint(startPiece, &pos, &dir);
    respawnPos = pos;
    respawnDir = dir;
    respawnPiece = PieceIndex(startPiece);
    respawnPieceId = startPiece ? startPiece->id : 0;
    PlaceCar(PlacementMatrix(pos, dir));
    Log(dsRaceEvent::RESTART, 0);
}

// The respawn point becomes the piece's start point (Coast: pieces 0x3c and 0xe8 also add 4 units of height and,
// turned 2 or 3 quarters, 16 along x; other 2-cell wide pieces with an x offset as in StartPoint).
// @port ov003:02112f8c partial (HUD split display 0x021049b0 not ported) / ov003:0211527c partial (split times
// 0x0210e9fc not ported)
void dsRaceProgress::OnCheckpoint(int pieceIndex)
{
    const dsTrackPiece *pc = m_track->pieces[pieceIndex];
    checkpointsPassed++;
    s32 env = dsRaceSetup::Get()->environment;
    if (env != ENV_STADIUM || (pc->id != 0x10 && pc->id != 0x11)) {
        respawnPieceId = pc->id;
        respawnPiece = pieceIndex;
        const dsPieceInfo *in = pc->info;
        VecFx32 d = {in->initDir[0], in->initDir[1], in->initDir[2]};
        VecFx32 o = pc->air ? in->initPosAir : in->initPosGround;
        bool turned = (u8)(pc->rotation - 2) < 2;
        if (env == ENV_COAST) {
            if (pc->id == 0xe8 || pc->id == 0x3c) {
                if (turned)
                    o.x += 0x10000;
                o.y += 0x4000;
            } else if (o.x != 0 && in->width == 2 && turned) {
                o.x += 0x10000;
            }
        }
        MtxFx43 r;
        MTX_RotY43_(&r, dsSin(pc->rotation * 6434), dsCos(pc->rotation * 6434));
        dsVec_MulMtx33(&d, &r, &d);
        dsVec_MulMtx33(&o, &r, &o);
        respawnDir = d;
        respawnPos = {pc->pos.x + o.x, pc->pos.y + o.y, pc->pos.z + o.z};
    }
    Log(dsRaceEvent::CHECKPOINT, checkpointOrder[pieceIndex]);
}

// Finish gate crossed. A finish track needs every checkpoint; a lap track needs the car to have left the start
// piece and every "cp<n>" trigger passed, then counts a lap (the multilap piece acts as a checkpoint: new respawn
// point) and re-arms the checkpoints.
// @port ov003:02112f50 done / ov003:02115704 done (HUD calls 0x02104620 / 0x02104670 become events)
void dsRaceProgress::OnFinish()
{
    if (!startPiece)
        return;
    if (startPiece->type != PIECE_MULTILAP) {
        if (checkpointsPassed != checkpointCount) {
            Log(dsRaceEvent::MISSED, checkpointCount - checkpointsPassed);
            return;
        }
        finished = true;
        clockRunning = false;
        finishTime = clock;
        Log(dsRaceEvent::FINISH, lapsDone);
        return;
    }
    if (!leftStart)
        return;
    int missed = 0;
    for (dsTrigger *t : m_triggers)
        if (t->kind == dsTrigger::CHECKPOINT && t->pending)
            missed++;
    if (missed) {
        if (!finished)
            Log(dsRaceEvent::MISSED, missed);
        return;
    }
    lapsLeft--;
    if (lapsLeft == 0) {
        finished = true;
        clockRunning = false;
        lapsDone = lapCount;
        finishTime = clock;
        Log(dsRaceEvent::FINISH, lapsDone);
    } else {
        OnCheckpoint(PieceIndex(startPiece));
        lapsDone = lapCount - lapsLeft;
        Log(dsRaceEvent::LAP, lapsDone);
    }
    for (dsTrigger *t : m_triggers)
        if (t->kind == dsTrigger::CHECKPOINT)
            t->pending = true;
}

const dsTrackPiece *dsRaceProgress::GridPiece(s32 x, s32 y, s32 z) const
{
    u32 gx, gy, gz;
    dsRaceSetup::Get()->GridSize(&gx, &gy, &gz);
    if (x < 0 || x >= (s32)gx || y < 0 || y >= (s32)gy || z < 0 || z >= (s32)gz)
        return nullptr;
    return m_track->PieceAtCell(x, y, z);
}

// Cell of a coordinate: FX_Div by the cell size rounded to the nearest integer (half away from zero).
static s32 RoundCell(fx32 v, fx32 cell)
{
    fx32 q = cell == 0 ? 0 : FX_Div(v, cell);
    if (q < 0)
        return ((q - 2047) >> 12) + 1;
    return q == 0 ? 0 : (q + 0x800) >> 12;
}

// Piece under the car: the car's cell, then the cells above and below, then the cell 7 units ahead along the
// velocity (and the one below it) when that is another column.
// @port ov003:02112360 partial (the editor modes 6/7 and the piece highlight 0x02112b5c are left out; the grid
// byte lookup is dsTrack::PieceAtCell)
void dsRaceProgress::UpdateCurrentPiece()
{
    const MtxFx43 &m = m_car->matrix;
    dsRaceSetup *s = dsRaceSetup::Get();
    fx32 cw = s->CellWidthFx(), ch = s->CellHeightFx();
    VecFx32 p = {m.m[3][0], m.m[3][1], m.m[3][2]};
    s32 x = RoundCell(p.x, cw), y = RoundCell(p.y, ch), z = RoundCell(p.z, cw);
    const dsTrackPiece *pc = GridPiece(x, y, z);
    if (pc) {
        currentPiece = pc;
        return;
    }
    currentPiece = nullptr;
    if ((pc = GridPiece(x, y + 1, z)) != nullptr)
        currentPiece = pc;
    if ((pc = GridPiece(x, y - 1, z)) != nullptr && pc != currentPiece)
        currentPiece = pc;
    if (currentPiece)
        return;
    VecFx32 v = m_car->velocity;
    if (v.x == 0 && v.y == 0 && v.z == 0)
        v = {0, 0, FX32_ONE};
    else
        dsVec_Normalize(&v, &v);
    auto ahead = [](fx32 a, fx32 u) { return a + (fx32)(((s64)(u32)u * 0x7000 + 0x800) >> 12); };
    s32 ax = RoundCell(ahead(p.x, v.x), cw), ay = RoundCell(ahead(p.y, v.y), ch), az = RoundCell(ahead(p.z, v.z), cw);
    if (x == ax && z == az)
        return;
    if ((pc = GridPiece(ax, ay, az)) != nullptr)
        currentPiece = pc;
    if (currentPiece)
        return;
    if ((pc = GridPiece(ax, ay - 1, az)) != nullptr)
        currentPiece = pc;
}

VecFx32 dsRaceProgress::TestPoint() const
{
    VecFx32 v = m_car->velocity;
    if (v.x == 0 && v.y == 0 && v.z == 0)
        v = {0, 0, FX32_ONE};
    else
        dsVec_Normalize(&v, &v);
    const MtxFx43 &m = m_car->matrix;
    // (u64)u * 0x3000: the original multiplies the unsigned components (umull), so a negative component adds
    // 3 * 2^32 >> 12 = 3 << 20 to the high word; only the low 32 bits are kept, which cancels it.
    auto add = [](fx32 a, fx32 u) { return a + (fx32)(((u64)(u32)u * 0x3000 + 0x800) >> 12); };
    return {add(m.m[3][0], v.x), add(m.m[3][1], v.y), add(m.m[3][2], v.z)};
}

// @port ov003:02113048 partial (pause, network, replay, ghosts, sounds and the HUD are left out)
// @port ov003:021146a4 partial / ov003:02114ee4 partial (countdown lights not drawn) / ov003:02115940 partial
// (the pause button is not handled)
// @port ov003:02121738 partial (local player only; the camera-distance update of the triggers is not ported)
// @port ov003:0212bd68 done (one car)
// @port main:020ad21c done
void dsRaceProgress::Frame(const dsCarInput &input, int ticks)
{
    // dsRace update: respawn request of the local player (sinking, stuck) -> mode reset 0.
    if (respawnRequest || m_car->respawnRequest)
        Reset(false);
    dsCarInput in = started ? input : dsCarInput();
    m_car->SetInput(in);
    for (int i = 0; i < ticks; i++)
        m_car->Step();
    if (m_car->respawnRequest)
        respawnRequest = true;
    AfterCarStep(ticks);
}

void dsRaceProgress::AfterCarStep(int ticks)
{
    UpdateCurrentPiece();
    VecFx32 tp = TestPoint();
    for (size_t i = 0; i < m_triggers.size(); i++) {
        dsTrigger *t = m_triggers[i];
        if (!(t->playerMask & 1) || !t->Update(0, tp))
            continue;
        if (t->kind == dsTrigger::FINISH)
            OnFinish();
        else
            OnCheckpoint(t->pieceIndex);
    }
    // HUD clock (0x02105408 -> main 0x020ad21c)
    if (clockRunning) {
        fx32 d = ticks * g_dsFrameTime;
        if (!clockDown) {
            clock += d;
        } else {
            clock -= d;
            if (clock < 0)
                clock = 0;
        }
    }
    // Countdown (0x02114ee4): "go" once the clock reaches 0.
    if (countdown && clock == 0) {
        countdown = false;
        clockDown = false;
        started = true;
        Log(dsRaceEvent::GO, 0);
    }
    // 0x02115940: below the sea level by more than 2 units -> 180 ticks later, respawn.
    if (!finished && sinking) {
        sinkTicks -= ticks;
        if (sinkTicks < 1)
            respawnRequest = true;
    }
    if (!finished) {
        fx32 y = m_car->matrix.m[3][1], lim = m_car->minHeight;
        if (y < lim && 0x2000 < lim - y)
            sinking = true;
    }
    if (!leftStart && currentPiece != startPiece)
        leftStart = true;
}
