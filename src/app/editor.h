// The track editor's state (ov002) behind RaceApp's EDITING screen, shared by race_app.cpp and editor.cpp.
#pragma once

#include <memory>
#include <string>
#include <vector>

#include "editor/dsEditorTrack.h"
#include "race_app.h"

// config/PuzzleProgression (editor.cpp): puzzle n's difficulty, puzzle medals to unlock it, gold / silver / bronze
// times and coppers per medal.
int PuzzleCount();
// A saved track's bytes (a file of the memory stick or the host's disk, not the game's file system).
bool ReadTrackFile(const std::string &path, std::vector<u8> *out);
bool PuzzleInfo(int n, std::string *diff, int *need, fx32 times[3], int *coppers);
std::string PuzzleName(int n);
// An editor block icon (Editor/Cell/<Env>Blocks) in a size x size square, and the icon of a block by its name in
// the environment's TrackPieceLayout (-1: none).
void FeBlockIcon(const FeCanvas &fe, int env, int icon, float x, float y, float size, u32 rgba);
int BlockIconOf(int env, const std::string &name); // "Puzzle Easy 1": its difficulty and its place in it

struct RaceApp::Editor {
    std::unique_ptr<dsEditorTrack> ed;
    dsBlockMenu menu;
    int env = 0;
    int puzzle = -1;                 // config/PuzzleProgression entry (0..16); -1: the free editor
    std::vector<std::pair<u32, int>> stock; // puzzle: blocks left per piece id

    // Block menu (TopFolder, then the folders opened).
    std::vector<std::string> folders;
    std::vector<int> folderCursor;
    bool menuOpen = false;
    u32 block = 0;                   // the block carried by the cursor (0: none, the cursor selects)
    int blockIcon = -1;
    std::string blockName;
    int rot = 0;

    s32 cur[3] = {10, 0, 10};        // cursor cell
    float yaw = 0.6f, pitch = 0.75f, dist = 70; // orbit camera around the cursor
    float target[3] = {};            // camera target, following the cursor
    std::unique_ptr<dsTrackPiece> preview; // the carried block at the cursor (drawn translucent)
    bool previewOk = false;

    std::vector<dsEditorTrack::Snapshot> undo;
    bool modified = false;
    bool validated = false;          // author time set by a validation run since the last change
    std::string file;                // the track's name (its save file), "" until saved

    // Overlays: the START menu, the objectives, the track list, the name entry, the help page.
    enum Overlay { O_NONE, O_MENU, O_OBJECTIVES, O_LOAD, O_NAME, O_HELP };
    int overlay = O_NONE;
    int cursor = 0;
    std::vector<std::string> files;  // O_LOAD: the saved tracks of this environment
    std::string entry;               // O_NAME
    int confirm = 0;                 // a yes / no question (Confirm values of editor.cpp), 0: none
    int confirmCursor = 1;
    std::string message;
    int messageFrames = 0;

    u32 prevHeld = 0;
    int repeat = 0;
    std::string script;              // EditorScript's pad presses
    size_t scriptPos = 0;
    u32 scriptHold = 0;              // keys held by the script ('[' ... ']')
    int run = 0;                     // RaceApp::Run of the race started from the editor; -1: leave once saved
    VecFx32 dropPos = {}, dropDir = {}; // the test drive's car
    // Modes besides building: free draw (X held paints the road along the cursor's way) and placing the test
    // vehicle (the DS's TestArrow piece, moved, raised and turned before the test drive).
    enum Mode { MODE_BUILD, MODE_DRAW, MODE_CAR };
    int mode = MODE_BUILD;
    bool stroke = false;            // MODE_DRAW: X held since a stroke began
    int carRot = 0;                 // MODE_CAR: the arrow's quarter turns
    bool carPlaced = false;         // the arrow's cell was chosen before (kept between test drives)
    s32 carCell[3] = {};
};
