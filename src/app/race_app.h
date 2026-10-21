// The race application shared by the race drivers (host and PSP): the front end (main menu, solo modes, pre-race
// settings, profile, options, shop), the pause and results menus, track loading and unloading, and the player's
// progress kept in a small save file (best times, coppers, purchases, options).
// The front end's content follows the DS (strings/main/frontend and backend, Scripts/config RaceProgression,
// PlatformProgression, TrackShop, SkinShop; medals are the times of the provided ghosts); its look is a port addition
// in the style of the TrackMania Wii menus on the PSP's 16:9 screen (fe_canvas.h, src/app/fe_menu.cpp). The 3D scene
// is drawn by the driver from `race` when it is not null.
#pragma once

#include <memory>
#include <string>
#include <vector>

#include "fe_canvas.h"
#include "race_session.h"

struct RaceTrackEntry {
    const char *id;    // scene: "coa/easy/0"
    const char *label; // the track's short name in the stand-in lists ("FACILE 1")
};
// The 26 race tracks of each environment (stadium, island, snow, coast), then the 17 platform tracks.
const RaceTrackEntry *RaceTracks(int *count);


class RaceApp {
public:
    enum Key { UP, DOWN, LEFT, RIGHT, OK, BACK, PAUSE };
    enum Screen { TRACKS, PLAYING, PAUSED, RESULT, EDITING }; // TRACKS: the front end; EDITING: the track editor
    enum Mode { MODE_RACE, MODE_QUICK, MODE_PLATFORM };

    // savePath: the player's progress ("" = none). okName / backName: unused (the hints show the PSP's buttons).
    RaceApp(const std::string &savePath, const char *okName, const char *backName);
    ~RaceApp();

    // Sound: reads audio/sound_data.sdat and starts the front end's music (dsGameAudio). Without it the program is
    // silent. The drivers render the sound driver's output after each Frame (SndStream).
    bool InitAudio();
    // Strings in `language` (dsStringTable::Language): Common, BackEnd (the race) and FrontEnd. The save file's
    // language option, when there is one, replaces the drivers' choice.
    void SetLanguage(int language);
    int Language() const { return m_language; }
    // Loads track `index` (any session is deleted first, with the collision meshes) and starts playing.
    bool Start(int index);
    int FindTrack(const char *id) const; // -1 when unknown; "easy/0" (no environment) is a Coast track
    int NextTrack(int index) const;      // the next track of the same menu section
    void Unload();
    // A menu key; returns false when the player leaves the program (BACK on the main menu).
    bool Press(Key k);
    // Keys for the menu captures: one letter per key (U D L R, O = OK, B = back, P = pause), pressed in order.
    void PressScript(const char *keys);
    // One 30 Hz frame: the race advances while playing; a finish records the best time, the medals and coppers,
    // and opens the result menu after 3 s (unless menus are off: the drivers' auto tests). Then the game's sound
    // updates (dsGameAudio, dsSound3D, dsSoundManager), on every screen.
    void Frame(const dsCarInput &in);
    void Horn(); // the horn button while playing
    // HUD while playing (the game's sprites, then the text stand-in) for a w x h screen (scale as RaceHudLayout),
    // the front end and the pause / results menus through `fe` (fitted to the screen by the caller).
    void Overlay(const FeCanvas &fe, int w, int h, float scale, RaceTextFn text, RaceRectFn rect,
                 RaceSpriteFn sprite, void *user) const;

    Screen screen = TRACKS;
    std::unique_ptr<RaceSession> race;
    int track = -1;       // loaded track
    int selected = 0;     // the last track chosen (the front end opens on its list after a race)
    int item = 0;         // cursor in the pause / result menu
    bool menus = true;    // false: no result menu (auto tests)
    bool lighting = true; // false: sessions draw unlit (tm_race --unlit)
    void SetPlayerName(const char *name) { if (name && *name) m_name = name; } // the profile's name
    void ToTracks(); // back to the front end (the track list of the last race), with its sounds and music

    // The track editor (src/app/editor.cpp). The drivers give it the pad every frame while Editing(): the held
    // buttons (PadButton) and the analog stick (-1..1, y down).
    enum PadButton {
        PAD_UP = 1, PAD_DOWN = 2, PAD_LEFT = 4, PAD_RIGHT = 8, PAD_CROSS = 16, PAD_CIRCLE = 32, PAD_SQUARE = 64,
        PAD_TRIANGLE = 128, PAD_L = 256, PAD_R = 512, PAD_START = 1024, PAD_SELECT = 2048
    };
    bool Editing() const { return screen == EDITING; }
    struct Editor; // the editor's state (editor.h)
    void EditorPad(u32 held, float stickX, float stickY);
    // Opens the editor on a new track of environment env (0..3), or puzzle n (0..16) of config/PuzzleProgression.
    bool OpenEditor(int env, int puzzle = -1);
    // Pad presses for the captures, one per frame: U D L R, X O S T (cross circle square triangle), l r (shoulders),
    // P (start), E (select), '.' (nothing).
    void EditorScript(const char *keys);

private:
    // Front end pages (src/app/fe_menu.cpp).
    enum Page {
        P_MAIN, P_SOLO, P_ENV, P_DIFF, P_TRACK, P_DAYTIME, P_SKIN, P_PROFILE, P_STATS, P_OPTIONS, P_AUDIO, P_LANG,
        P_DATA, P_SHOP, P_SHOP_ENV, P_SHOP_ITEMS, P_CREDITS, P_DISPLAY, P_EDITOR_ENV, P_PUZZLES, P_MYTRACKS
    };
    struct PageState {
        int page, cursor;
    };
    struct Item; // one entry of a page, rebuilt from the state when needed
    void Items(int page, std::vector<Item> *out) const;
    void Open(int page, int cursor = 0);
    void Back();
    bool PressMenu(Key k);
    void Choose(const Item &it);
    void Popup(const std::string &text, int confirm = 0);
    void Confirmed(int action);
    void DrawFrontEnd(const FeCanvas &fe) const;
    void DrawPage(const FeCanvas &fe, int page, int cursor, float dx, float alpha) const;
    void DrawPanel(const FeCanvas &fe, const Item &it, float x, float y, float w, float h) const;
    void DrawRaceMenu(const FeCanvas &fe) const;
    void DrawPopup(const FeCanvas &fe) const;
    void DrawTitle(const FeCanvas &fe, const char *title, float alpha) const;
    void DrawHints(const FeCanvas &fe, bool back) const;
    void StartChosen(int index);
    void BuildContextFor(int index);

    std::unique_ptr<Editor> m_ed; // the track editor (src/app/editor.cpp), null outside it
    // What the race in progress is: a track of the menus, the editor's test drive or validation, a puzzle, a
    // saved track (My Tracks). The pause and results menus depend on it.
    enum Run { RUN_TRACK, RUN_TEST, RUN_VALIDATE, RUN_PUZZLE, RUN_CUSTOM };
    int m_run = RUN_TRACK;
    void EditorFrame();
    void DrawEditor(const FeCanvas &fe) const;
    void EditorKey(u32 pressed, u32 held);
    void EditorMenuKey(u32 pressed);
    void EditorModeKey(u32 pressed, u32 held, bool moved);
    void EditorPlace();
    void EditorDelete();
    void EditorSync();
    void EditorPreview();
    void EditorMessage(const std::string &text);
    void EditorRace(bool validate);
    void EditorBack();             // from its race to the editor (the view session again)
    void EditorClose();            // to the front end
    void EditorFinished();         // a validation or puzzle run reached the finish
    bool EditorSave(const std::string &name, bool overwrite);
    bool EditorLoad(const std::string &name);
    std::string TracksDir() const;
    std::vector<std::string> SavedTracks(int env) const; // names of the saved tracks (all environments: env -1)
    // The saved tracks' headers, read once from TracksDir() and again after a save or a deletion: the front end's
    // pages are rebuilt every frame, and reading the memory stick there made the music stutter.
    struct SavedTrack {
        std::string name;
        int env = 0;
        bool platform = false;
        fx32 times[4] = {}; // author, gold, silver, bronze (0: not validated)
    };
    const std::vector<SavedTrack> &SavedList() const;
    mutable std::vector<SavedTrack> m_saved;
    mutable bool m_savedRead = false;
    bool StartCustom(const std::string &name);           // My Tracks: race a saved, validated track
    std::vector<int> RaceMenu() const;                   // the actions of the pause / results menu
    int PuzzleMedal(int n) const;
    std::vector<fx32> m_puzzleBest;          // per PuzzleProgression entry, 0 = none
    std::string m_custom;                    // the saved track being raced (RUN_CUSTOM)
    std::vector<u8> m_customData;
    std::vector<std::pair<std::string, fx32>> m_customBest;
    bool m_editorUnlocked = false;           // a validated track was saved: Puzzle and My Tracks open

    // Progress (src/app/race_app.cpp).
    int Medal(int index) const;              // 0 none, 1 bronze, 2 silver, 3 gold (best run)
    int MedalFor(int index, fx32 time, int retries) const;
    fx32 MedalTime(int index, int medal) const; // race tracks: the provided ghost's finish time (0: none)
    int MedalsTotal(bool platform) const;
    int RequiredMedals(int index) const;     // medals to unlock the track's difficulty
    int Coppers(int index) const;            // coppers per medal won on the track
    int ShopTrack(int index) const;          // its config/TrackShop entry, -1 when free
    bool Unlocked(int index) const;          // difficulty reached and bought when sold
    bool PlatformUnlocked() const;           // a medal in every race environment ("lockedplatformmode")
    bool Owned(int shopKind, int n) const;   // shopKind 0 tracks, 1 skins, 3 blocks
    // config/BlockShop: the block of environment env named `name` (PieceData name, any case) is sold and not bought
    // yet; the editor refuses it (main 0x020b21e4, from ov002's block menu). Never in puzzle mode.
    bool BlockLocked(int env, const std::string &name) const;
    // Entry n of config/TrackShop (kind 0: index = the track), config/SkinShop (1: index = skin 0..9) or
    // config/BlockShop (3: name = "STA_Multilap", the environment's prefix and the block's PieceData name).
    bool ShopEntry(int kind, int n, int *env, int *price, int *index, std::string *name) const;
    int MedalRetries(int platformIndex, int medal) const; // config/PlatformProgression's retries for a medal
    std::string PlayerGhostPath(int index) const; // the player's best run next to the save file ("": none)
    void LoadGhosts(int index);
    void RaceFrame(const dsCarInput &in);
    void Sound(const char *name) const;
    void LoadSave();
    void WriteSave() const;
    int ItemCount() const;
    void Restart();

    std::string m_savePath;
    std::unique_ptr<fx32[]> m_best; // best finish time per track, 0 = none
    std::unique_ptr<std::vector<fx32>[]> m_splits; // the best run's split times per track (HUD differences)
    std::unique_ptr<int[]> m_bestRetries; // platform tracks: retries of the best run (-1 = none)
    mutable std::unique_ptr<fx32[]> m_medalTimes; // per race track: bronze, silver, gold (-1: not read yet)
    int m_count = 0;
    int m_finishFrames = -1;  // frames since the finish (-1: not finished)
    bool m_newRecord = false;
    int m_medalsWon = 0, m_coppersWon = 0, m_runMedal = 0; // the last finish

    // Player data (save file).
    int m_coppers = 0;
    std::vector<char> m_ownedTracks, m_ownedSkins, m_ownedBlocks; // config/TrackShop, SkinShop, BlockShop entries
    int m_skin[4] = {0, 0, 0, 0};                  // chosen skin per environment
    int m_camera[4] = {0, 0, 0, 0};                // chase camera per environment (0 low, 1 high)
    int m_language = 0;
    bool m_languageSaved = false;
    bool m_musicOn = true, m_sfxOn = true;
    // The opponents raced (the pause menu's "Concurrents", ov003 0x02130a54, also in the display options): the
    // provided ghost of one medal, all three, or none; and the player's best run ("playerghost").
    enum GhostChoice { GHOSTS_GOLD, GHOSTS_SILVER, GHOSTS_BRONZE, GHOSTS_ALL, GHOSTS_NONE, GHOSTS_COUNT };
    int m_ghostChoice = GHOSTS_GOLD;
    bool m_ghostPlayer = true;
    bool m_ghostMenu = false; // the pause menu's opponent list is open (item = its cursor)
    static const char *GhostChoiceKey(int choice); // ghostgold ... ghostnone (strings/main/backend)
    int m_races = 0, m_finishes = 0;               // statistics
    std::string m_name = "Player";

    // Front end state.
    std::vector<PageState> m_stack;
    int m_mode = MODE_RACE, m_env = 3, m_diff = 1, m_tod = -1, m_shopKind = 0;
    std::string m_popup;
    int m_buy = 0;                          // the shop entry of a purchase awaiting its confirmation
    int m_confirm = 0, m_confirmCursor = 1; // a yes / no popup's action (0: a message), cursor 0 yes / 1 no
    int m_anim = 0, m_animDir = 1;          // page transition frames left, direction
    int m_frame = 0;
};
