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
    std::vector<std::pair<G3Model *, const dsTrackPiece *>> pieces;
    size_t shownEvents = 0, resets = 0;

    bool Load(const char *trackName);
    // One 30 Hz frame: two 1/60 s ticks of the race with this input, events printed to stdout, the camera reset on
    // restart/respawn and updated once.
    void Frame(const dsCarInput &in);
    void Respawn() { progress->Reset(false); }
    void Restart() { progress->Reset(true); }
    void ToggleCamera()
    {
        camIndex ^= 1;
        cams[camIndex].Reset();
    }
};

bool RaceReadFile(const std::string &path, std::vector<u8> *out);
G3Model *RaceLoadModel(const std::string &name); // graphics/<name>.n, uploaded

// DS 4x3 matrix (row vectors, fx32) to a column-major 4x4 float matrix (GL and GU layout).
void RaceMatrix(const MtxFx43 &m, float *g);

// HUD stand-in: clock, checkpoints and laps in the corners, the countdown and the race messages in the middle, for
// a w x h screen; `scale` multiplies the pixel sizes (1 at 768 x 576). Text is drawn by the callback.
typedef void (*RaceTextFn)(float x, float y, float px, const char *s, float r, float g, float b, void *user);
void RaceHudLayout(int w, int h, float scale, const dsRaceProgress &rp, RaceTextFn text, void *user);
// Rows of a 5 x 7 glyph ('1' = pixel), NULL for characters without a glyph.
const char *const *RaceGlyph(char c);
