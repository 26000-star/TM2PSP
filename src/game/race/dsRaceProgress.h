// Race progress of the local player: triggers, checkpoints, laps, finish, respawn, countdown and race clock.
// It gathers the parts of three original objects the race needs on the host:
//  - the race manager dsRace (*0x02137854, ov003): checkpoint count +0x134, lap count +0x148, start piece +0x5c,
//    piece under the car +0x60, per-frame update 0x02113048, trigger creation 0x02111280 / 0x021114e4;
//  - the race game mode (vtable 0x02136828, constructor 0x02115fd8): reset 0x02115af4 (vtable +0x20), finish
//    0x02115704 (+0x24), checkpoint 0x0211527c (+0x28), update 0x021146a4 / 0x02114ee4 / 0x02115940;
//  - the HUD race clock (*0x02137830 +0x130, main 0x020ad21c): 3 s countdown, then counts up.
// The HUD drawing, sounds, ghosts, replays, split-time records and network modes are not ported.
#pragma once

#include <string>
#include <vector>

#include "nitro.h"
#include "race/dsTrigger.h"

class dsTrack;
class dsTrackPiece;
class dsCarDynamics;
struct dsCarInput;

struct dsRaceEvent {
    enum Type { RESTART, GO, CHECKPOINT, MISSED, LAP, FINISH, RESPAWN } type;
    int value;   // CHECKPOINT: order, MISSED: number missed, LAP: laps done
    fx32 time;   // race clock when it happened
};

class dsRaceProgress {
public:
    dsRaceProgress(dsTrack *track, dsCarDynamics *car);
    ~dsRaceProgress();

    // Game mode reset (ov003 0x02115af4): full = back to the start with the countdown, otherwise respawn at the
    // last checkpoint (turned into a full reset when none was passed or the race is over).
    void Reset(bool full);
    // The respawn of dsRace_Update (ov003 0x02113048: the respawn button or the player's own request, sinking or
    // stuck): retries counts it (0 when no checkpoint was passed: back to the start), and on Stadium a respawn
    // whose only checkpoint is a ring (pieces 0x10 / 0x11, which give no respawn point) restarts the race.
    void RespawnRequest();
    // The restart button (dsRace_Update): retries back to 0, full reset.
    void Restart();
    // One game frame of `ticks` 1/60 s ticks: respawn request, car steps with the given input (zero until the
    // countdown ends), piece under the car, triggers, clock, countdown, sinking. ov003 0x021146a4 + 0x02113048.
    void Frame(const dsCarInput &input, int ticks);
    // The part of Frame after the car steps (also used by the checks, which move the car themselves).
    void AfterCarStep(int ticks);

    // Trigger matrices (exposed for the checks); built by CreateTriggers.
    const std::vector<dsTrigger *> &Triggers() const { return m_triggers; }
    // Car matrix the game would place at pos facing dir (ov003 0x0211c184 then the player reset): ground probe
    // from 2 units above, direction snapped to the axes, frame from dsMtx43_FromAxes.
    MtxFx43 PlacementMatrix(const VecFx32 &pos, const VecFx32 &dir) const;
    // The editor's test drive (ov002 "Placing vehicle"): the car and its respawn point at pos, heading dir.
    void Drop(const VecFx32 &pos, const VecFx32 &dir);
    // Start position and direction of the start piece (ov003 0x02114a6c, without the camera call).
    void StartPoint(const dsTrackPiece *piece, VecFx32 *pos, VecFx32 *dir) const;
    // Test point of the triggers: car position + 3 * unit velocity (ov003 0x02121738).
    VecFx32 TestPoint() const;

    // dsRace
    int checkpointCount = 0;           // +0x134 checkpoints, + 1 for the multilap piece of a lap track
    int lapCount = 1;                  // +0x148
    const dsTrackPiece *startPiece = nullptr;   // +0x5c / grid 0x0208cdcc: first start or multilap piece
    const dsTrackPiece *currentPiece = nullptr; // +0x60
    std::vector<int> checkpointOrder;  // piece +0x28 per piece index (-1 for other pieces)
    // race mode
    bool countdown = false;            // +0x14
    int lapsLeft = 0;                  // +0x1c
    bool finished = false;             // +0x20
    bool leftStart = false;            // +0x23 the car left the start piece (lap tracks)
    VecFx32 respawnPos = {};           // +0x24
    VecFx32 respawnDir = {};           // +0x30
    int respawnPiece = 0;              // +0x3c
    int respawnPieceId = 0;            // +0x40
    int checkpointsPassed = 0;         // +0x44
    int lastCheckpoint = -1;           // race +0x138: piece index of the last checkpoint passed (0x02112f8c)
    int retries = 0;                   // race +0x14c: respawns since the start (HUD +0x32c, platform mode)
    bool sinking = false;              // +0x48
    int sinkTicks = 180;               // +0x4c
    // player / clock
    bool started = false;              // player +0xbe: controls released at "go"
    bool respawnRequest = false;       // player +0xd5 (also set by the car when stuck)
    fx32 clock = 0;                    // HUD timer +0x28 (fx32 seconds)
    bool clockDown = false;            // +0x2c counting down
    bool clockRunning = false;         // +0x79
    int lapsDone = 0;                  // player +0x70
    fx32 finishTime = -1;              // player +0x84
    std::vector<dsRaceEvent> events;   // what happened, in order (host log / HUD)

    // ov003 0x021213f8 (clear), 0x02111280 (finish), 0x021114e4 (checkpoints); called by Reset(true).
    void CreateTriggers();

private:
    void PlaceCar(const MtxFx43 &m);   // player reset 0x0210f3b4 / 0x0210e124 (car reset, trigger sides)
    void OnCheckpoint(int pieceIndex); // dsRace 0x02112f8c -> mode 0x0211527c
    void OnFinish();                   // dsRace 0x02112f50 -> mode 0x02115704
    void UpdateCurrentPiece();         // ov003 0x02112360
    const dsTrackPiece *GridPiece(s32 x, s32 y, s32 z) const;
    int PieceIndex(const dsTrackPiece *p) const;
    void Log(dsRaceEvent::Type t, int v) { events.push_back({t, v, clock}); }

    dsTrack *m_track;
    dsCarDynamics *m_car;
    std::vector<dsTrigger *> m_triggers;
};
