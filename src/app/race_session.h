// The race session shared by the race drivers (src/host/race.cpp on SDL2/OpenGL, src/psp/race_psp.cpp on
// sceGu/sceCtrl): track, terrain and collision, car, race progress, chase cameras and the models to draw, plus the
// HUD stand-in's layout and 5 x 7 font. Drawing goes through the G3D stand-in (g3d_model.h) of each platform.
#pragma once

#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "car/dsCarDynamics.h"
#include "car/dsCarLights.h"
#include "car/dsCarParams.h"
#include "g2d_cells.h"
#include "g3d_model.h"
#include "gfx/dsSeaBackdrop.h"
#include "hud/dsHud.h"
#include "race/dsChaseCamera.h"
#include "race/dsGhost.h"
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
    G3Model *wheelModels[2] = {}; // Cars/<env>/lwheel (wheels 0, 2) and rwheel (1, 3), ov003 0x020fd914
    G3Model *shadowModel = nullptr; // Cars/<env>/shadow, a shadow volume (ov003 0x020fd914, car visual +0x10)
    float skyScale[3] = {1, 1, 1};
    // The sea (dsSeaBackdrop) and the model holding its texture, misc/<long environment name>/<same>; none: no sea.
    G3Model *seaModel = nullptr;
    dsSeaBackdrop sea;
    // The scenery around the track (ov003 0x0211d37c Stadium: two stands and two screens, 0x0211d02c Island: four
    // islands, 0x0211ccec Snow: four snowy ridges, 0x0211ca88 Coast: one landscape), all outside the track's cells.
    // The drivers draw them after the sky with its far plane (the race's own, 600 units, would cut them), depth
    // tested among themselves, then clear the depth for the track.
    struct Scenery {
        G3Model *model;
        MtxFx43 matrix;
        int variant; // quarter turns (0 or 2), for the PSP's lit colours
    };
    std::vector<Scenery> scenery;
    // The ground under the track (ov003 0x0211de20): a horizontal plane of the collision world.
    std::unique_ptr<class dsCollisionPlaneShape> groundPlane;
    // DS lighting and fog of the track's time of day (Scripts/config/tracks, see BuildLighting); off: the
    // material colours as before (tm_race --unlit).
    G3LightEnv lightEnv;
    bool lighting = true;
    std::vector<std::pair<G3Model *, const dsTrackPiece *>> pieces;
    std::vector<std::unique_ptr<G3Model>> models; // owns batch, carModel and the piece models
    std::vector<char> visible;  // per entry of `pieces`, from Cull
    std::vector<u8> detail;     // per entry of `pieces`, from Cull: G3SetDrawDetail level by distance from the eye
    size_t shownEvents = 0, resets = 0;
    std::string shownMessage; // the HUD message last logged
    // The game's HUD (dsHud): its sprites are drawn by the drivers with the banks of RaceHudBanks. The record's
    // split times (passing order) and finish time are set by the application (none: 0 / empty).
    dsHud hud;
    std::vector<fx32> recordSplits;
    fx32 recordTime = 0;
    // The car's brake / reverse / head lights (dsCarLights) and the carModel materials they control ("blight",
    // "rlight", "hlight"; -1: absent), applied as G3Model overrides after every frame.
    dsCarLights carLights;
    int carLightMaterial[3] = {-1, -1, -1};
    // The start lights: the "StartRed" / "StartAmber" / "StartGreen" batch materials of the piece models (material
    // controllers of main 0x0206aaf4, lights 0 to 2) and their levels, set by the countdown (ov003 0x02114ee4).
    struct StartLight {
        G3Model *model;
        int material, light;
    };
    std::vector<StartLight> startLights;
    s8 startLevel[3] = {0, 0, 0};
    void ApplyStartLights();
    int texAnimFrame = 0;         // frame of the material animations (G3SetTexAnimFrame)
    // Sound (dsGameAudio, dsCarAudio): the race events already heard and the countdown beep last played (the race
    // mode's +0x18: -1 after a restart, then 0..3).
    size_t soundEvents = 0;
    int countdownSound = -1;
    // Ghost cars (dsGhost, set by the application: the medal opponent, the player's best run), drawn with the car's
    // body model made translucent (GhostLook), and the recording of this run from the start (the race clock's 0).
    std::vector<std::unique_ptr<dsGhost>> ghosts;
    dsGhost recorder;
    bool recordStarted = false;
    struct Draw;
    void GhostLook(const Draw &d, bool on);
    std::vector<G3Model::MatOverride> savedOverrides;

    // The track's collision shapes leave the collision world with the track; the drivers clear the world's mesh
    // cache after deleting a session (RaceUnloadWorld) so that reloading tracks does not grow the heap.
    ~RaceSession();
    // "coa/easy/0": environment, difficulty, number. "pla/easy/1": a platform track (race mode 8), whose
    // environment is the scene's own (pla/<difficulty>/<n> is environment n).
    bool Load(const char *sceneName);
    // The track editor (src/app/editor.cpp). LoadTrack: a race on a copy of the editor's pieces (mode 6: test drive,
    // which may have no start; the terrain is rebuilt). The piece catalogue of `env` must be loaded already: its
    // entries are the editor's. LoadView: the environment's scene without car or race (sky, scenery, lighting, the
    // texture batch) for the editor's view, whose pieces are given by SyncPieces after every change.
    bool LoadTrack(int env, int mode, const dsTrackInfo &info, const std::vector<dsTrackPiece *> &pieces);
    bool LoadView(int env, int timeOfDay);
    void SyncPieces(const std::vector<dsTrackPiece *> &list);
    G3Model *PieceModel(const std::string &path); // a piece model, loaded once per session
    bool editorView = false;
    // Set before Load by the front end: the car's skin (Cars/<env>/<skin>, config/SkinShop) and the time of day
    // (0 rise, 1 day, 2 set; -1: the track's own, as the race challenges).
    int skin = 0;
    int timeOfDay = -1;
    void LoadScenery(int env);
    bool Build(const char *sceneName); // everything after the pieces are placed
    void LoadCar(const std::string &envName);
    void LoadEnvironment();
    G3Model *Own(G3Model *m);
    void FindStartLights(G3Model *m);
    std::map<std::string, G3Model *> modelCache;
    int mode = 1;                 // dsRaceSetup +0x10: 1 race, 8 platform
    // One 30 Hz frame: two 1/60 s ticks of the race with this input, events printed to stdout, the camera reset on
    // restart/respawn and updated once.
    void Frame(const dsCarInput &in);
    void ApplyCarLights();
    // The game audio's per-frame update (main 0x0208c730: faders, then the car audio heard from the current camera),
    // called by the application after Frame, and also while the race is paused or over.
    void AudioUpdate();
    void Horn(); // the horn button (dsRace_Update, ov003 0x02113048 -> 0x020f6dc8)
    // The car's shadow volume: its matrix (column-major) and colour; drawn by the drivers after the car with
    // G3Model::DrawShadowVolume. False when there is none.
    bool ShadowDraw(float *matrix, u16 *rgb555, int *alpha) const;
    // Marks the pieces whose bounding sphere touches the current camera's view frustum (vertical FOV from the
    // camera, aspect w/h, near and far planes as the driver's projection). A port addition for the PSP's GE: the
    // DS draws every piece the race grid gives it. Also picks each piece's detail level (PSP guard band split):
    // 1 beyond 20 units from the eye, 2 beyond 60 (2.5 times their edges of 8 and 24 units).
    void Cull(float aspect, float nearZ, float farZ);
    // Lighting of piece k (its rotation; colours baked per rotation on the PSP) and of the car (its matrix, the
    // camera's line of sight for the specular term, recomputed every draw). Null when lighting is off.
    const G3DrawLight *PieceLight(size_t k, G3DrawLight *out) const;
    const G3DrawLight *CarLight(G3DrawLight *out) const;
    const G3DrawLight *SceneryLight(size_t i, G3DrawLight *out) const;
    // The car's draws: the body (lod1) at the car matrix and the four wheels at the matrices of the visual update
    // (dsCarDynamics::wheelMatrix, ov003 0x02126d10, given to the car visual by ov003 0x020fbe48).
    struct Draw {
        const G3Model *model;
        float matrix[16];
        G3DrawLight light;
        bool lit;
        // 1: a ghost car's body; 2: the editor's block under the cursor, 3: the same where it cannot be placed;
        // 4: an editor direction marker (translucent, dsTrackPiece::EditorMarkers).
        // Drawn between GhostLook(draw, true) and GhostLook(draw, false); a piece model takes the batch.
        int ghost;
    };
    enum { MAX_GHOSTS = 4, MAX_EDITOR_DRAWS = 64, MAX_CAR_DRAWS = 5 + MAX_GHOSTS + MAX_EDITOR_DRAWS };
    int CarDraws(Draw *out) const; // up to MAX_CAR_DRAWS
    // The editor's extra draws (CarDraws appends them) and lines in world units (the building area's edge, the
    // cursor's column), drawn by the drivers after the opaque pieces with depth test.
    std::vector<Draw> editorDraws;
    struct Line {
        float a[3], b[3];
        u32 rgba; // red in the low byte
    };
    std::vector<Line> lines;
    Draw PieceDraw(const G3Model *m, const MtxFx43 &mtx, int rotation, int ghost) const;
    // The sea's quads for the current camera (dsSeaBackdrop::QUADS x 4 vertices, world units) and their colour, for
    // seaModel->DrawQuads with its texture 0 (fog: lightEnv.fog when lit). The drivers draw them after the track
    // and the shadow, before the car (the DS draws translucent polygons after the opaque ones). False: no sea.
    bool SeaDraw(G3QuadVertex *v, u16 *rgb555) const;
    void Respawn() { progress->RespawnRequest(); }
    void Restart() { progress->Restart(); }
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

// The HUD's cell banks (dsHudBank order), loaded once on first use and kept (the drivers upload them); null when
// a file is missing.
G2CellBank *const *RaceHudBanks();
// The HUD sprites for a w x h screen: the DS top screen (256 x 192) scaled by h / 192 around the screen's centre;
// on a wider screen the side parts move to the edges (dsHudSprite::anchor).
// `sprite` receives the bank, the cell's place in the atlas and the destination rectangle.
typedef void (*RaceSpriteFn)(const G2CellBank &bank, const G2Cell &cell, float x, float y, float w, float h,
                             void *user);
void RaceHudSprites(int w, int h, const dsHud &hud, RaceSpriteFn sprite, void *user);
// The HUD's translucent backing panels (dsHud::BackingQuads), drawn before the sprites, with the same screen
// mapping. Their texture (Graphics/hud/backing.n "speedobg": black, alpha only) is read once and drawn exactly as
// rectangles of equal alpha, so the drivers need only their filled-rectangle callback.
typedef void (*RaceRectFn)(float x, float y, float w, float h, float r, float g, float b, float a, void *user);
void RaceHudBacking(int w, int h, const dsHud &hud, RaceRectFn rect, void *user);

// HUD text stand-in: the finish message in the middle of a w x h screen; `scale`
// multiplies the pixel sizes (1 at 768 x 576). Text is drawn by the callback.
typedef void (*RaceTextFn)(float x, float y, float px, const char *s, float r, float g, float b, void *user);
void RaceHudLayout(int w, int h, float scale, const dsRaceProgress &rp, RaceTextFn text, void *user);
// Rows of a 5 x 7 glyph ('1' = pixel), NULL for characters without a glyph.
const char *const *RaceGlyph(char c);
