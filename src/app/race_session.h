// The race session shared by the race drivers (src/host/race.cpp on SDL2/OpenGL, src/psp/race_psp.cpp on
// sceGu/sceCtrl): track, terrain and collision, car, race progress, chase cameras and the models to draw, plus the
// HUD stand-in's layout and 5 x 7 font. Drawing goes through the G3D stand-in (g3d_model.h) of each platform.
#pragma once

#include <memory>
#include <utility>
#include <vector>

#include "car/dsCarDynamics.h"
#include "car/dsCarParams.h"
#include "g3d_model.h"
#include "race/dsChaseCamera.h"
#include "race/dsRaceProgress.h"
#include "track/dsTrack.h"

struct RaceSession {
    dsTrack track;
    dsCarParamsTable table;
    dsCarModelGeometry geo;
    std::unique_ptr<dsCarDynamics> car;
    std::unique_ptr<dsRaceProgress> progress;
    dsChaseCamera cams[2] = {dsChaseCamera(1), dsChaseCamera(2)}; // config/cameras Chase1 / Chase2
    int camIndex = 0;
    G3Model *batch = nullptr, *carModel = nullptr;
    G3Model *skyModel = nullptr;  // Sky/<env>/sky_<time of day> (ov003 0x0211de20)
    float skyScale[3] = {1, 1, 1};
    // DS lighting and fog of the track's time of day (Scripts/config/tracks, see BuildLighting); off: the
    // material colours as before (tm_race --unlit).
    G3LightEnv lightEnv;
    bool lighting = true;
    std::vector<std::pair<G3Model *, const dsTrackPiece *>> pieces;
    std::vector<std::unique_ptr<G3Model>> models; // owns batch, carModel and the piece models
    std::vector<char> visible;  // per entry of `pieces`, from Cull
    size_t shownEvents = 0, resets = 0;

    // The track's collision shapes leave the collision world with the track; the drivers clear the world's mesh
    // cache after deleting a session (RaceUnloadWorld) so that reloading tracks does not grow the heap.
    ~RaceSession();
    bool Load(const char *trackName);
    // One 30 Hz frame: two 1/60 s ticks of the race with this input, events printed to stdout, the camera reset on
    // restart/respawn and updated once.
    void Frame(const dsCarInput &in);
    // Marks the pieces whose bounding sphere touches the current camera's view frustum (vertical FOV from the
    // camera, aspect w/h, near and far planes as the driver's projection). A port addition for the PSP's GE: the
    // DS draws every piece the race grid gives it.
    void Cull(float aspect, float nearZ, float farZ);
    // Lighting of piece k (its rotation; colours baked per rotation on the PSP) and of the car (its matrix, the
    // camera's line of sight for the specular term, recomputed every draw). Null when lighting is off.
    const G3DrawLight *PieceLight(size_t k, G3DrawLight *out) const;
    const G3DrawLight *CarLight(G3DrawLight *out) const;
    void Respawn() { progress->Reset(false); }
    void Restart() { progress->Reset(true); }
    void ToggleCamera()
    {
        camIndex ^= 1;
        cams[camIndex].Reset();
    }
};

void RaceUnloadWorld(); // collision meshes kept by the collision world (call with no session alive)

// The race renderer's lighting for an environment and time of day: lights (ov003 0x0211dc68), colour sets
// (main 0x02055970 via the renderer +0xdc), shininess table and fog (renderer init main 0x0204f528, fog colour
// renderer +0xe0, G3X_SetFog every frame in itcm 0x01ff9628). False when config/tracks has no entry.
bool RaceBuildLighting(const char *env, const char *timeOfDay, G3LightEnv *out);

bool RaceReadFile(const std::string &path, std::vector<u8> *out);
G3Model *RaceLoadModel(const std::string &name, bool upload = true); // graphics/<name>.n

// DS 4x3 matrix (row vectors, fx32) to a column-major 4x4 float matrix (GL and GU layout).
void RaceMatrix(const MtxFx43 &m, float *g);

// HUD stand-in: clock, checkpoints and laps in the corners, the countdown and the race messages in the middle, for
// a w x h screen; `scale` multiplies the pixel sizes (1 at 768 x 576). Text is drawn by the callback.
typedef void (*RaceTextFn)(float x, float y, float px, const char *s, float r, float g, float b, void *user);
void RaceHudLayout(int w, int h, float scale, const dsRaceProgress &rp, RaceTextFn text, void *user);
// Rows of a 5 x 7 glyph ('1' = pixel), NULL for characters without a glyph.
const char *const *RaceGlyph(char c);
