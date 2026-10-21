// The track editor (ov002, with the editor modes 6 and 7 of ov003) and the puzzle mode on the PSP's pad.
// The DS builds on its touch screen (drag a block from the block menu, touch to select, rotate, delete, draw);
// here a cursor walks the grid with the d-pad, as the console TrackMania editors do. The content is the DS's: the
// block menus of Scripts/screens/<Env>/TrackPieceLayout and their icons (Editor/Cell/<Env>Blocks, <Env>Folders),
// the placement rules, the budget, the validation messages, the objectives, the bit-packed track files, the
// puzzles of Scenes/puz and config/PuzzleProgression (dsEditorTrack does the work). Port additions: the orbit
// camera on the analog stick, the undo list, the help page with the PSP controls.
//
// Controls: d-pad moves the cursor (relative to the camera), L / R lower / raise it, stick turns and zooms the
// camera; X places the block (on a block, without one: takes it), square turns it, O deletes the block under the
// cursor (or puts the carried one back), triangle opens the block menu, START the editor menu, SELECT the help.
#include "editor.h"

#include <dirent.h>
#ifdef _WIN32
#include <direct.h>
#endif
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#include <algorithm>
#include <map>

#include "fe_text.h"
#include "math/dsMath.h"
#include "script/dsLua.h"
#include "sound/dsCarAudio.h"
#include "sound/dsGameAudio.h"
#include "text/dsStringTable.h"
#include "track/dsPieceData.h"
#include "track/dsRaceSetup.h"

namespace {

const u32 kInk = FeRgba(40, 40, 45), kGrey = FeRgba(150, 150, 155), kBlue = FeRgba(60, 140, 230),
          kWhite = FeRgba(250, 250, 250), kEdge = FeRgba(195, 195, 200), kSoft = FeRgba(90, 90, 100),
          kRed = FeRgba(220, 60, 60);

const char *const kLayoutDirs[4] = {"Sta", "Isl", "Sno", "Coa"};
const char *const kBankNames[4] = {"Stadium", "Island", "Snow", "Coast"};
const char *const kEnvKeys[4] = {"environmentstadiumbutton", "environmentislandbutton", "environmentsnowbutton",
                                 "environmentcoastbutton"};
const char *const kDiffKeys[5] = {"difficultypractice", "difficultyeasy", "difficultymedium", "difficultyhard",
                                  "difficultyextreme"};

enum Confirm { EC_NONE, EC_CLEAR, EC_EXIT, EC_OVERWRITE, EC_LOAD, EC_DELETE };
enum MenuItem {
    M_RESUME, M_DRAW, M_TEST, M_VALIDATE, M_OBJECTIVES, M_SAVE, M_LOAD, M_UNDO, M_CLEAR, M_HELP, M_EXIT
};

// Editor/Cell/<Env>Blocks and <Env>Folders in the environment's palette, loaded once.
G2CellBank *IconBank(int env, bool folders)
{
    static G2CellBank *banks[4][2];
    static bool tried[4][2];
    if (env < 0 || env > 3)
        return nullptr;
    if (!tried[env][folders]) {
        tried[env][folders] = true;
        std::string base = std::string("graphics/2dsprites/common/editor/cell/") + kBankNames[env] +
                           (folders ? "Folders" : "Blocks");
        std::vector<u8> e, g, c;
        G2CellBank *b = new G2CellBank;
        if (RaceReadFile(base + ".ncer", &e) && RaceReadFile(base + ".ncgr", &g) &&
            RaceReadFile(std::string("graphics/2dsprites/common/editor/cell/editor") + kBankNames[env] + ".nclr", &c) &&
            b->Load(g, e, c))
            banks[env][folders] = b;
        else
            delete b;
    }
    return banks[env][folders];
}

void Icon(const FeCanvas &fe, G2CellBank *b, int index, float x, float y, float size, u32 rgba = 0xffffffff)
{
    if (!b || index < 0 || index >= (int)b->cells.size() || !b->cells[index].w)
        return;
    const G2Cell &c = b->cells[index];
    float k = size / (c.w > c.h ? c.w : c.h);
    fe.cell(*b, c, fe.ox + (x + (size - c.w * k) / 2) * fe.scale, fe.oy + (y + (size - c.h * k) / 2) * fe.scale,
            c.w * k * fe.scale, c.h * k * fe.scale, rgba, fe.user);
}

u32 Fade(u32 c, float a) { return (c & 0xffffff) | (u32)((c >> 24) * a) << 24; }

std::string Lower(std::string s)
{
    for (char &c : s)
        c = (char)tolower((unsigned char)c);
    return s;
}

} // namespace

void FeBlockIcon(const FeCanvas &fe, int env, int icon, float x, float y, float size, u32 rgba)
{
    Icon(fe, IconBank(env, false), icon, x, y, size, rgba);
}

int BlockIconOf(int env, const std::string &name)
{
    static std::map<std::string, int> icons[4];
    static bool read[4];
    if (env < 0 || env > 3)
        return -1;
    if (!read[env]) {
        read[env] = true;
        dsBlockMenu menu;
        menu.Load(FeFmt("screens/%s/TrackPieceLayout", kLayoutDirs[env]).c_str());
        for (auto &f : menu.folders)
            for (const dsBlockMenuEntry &b : f.second)
                if (!b.folder)
                    icons[env][Lower(b.name)] = b.icon;
    }
    auto it = icons[env].find(Lower(name));
    return it == icons[env].end() ? -1 : it->second;
}

namespace {


} // namespace

// config/PuzzleProgression: { difficulty, number, puzzle medals required, gold, silver, bronze, coppers } per puzzle,
// in order; puzzle k of a difficulty is Scenes/puz/<difficulty>/<k> (k = its environment).
struct PuzzleEntry {
    std::string diff;
    int need = 0, coppers = 0, number = 0;
    fx32 times[3] = {}; // gold, silver, bronze
    std::string scene;
};

static const std::vector<PuzzleEntry> &Puzzles()
{
    static std::vector<PuzzleEntry> list;
    static bool loaded = false;
    if (loaded)
        return list;
    loaded = true;
    static dsScriptManager scripts;
    dsLuaStateRef st = scripts.Run("config/PuzzleProgression");
    if (!st.get())
        return list;
    dsLuaObject t = st->Global("PuzzleProgression");
    std::map<std::string, int> perDiff;
    for (int i = 0, n = t.Count(); i < n; i++) {
        dsLuaObject e = t.Index(i);
        PuzzleEntry p;
        const char *d = e.IndexString(0);
        p.diff = d ? d : "";
        p.number = e.IndexNumber(1) >> 12;
        p.need = e.IndexNumber(2) >> 12;
        for (int m = 0; m < 3; m++)
            p.times[m] = e.IndexNumber(3 + m);
        p.coppers = e.IndexNumber(6) >> 12;
        p.scene = FeFmt("puz/%s/%d", p.diff.c_str(), perDiff[p.diff]++);
        list.push_back(p);
    }
    return list;
}

int PuzzleCount() { return (int)Puzzles().size(); }

bool ReadTrackFile(const std::string &path, std::vector<u8> *out)
{
    FILE *f = fopen(path.c_str(), "rb");
    if (!f)
        return false;
    out->clear();
    u8 buf[512];
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, f)) > 0)
        out->insert(out->end(), buf, buf + n);
    fclose(f);
    return !out->empty();
}

std::string PuzzleName(int n)
{
    const std::vector<PuzzleEntry> &p = Puzzles();
    if (n < 0 || n >= (int)p.size())
        return FeS("puzzlemode");
    int k = 0;
    for (int i = 0; i < n; i++)
        k += p[(size_t)i].diff == p[(size_t)n].diff;
    static const char *const kDiffs[5] = {"practice", "easy", "medium", "hard", "extreme"};
    int d = 0;
    while (d < 4 && p[(size_t)n].diff != kDiffs[d])
        d++;
    return FeFmt("%s %s %d", FeS("puzzlemode"), FeS(kDiffKeys[d]), k + 1);
}

bool PuzzleInfo(int n, std::string *diff, int *need, fx32 times[3], int *coppers)
{
    const std::vector<PuzzleEntry> &p = Puzzles();
    if (n < 0 || n >= (int)p.size())
        return false;
    *diff = p[(size_t)n].diff;
    *need = p[(size_t)n].need;
    for (int m = 0; m < 3; m++)
        times[m] = p[(size_t)n].times[m];
    *coppers = p[(size_t)n].coppers;
    return true;
}

// ---------------------------------------------------------------------------------------------------------------

bool RaceApp::OpenEditor(int env, int puzzle)
{
    const std::vector<PuzzleEntry> &puz = Puzzles();
    if (puzzle >= (int)puz.size())
        return false;
    std::string scene;
    if (puzzle >= 0) {
        scene = puz[(size_t)puzzle].scene;
        dsTrack header;
        dsRaceSetup::Get()->mode = 7;
        if (!header.LoadScene(scene.c_str(), false))
            return false;
        env = header.info.environment;
    }
    if (env < 0 || env > 3)
        return false;
    Unload();
    m_ed.reset(new Editor);
    Editor &e = *m_ed;
    e.env = env;
    e.puzzle = puzzle;
    dsRaceSetup::Get()->mode = puzzle >= 0 ? 7 : 6;
    e.ed.reset(new dsEditorTrack(env));
    e.ed->puzzle = puzzle >= 0;
    if (puzzle >= 0) {
        e.ed->LoadScene(scene.c_str(), true);
        e.menu.Load((scene + "-Layout").c_str());
        if (const std::vector<dsBlockMenuEntry> *top = e.menu.Folder("TopFolder"))
            for (const dsBlockMenuEntry &b : *top)
                e.stock.push_back({b.id, b.count});
    } else {
        e.menu.Load(FeFmt("screens/%s/TrackPieceLayout", kLayoutDirs[env]).c_str());
        e.ed->header.author[0] = 0;
        snprintf(e.ed->header.author, sizeof e.ed->header.author, "%s", m_name.c_str());
    }
    e.folders.assign(1, "TopFolder");
    e.folderCursor.assign(1, 0);
    e.cur[0] = (s32)e.ed->gridSize[0] / 2;
    e.cur[2] = (s32)e.ed->gridSize[2] / 2;
    e.cur[1] = dsPieceDataTable::Get()->GroundLevel();
    if (dsTrackPiece *s = e.ed->StartPiece()) {
        e.cur[0] = s->cell[0];
        e.cur[1] = s->cell[1];
        e.cur[2] = s->cell[2];
    }
    race.reset(new RaceSession);
    race->lighting = lighting;
    if (!race->LoadView(env, e.ed->track.info.timeOfDay)) {
        race.reset();
        m_ed.reset();
        return false;
    }
    race->camIndex = 0;
    dsGameAudio *ga = dsGameAudio::Get();
    ga->PlayMusic(dsGameAudio::MUSIC_NONE, 0);
    ga->StartEditor();
    screen = EDITING;
    m_run = RUN_TRACK;
    EditorSync();
    for (int i = 0; i < 3; i++)
        e.target[i] = 0;
    EditorFrame();
    if (puzzle >= 0)
        EditorMessage(FeS("helppuzzle1"));
    return true;
}

void RaceApp::EditorScript(const char *keys)
{
    if (m_ed && keys) {
        m_ed->script = keys;
        m_ed->scriptPos = 0;
    }
}

void RaceApp::EditorMessage(const std::string &text)
{
    m_ed->message = text;
    m_ed->messageFrames = 120;
}

void RaceApp::EditorSync()
{
    Editor &e = *m_ed;
    if (race)
        race->SyncPieces(e.ed->track.pieces);
    EditorPreview();
}

static int StockOf(const RaceApp::Editor &e, u32 id)
{
    for (const auto &s : e.stock)
        if (s.first == id)
            return s.second;
    return 0;
}

static void AddStock(RaceApp::Editor &e, u32 id, int n)
{
    for (auto &s : e.stock)
        if (s.first == id)
            s.second += n;
}

void RaceApp::EditorPreview()
{
    Editor &e = *m_ed;
    e.preview.reset();
    if (e.mode == Editor::MODE_CAR) {
        // The DS's test arrow: piece id -1, Tracks/<env>/TestArrow (ov002 0x020df178 creates it).
        e.preview.reset(new dsTrackPiece(-1, dsEditorTrack::CellPosition(e.env, e.cur[0], e.cur[1], e.cur[2]),
                                         (u8)e.carRot));
        e.previewOk = true;
        return;
    }
    if (!e.block)
        return;
    e.preview.reset(new dsTrackPiece((s32)e.block, dsEditorTrack::CellPosition(e.env, e.cur[0], e.cur[1], e.cur[2]),
                                     (u8)e.rot));
    e.previewOk = e.ed->CanPlace(e.block, e.rot, e.cur[0], e.cur[1], e.cur[2]) == dsEditorTrack::PLACED &&
                  (e.puzzle < 0 || StockOf(e, e.block) > 0);
}

void RaceApp::EditorPlace()
{
    Editor &e = *m_ed;
    if (e.puzzle >= 0 && StockOf(e, e.block) <= 0) {
        Sound("MENU_ERROR");
        return;
    }
    dsEditorTrack::Snapshot snap = e.ed->Save();
    std::vector<u32> before;
    for (const dsTrackPiece *p : e.ed->track.pieces)
        if (!p->autoTerrain)
            before.push_back((u32)p->id);
    dsEditorTrack::PlaceResult r = e.ed->Place(e.block, e.rot, e.cur[0], e.cur[1], e.cur[2]);
    if (r != dsEditorTrack::PLACED) {
        Sound("MENU_ERROR");
        if (r == dsEditorTrack::FULL)
            EditorMessage(FeL("M\xe9""moire de l'\xe9""diteur pleine : supprimez des blocs.",
                              "The editor's memory is full: delete some blocks."));
        else if (r == dsEditorTrack::LOCKED)
            EditorMessage(FeL("Ce bloc du puzzle ne peut pas \xea""tre remplac\xe9"".",
                              "This puzzle block cannot be replaced."));
        else
            EditorMessage(FeL("Hors de la zone de construction.", "Outside the building area."));
        return;
    }
    // Puzzle: the replaced blocks go back to the stock, the new one is taken from it.
    if (e.puzzle >= 0) {
        std::vector<u32> after;
        for (const dsTrackPiece *p : e.ed->track.pieces)
            if (!p->autoTerrain)
                after.push_back((u32)p->id);
        for (u32 id : before) {
            auto it = std::find(after.begin(), after.end(), id);
            if (it != after.end())
                after.erase(it);
            else
                AddStock(e, id, 1);
        }
        AddStock(e, e.block, -1);
    }
    e.undo.push_back(snap);
    if (e.undo.size() > 40)
        e.undo.erase(e.undo.begin());
    e.modified = true;
    e.validated = false;
    Sound("EDITOR_STAMP");
    EditorSync();
}

void RaceApp::EditorDelete()
{
    Editor &e = *m_ed;
    dsTrackPiece *p = e.ed->PieceAt(e.cur[0], e.cur[1], e.cur[2]);
    if (!p || p->autoTerrain)
        return;
    if (e.ed->IsLocked(p)) {
        Sound("MENU_ERROR");
        return;
    }
    dsEditorTrack::Snapshot snap = e.ed->Save();
    u32 id = (u32)p->id;
    if (!e.ed->Remove(p))
        return;
    AddStock(e, id, 1);
    e.undo.push_back(snap);
    e.modified = true;
    e.validated = false;
    Sound("EDITOR_DELETE");
    EditorSync();
}

// The cursor's world box, its column and the building area at the cursor's height, and the carried block.
void RaceApp::EditorFrame()
{
    if (!m_ed || !race)
        return;
    Editor &e = *m_ed;
    if (e.messageFrames > 0)
        e.messageFrames--;
    const dsRaceSetup *setup = dsRaceSetup::Get();
    float cw = setup->CellWidthFx() / 4096.0f, ch = setup->CellHeightFx() / 4096.0f, half = cw / 2;
    const float base = 0x59a / 4096.0f;
    float tx = e.cur[0] * cw, ty = e.cur[1] * ch + base, tz = e.cur[2] * cw;
    float goal[3] = {tx, ty, tz};
    for (int i = 0; i < 3; i++)
        e.target[i] = e.target[i] == 0 ? goal[i] : e.target[i] + (goal[i] - e.target[i]) * 0.35f;
    // Orbit camera: eye above and behind the target; the chase camera slot the drivers read.
    float cp = cosf(e.pitch), eye[3] = {e.target[0] + e.dist * cp * sinf(e.yaw), e.target[1] + e.dist * sinf(e.pitch),
                                        e.target[2] + e.dist * cp * cosf(e.yaw)};
    float f[3] = {e.target[0] - eye[0], e.target[1] - eye[1], e.target[2] - eye[2]};
    float fl = sqrtf(f[0] * f[0] + f[1] * f[1] + f[2] * f[2]);
    for (float &v : f)
        v /= fl;
    float r[3] = {-f[2], 0, f[0]};
    float rl = sqrtf(r[0] * r[0] + r[2] * r[2]);
    r[0] /= rl;
    r[2] /= rl;
    float u[3] = {r[1] * f[2] - r[2] * f[1], r[2] * f[0] - r[0] * f[2], r[0] * f[1] - r[1] * f[0]};
    MtxFx43 &m = race->cams[0].matrix;
    for (int i = 0; i < 3; i++) {
        m.m[0][i] = (fx32)lrintf(r[i] * 4096);
        m.m[1][i] = (fx32)lrintf(u[i] * 4096);
        m.m[2][i] = (fx32)lrintf(f[i] * 4096);
        m.m[3][i] = (fx32)lrintf(eye[i] * 4096);
    }
    race->camIndex = 0;

    // Lines.
    race->lines.clear();
    auto line = [&](float x0, float y0, float z0, float x1, float y1, float z1, u32 c) {
        RaceSession::Line l = {{x0, y0, z0}, {x1, y1, z1}, c};
        race->lines.push_back(l);
    };
    auto box = [&](float x0, float y0, float z0, float x1, float y1, float z1, u32 c) {
        line(x0, y0, z0, x1, y0, z0, c);
        line(x1, y0, z0, x1, y0, z1, c);
        line(x1, y0, z1, x0, y0, z1, c);
        line(x0, y0, z1, x0, y0, z0, c);
        line(x0, y1, z0, x1, y1, z0, c);
        line(x1, y1, z0, x1, y1, z1, c);
        line(x1, y1, z1, x0, y1, z1, c);
        line(x0, y1, z1, x0, y1, z0, c);
        line(x0, y0, z0, x0, y1, z0, c);
        line(x1, y0, z0, x1, y1, z0, c);
        line(x1, y0, z1, x1, y1, z1, c);
        line(x0, y0, z1, x0, y1, z1, c);
    };
    // The building area at the cursor's height (grid border excluded on Island and Coast).
    int b = e.ed->border ? 1 : 0;
    float ax0 = b * cw - half, ax1 = (e.ed->gridSize[0] - 1 - b) * cw + half;
    float az0 = b * cw - half, az1 = (e.ed->gridSize[2] - 1 - b) * cw + half;
    u32 areaColor = FeRgba(255, 255, 255, 140);
    line(ax0, ty, az0, ax1, ty, az0, areaColor);
    line(ax1, ty, az0, ax1, ty, az1, areaColor);
    line(ax1, ty, az1, ax0, ty, az1, areaColor);
    line(ax0, ty, az1, ax0, ty, az0, areaColor);
    // The cursor: the carried block's footprint, else the block under the cursor, else the cell.
    const dsPieceInfo *info = e.block ? dsPieceDataTable::Get()->FindById(e.block) : nullptr;
    dsTrackPiece *under = e.block ? nullptr : e.ed->PieceAt(e.cur[0], e.cur[1], e.cur[2]);
    std::vector<s32> cells;
    if (info)
        e.ed->Footprint(*info, e.rot, e.cur[0], e.cur[1], e.cur[2], false, &cells);
    else if (under && under->info)
        e.ed->Footprint(*under->info, under->rotation, under->cell[0], under->cell[1], under->cell[2], false, &cells);
    s32 lo[3] = {e.cur[0], e.cur[1], e.cur[2]}, hi[3] = {e.cur[0], e.cur[1], e.cur[2]};
    for (s32 c : cells) {
        s32 x = c % (s32)e.ed->gridSize[0], rest = c / (s32)e.ed->gridSize[0];
        s32 z = rest % (s32)e.ed->gridSize[2], y = rest / (s32)e.ed->gridSize[2];
        s32 v[3] = {x, y, z};
        for (int i = 0; i < 3; i++) {
            lo[i] = std::min<s32>(lo[i], v[i]);
            hi[i] = std::max<s32>(hi[i], v[i]);
        }
    }
    u32 color = info ? (e.previewOk ? FeRgba(80, 200, 255) : FeRgba(255, 70, 60))
                     : under ? (e.ed->IsLocked(under) ? FeRgba(255, 160, 40) : FeRgba(255, 230, 60))
                             : FeRgba(255, 255, 255);
    box(lo[0] * cw - half, lo[1] * ch + base, lo[2] * cw - half, hi[0] * cw + half, (hi[1] + 1) * ch + base,
        hi[2] * cw + half, color);
    // A grid of the cells around the cursor, at its height.
    const int R = 4;
    u32 gridColor = FeRgba(255, 255, 255, 70);
    for (int i = -R; i <= R + 1; i++) {
        float gx = (e.cur[0] + i) * cw - half, gz = (e.cur[2] + i) * cw - half;
        line(gx, ty, (e.cur[2] - R) * cw - half, gx, ty, (e.cur[2] + R) * cw + half, gridColor);
        line((e.cur[0] - R) * cw - half, ty, gz, (e.cur[0] + R) * cw + half, ty, gz, gridColor);
    }
    // The column down to the ground, for the height.
    if (e.cur[1] > 0)
        line(tx, base, tz, tx, ty, tz, FeRgba(255, 255, 255, 180));

    race->editorDraws.clear();
    // The direction markers of the start, finish, checkpoint and multilap blocks (the DS shows them in the editor).
    for (const dsTrackPiece *p : e.ed->track.pieces) {
        dsTrackPiece::Marker mk[2];
        for (int i = 0, n = p->EditorMarkers(mk); i < n; i++)
            if (G3Model *mm = race->PieceModel(mk[i].model))
                if (race->editorDraws.size() + 1 < RaceSession::MAX_EDITOR_DRAWS)
                    race->editorDraws.push_back(race->PieceDraw(mm, mk[i].matrix, p->rotation, 4));
    }
    if (e.preview) {
        G3Model *pm = race->PieceModel(e.preview->modelPath);
        if (pm)
            race->editorDraws.push_back(race->PieceDraw(pm, e.preview->matrix, e.preview->rotation,
                                                        e.previewOk ? 2 : 3));
    }
}

void RaceApp::EditorPad(u32 held, float sx, float sy)
{
    if (!m_ed || screen != EDITING)
        return;
    Editor &e = *m_ed;
    // Scripted presses (captures): one per frame, released the next one; '[' presses X and keeps it held until ']'
    // (free draw strokes).
    if (e.scriptPos < e.script.size()) {
        static const char kKeys[] = "UDLRXOSTlrPE";
        char c = e.script[e.scriptPos++];
        const char *k = strchr(kKeys, c);
        u32 key = k && c ? 1u << (k - kKeys) : 0;
        if (c == '[')
            e.scriptHold = key = PAD_CROSS;
        else if (c == ']')
            e.scriptHold = 0;
        held = key | e.scriptHold;
        e.prevHeld &= ~(key & e.prevHeld); // the same key twice in a row: two presses
    }
    u32 pressed = held & ~e.prevHeld;
    // D-pad and shoulders with auto-repeat after 10 frames, every 3 (as the menus).
    const u32 rep = PAD_UP | PAD_DOWN | PAD_LEFT | PAD_RIGHT | PAD_L | PAD_R;
    e.repeat = held & rep ? e.repeat + 1 : 0;
    if (e.repeat > 10 && e.repeat % 3 == 0)
        pressed |= held & rep;
    e.prevHeld = held;
    if (e.overlay != Editor::O_NONE || e.confirm)
        EditorMenuKey(pressed);
    else
        EditorKey(pressed, held);
    if (!m_ed || screen != EDITING)
        return;
    // Camera on the stick (outside the overlays): turn and zoom.
    if (m_ed->overlay == Editor::O_NONE) {
        auto dead = [](float v) { return fabsf(v) < 0.2f ? 0.0f : (v > 0 ? v - 0.2f : v + 0.2f) / 0.8f; };
        m_ed->yaw += dead(sx) * 0.07f;
        m_ed->dist = std::min(400.0f, std::max(20.0f, m_ed->dist * (1 + dead(sy) * 0.04f)));
    }
}

void RaceApp::EditorKey(u32 pressed, u32 held)
{
    (void)held;
    Editor &e = *m_ed;
    if (e.menuOpen) {
        const std::vector<dsBlockMenuEntry> *f = e.menu.Folder(e.folders.back());
        int n = f ? (int)f->size() : 0;
        int &c = e.folderCursor.back();
        if (pressed & PAD_LEFT && n) {
            c = (c + n - 1) % n;
            Sound("MENU_MOVE");
        }
        if (pressed & PAD_RIGHT && n) {
            c = (c + 1) % n;
            Sound("MENU_MOVE");
        }
        if (pressed & (PAD_UP | PAD_CIRCLE)) {
            Sound("MENU_BACK");
            if (e.folders.size() > 1) { // "Press Up to backtrack"
                e.folders.pop_back();
                e.folderCursor.pop_back();
            } else if (pressed & PAD_CIRCLE) {
                e.menuOpen = false;
            }
        }
        if (pressed & PAD_TRIANGLE) {
            e.menuOpen = false;
            Sound("MENU_BACK");
        }
        if (pressed & (PAD_CROSS | PAD_DOWN) && n) {
            const dsBlockMenuEntry &b = (*f)[(size_t)c];
            if (b.folder) {
                Sound("MENU_SELECT");
                e.folders.push_back(b.name);
                e.folderCursor.push_back(0);
            } else if (b.id && e.puzzle < 0 && BlockLocked(e.env, b.name)) {
                Sound("MENU_ERROR");
                EditorMessage(FeS("lockedrequirementpurchase") + std::string(" (") + FeS("shopblocksbutton") + ")");
            } else if (b.id) {
                Sound("EDITOR_PIECE_SELECT");
                e.block = b.id;
                e.blockIcon = b.icon;
                e.blockName = b.name;
                e.menuOpen = false;
                EditorPreview();
            }
        }
        return;
    }
    bool moved = false;
    if (pressed & (PAD_UP | PAD_DOWN | PAD_LEFT | PAD_RIGHT | PAD_L | PAD_R)) {
        // Directions relative to the camera, snapped to the grid's axes.
        float fx = -sinf(e.yaw), fz = -cosf(e.yaw);
        int ax, az;
        if (fabsf(fx) > fabsf(fz)) {
            ax = fx > 0 ? 1 : -1;
            az = 0;
        } else {
            ax = 0;
            az = fz > 0 ? 1 : -1;
        }
        int dx = 0, dz = 0, dy = 0;
        if (pressed & PAD_UP) {
            dx += ax;
            dz += az;
        }
        if (pressed & PAD_DOWN) {
            dx -= ax;
            dz -= az;
        }
        if (pressed & PAD_RIGHT) { // right = forward turned a quarter clockwise seen from above
            dx += -az;
            dz += ax;
        }
        if (pressed & PAD_LEFT) {
            dx -= -az;
            dz -= ax;
        }
        if (pressed & PAD_R)
            dy++;
        if (pressed & PAD_L)
            dy--;
        s32 n[3] = {e.cur[0] + dx, e.cur[1] + dy, e.cur[2] + dz};
        for (int i = 0; i < 3; i++)
            n[i] = std::max<s32>(0, std::min<s32>(n[i], (s32)e.ed->gridSize[i] - 1));
        if (n[0] != e.cur[0] || n[1] != e.cur[1] || n[2] != e.cur[2]) {
            for (int i = 0; i < 3; i++)
                e.cur[i] = n[i];
            Sound("MENU_MOVE");
            EditorPreview();
            moved = true;
        }
    }
    if (e.mode != Editor::MODE_BUILD) {
        EditorModeKey(pressed, held, moved);
        return;
    }
    if (pressed & PAD_SQUARE && e.block) {
        e.rot = (e.rot + 1) & 3;
        Sound("EDITOR_ROTATE");
        EditorPreview();
    }
    if (pressed & PAD_CROSS) {
        if (e.block) {
            EditorPlace();
        } else if (dsTrackPiece *p = e.ed->PieceAt(e.cur[0], e.cur[1], e.cur[2])) {
            // Select: the block leaves the track and follows the cursor from its origin cell.
            if (p->autoTerrain || e.ed->IsLocked(p)) {
                Sound("MENU_ERROR");
            } else {
                dsEditorTrack::Snapshot snap = e.ed->Save();
                e.block = (u32)p->id;
                e.rot = p->rotation;
                e.blockName = p->info ? p->info->name : "";
                e.blockIcon = -1;
                for (auto &f : e.menu.folders)
                    for (const dsBlockMenuEntry &b : f.second)
                        if (!b.folder && b.id == e.block)
                            e.blockIcon = b.icon;
                for (int i = 0; i < 3; i++)
                    e.cur[i] = p->cell[i];
                e.ed->Remove(p);
                AddStock(e, e.block, 1);
                e.undo.push_back(snap);
                e.modified = true;
                e.validated = false;
                Sound("EDITOR_PIECE_SELECT");
                EditorSync();
            }
        }
    }
    if (pressed & PAD_CIRCLE) {
        if (e.block) {
            e.block = 0;
            Sound("MENU_BACK");
            EditorPreview();
        } else {
            EditorDelete();
        }
    }
    if (pressed & PAD_TRIANGLE) {
        e.menuOpen = true;
        Sound("MENU_SELECT");
    }
    if (pressed & PAD_START) {
        e.overlay = Editor::O_MENU;
        e.cursor = 0;
        Sound("MENU_SELECT");
    }
    if (pressed & PAD_SELECT) {
        e.overlay = Editor::O_HELP;
        Sound("MENU_SELECT");
    }
}

// Free draw and the test vehicle's placement: the cursor moves as when building.
void RaceApp::EditorModeKey(u32 pressed, u32 held, bool moved)
{
    Editor &e = *m_ed;
    if (e.mode == Editor::MODE_DRAW) {
        bool stepped = false;
        if (pressed & PAD_CROSS) {
            e.undo.push_back(e.ed->Save());
            e.ed->FreeDrawBegin();
            e.stroke = true;
            stepped = e.ed->FreeDrawStep(e.cur[0], e.cur[1], e.cur[2]);
        } else if (e.stroke && (held & PAD_CROSS) && moved) {
            stepped = e.ed->FreeDrawStep(e.cur[0], e.cur[1], e.cur[2]);
        }
        if (!(held & PAD_CROSS))
            e.stroke = false;
        if (stepped) {
            e.modified = true;
            e.validated = false;
            Sound("EDITOR_STAMP");
            EditorSync();
        }
        if (pressed & PAD_CIRCLE) {
            e.mode = Editor::MODE_BUILD;
            Sound("MENU_BACK");
        }
        if (pressed & PAD_TRIANGLE) {
            e.mode = Editor::MODE_BUILD;
            e.menuOpen = true;
            Sound("MENU_SELECT");
        }
    } else {
        if (pressed & PAD_SQUARE) {
            e.carRot = (e.carRot + 1) & 3;
            Sound("EDITOR_ROTATE");
            EditorPreview();
        }
        if (pressed & PAD_CIRCLE) {
            e.mode = Editor::MODE_BUILD;
            Sound("MENU_BACK");
            EditorPreview();
        }
        if (pressed & PAD_CROSS) {
            for (int i = 0; i < 3; i++)
                e.carCell[i] = e.cur[i];
            e.carPlaced = true;
            Sound("MENU_SELECT");
            EditorRace(false);
            return;
        }
    }
    if (pressed & PAD_START) {
        e.overlay = Editor::O_MENU;
        e.cursor = 0;
        Sound("MENU_SELECT");
    }
    if (pressed & PAD_SELECT) {
        e.overlay = Editor::O_HELP;
        Sound("MENU_SELECT");
    }
}

static std::vector<int> EditorMenuItems(const RaceApp::Editor &e)
{
    if (e.puzzle >= 0)
        return {M_RESUME, M_VALIDATE, M_UNDO, M_CLEAR, M_HELP, M_EXIT};
    return {M_RESUME, M_DRAW, M_TEST, M_VALIDATE, M_OBJECTIVES, M_SAVE, M_LOAD, M_UNDO, M_CLEAR, M_HELP, M_EXIT};
}

static const char *MenuLabel(int item)
{
    switch (item) {
    case M_RESUME: return FeS("resumemsg");
    case M_DRAW: return FeS("helpfreedrawtitle");
    case M_TEST: return FeS("helptesttitle");
    case M_VALIDATE: return FeS("helpvalidatetitle");
    case M_OBJECTIVES: return FeS("helpobjectivestitle");
    case M_SAVE: return FeS("helpsavetitle");
    case M_LOAD: return FeS("helploadtitle");
    case M_UNDO: return FeL("Annuler", "Undo");
    case M_CLEAR: return FeS("helpclearalltitle");
    case M_HELP: return FeL("Aide", "Help");
    default: return FeS("helpexittitle");
    }
}

// The name entry's keys: four rows of ten characters, then delete and OK.
static const char kNameKeys[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789 -_.";
static const int NAME_KEYS = 40, NAME_COLS = 10;

void RaceApp::EditorMenuKey(u32 pressed)
{
    Editor &e = *m_ed;
    if (e.confirm) {
        if (pressed & (PAD_LEFT | PAD_RIGHT)) {
            e.confirmCursor ^= 1;
            Sound("MENU_MOVE");
        }
        if (pressed & (PAD_CROSS | PAD_CIRCLE)) {
            bool yes = (pressed & PAD_CROSS) && e.confirmCursor == 0;
            int what = e.confirm;
            e.confirm = 0;
            Sound(pressed & PAD_CROSS ? "MENU_SELECT" : "MENU_BACK");
            if (what == EC_CLEAR && yes) {
                e.undo.push_back(e.ed->Save());
                for (const dsTrackPiece *p : e.ed->track.pieces)
                    if (!p->autoTerrain && !e.ed->IsLocked(p))
                        AddStock(e, (u32)p->id, 1);
                e.ed->Clear();
                e.modified = true;
                e.validated = false;
                e.overlay = Editor::O_NONE;
                EditorSync();
            } else if (what == EC_EXIT) {
                // "Track has been modified. Would you like to save changes before quitting?"
                if (yes) {
                    if (e.file.empty()) {
                        e.overlay = Editor::O_NAME;
                        e.entry.clear();
                        e.cursor = 0;
                        e.confirm = 0;
                        e.run = -1; // leave once saved
                    } else if (EditorSave(e.file, true)) {
                        EditorClose();
                    }
                } else if (pressed & PAD_CROSS) {
                    EditorClose();
                }
            } else if (what == EC_OVERWRITE && yes) {
                if (EditorSave(e.entry, true)) {
                    e.overlay = Editor::O_NONE;
                    if (e.run == -1)
                        EditorClose();
                }
            } else if (what == EC_LOAD && yes) {
                if (e.cursor < (int)e.files.size())
                    EditorLoad(e.files[(size_t)e.cursor]);
                e.overlay = Editor::O_NONE;
            } else if (what == EC_DELETE && yes) {
                if (e.cursor < (int)e.files.size()) {
                    remove((TracksDir() + "/" + e.files[(size_t)e.cursor] + ".trk").c_str());
                    m_savedRead = false;
                    e.files = SavedTracks(e.env);
                    e.cursor = std::min(e.cursor, std::max(0, (int)e.files.size() - 1));
                }
            }
        }
        return;
    }
    switch (e.overlay) {
    case Editor::O_MENU: {
        std::vector<int> items = EditorMenuItems(e);
        int n = (int)items.size();
        if (pressed & PAD_UP) {
            e.cursor = (e.cursor + n - 1) % n;
            Sound("MENU_MOVE");
        }
        if (pressed & PAD_DOWN) {
            e.cursor = (e.cursor + 1) % n;
            Sound("MENU_MOVE");
        }
        if (pressed & (PAD_CIRCLE | PAD_START)) {
            e.overlay = Editor::O_NONE;
            Sound("MENU_BACK");
        }
        if (!(pressed & PAD_CROSS))
            break;
        Sound("MENU_SELECT");
        switch (items[(size_t)e.cursor]) {
        case M_RESUME: e.overlay = Editor::O_NONE; break;
        case M_DRAW:
            e.overlay = Editor::O_NONE;
            e.mode = Editor::MODE_DRAW;
            e.block = 0;
            e.stroke = false;
            e.ed->FreeDrawBegin();
            EditorPreview();
            // (the DS's helpfreedraw1 speaks of the stylus)
            EditorMessage(FeL("Maintenez X et d\xe9""placez le curseur pour tracer la route.",
                              "Hold X and move the cursor to draw the road."));
            break;
        case M_TEST:
            // "Placing Vehicle...": the arrow where it was last, else at the cursor; X starts the test drive.
            e.overlay = Editor::O_NONE;
            e.mode = Editor::MODE_CAR;
            e.block = 0;
            if (e.carPlaced)
                for (int i = 0; i < 3; i++)
                    e.cur[i] = e.carCell[i];
            EditorPreview();
            break;
        case M_VALIDATE: {
            static const char *const kChecks[] = {"", "", "validate1startpiece", "validateonly1start",
                                                  "validate1finishpiece", "validate1multilapnostartfinish"};
            dsEditorCheck c = e.ed->Validate();
            if (c != CHECK_OK) {
                Sound("MENU_ERROR");
                EditorMessage(FeS(kChecks[c]));
                e.overlay = Editor::O_NONE;
                break;
            }
            e.overlay = Editor::O_NONE;
            EditorRace(true);
            break;
        }
        case M_OBJECTIVES:
            e.ed->Validate(); // the lap count of a multilap track
            e.overlay = Editor::O_OBJECTIVES;
            e.cursor = 0;
            break;
        case M_SAVE:
            if (TracksDir().empty()) {
                EditorMessage(FeL("Pas de dossier de sauvegarde.", "No save folder."));
                e.overlay = Editor::O_NONE;
                break;
            }
            e.overlay = Editor::O_NAME;
            e.entry = e.file;
            e.cursor = 0;
            e.run = 0;
            break;
        case M_LOAD:
            e.files = SavedTracks(e.env);
            e.overlay = Editor::O_LOAD;
            e.cursor = 0;
            break;
        case M_UNDO:
            if (!e.undo.empty()) {
                // Puzzle: the stock follows the pieces.
                if (e.puzzle >= 0) {
                    for (const dsTrackPiece *p : e.ed->track.pieces)
                        if (!p->autoTerrain && !e.ed->IsLocked(p))
                            AddStock(e, (u32)p->id, 1);
                    for (const dsEditorTrack::Snapshot::P &p : e.undo.back().pieces)
                        if (!p.locked)
                            AddStock(e, p.id, -1);
                }
                e.ed->Restore(e.undo.back());
                e.undo.pop_back();
                e.modified = true;
                e.validated = false;
                EditorSync();
            } else {
                Sound("MENU_ERROR");
            }
            e.overlay = Editor::O_NONE;
            break;
        case M_CLEAR:
            e.confirm = EC_CLEAR;
            e.confirmCursor = 1;
            break;
        case M_HELP: e.overlay = Editor::O_HELP; break;
        default:
            if (e.modified && e.puzzle < 0) {
                e.confirm = EC_EXIT;
                e.confirmCursor = 0;
            } else {
                EditorClose();
            }
            break;
        }
        break;
    }
    case Editor::O_OBJECTIVES: {
        // Challenge type (race / platform), difficulty, laps (multilap tracks: 2..30), as ov002 0x020e3d68.
        dsEditorTrack::Header &h = e.ed->header;
        dsTrackPiece *s = e.ed->StartPiece();
        bool multilap = s && s->type == PIECE_MULTILAP;
        int rows = multilap ? 3 : 2;
        if (pressed & PAD_UP) {
            e.cursor = (e.cursor + rows - 1) % rows;
            Sound("MENU_MOVE");
        }
        if (pressed & PAD_DOWN) {
            e.cursor = (e.cursor + 1) % rows;
            Sound("MENU_MOVE");
        }
        int d = pressed & PAD_LEFT ? -1 : pressed & PAD_RIGHT ? 1 : 0;
        if (d) {
            Sound("MENU_MOVE");
            if (e.cursor == 0)
                h.platform ^= 1;
            else if (e.cursor == 1)
                h.difficulty = (u8)((h.difficulty + 5 + d) % 5);
            else
                h.lapCount = (u8)std::max(2, std::min(30, h.lapCount + d));
            e.ed->track.info.lapCount = h.lapCount;
            e.modified = true;
            e.validated = false;
        }
        if (pressed & (PAD_CROSS | PAD_CIRCLE | PAD_START)) {
            e.overlay = Editor::O_NONE;
            Sound("MENU_BACK");
        }
        break;
    }
    case Editor::O_LOAD: {
        int n = (int)e.files.size();
        if (n && pressed & PAD_UP) {
            e.cursor = (e.cursor + n - 1) % n;
            Sound("MENU_MOVE");
        }
        if (n && pressed & PAD_DOWN) {
            e.cursor = (e.cursor + 1) % n;
            Sound("MENU_MOVE");
        }
        if (pressed & PAD_CIRCLE) {
            e.overlay = Editor::O_NONE;
            Sound("MENU_BACK");
        }
        if (n && pressed & PAD_SQUARE) { // delete the slot ("CustomDataDeleteButton")
            e.confirm = EC_DELETE;
            e.confirmCursor = 1;
        }
        if (n && pressed & PAD_CROSS) {
            Sound("MENU_SELECT");
            if (e.modified) {
                e.confirm = EC_LOAD;
                e.confirmCursor = 1;
            } else {
                EditorLoad(e.files[(size_t)e.cursor]);
                e.overlay = Editor::O_NONE;
            }
        }
        break;
    }
    case Editor::O_NAME: {
        int keys = NAME_KEYS + 2; // + delete, OK
        int c = e.cursor;
        if (pressed & PAD_LEFT)
            c = c % NAME_COLS ? c - 1 : std::min(c + NAME_COLS - 1, keys - 1);
        if (pressed & PAD_RIGHT)
            c = c % NAME_COLS < NAME_COLS - 1 && c + 1 < keys ? c + 1 : c - c % NAME_COLS;
        if (pressed & PAD_UP)
            c = c >= NAME_COLS ? c - NAME_COLS : std::min(c + NAME_KEYS, keys - 1);
        if (pressed & PAD_DOWN)
            c = c + NAME_COLS < keys ? c + NAME_COLS : c % NAME_COLS < 2 ? NAME_KEYS + c % NAME_COLS : c % NAME_COLS;
        if (c != e.cursor) {
            e.cursor = c;
            Sound("MENU_MOVE");
        }
        bool ok = pressed & PAD_START || (pressed & PAD_CROSS && e.cursor == NAME_KEYS + 1);
        if (pressed & PAD_CROSS && e.cursor < NAME_KEYS) {
            if (e.entry.size() < 12) {
                e.entry += kNameKeys[e.cursor];
                Sound("MENU_KEYPRESS");
            } else {
                Sound("MENU_ERROR");
            }
        } else if ((pressed & PAD_CROSS && e.cursor == NAME_KEYS) || pressed & PAD_SQUARE) {
            if (!e.entry.empty())
                e.entry.pop_back();
            Sound("MENU_BACK");
        } else if (pressed & PAD_CIRCLE) {
            Sound("MENU_BACK");
            if (!e.entry.empty())
                e.entry.pop_back();
            else
                e.overlay = Editor::O_NONE;
        }
        if (ok) {
            while (!e.entry.empty() && e.entry.back() == ' ')
                e.entry.pop_back();
            if (e.entry.empty()) {
                Sound("MENU_ERROR");
                break;
            }
            Sound("MENU_SELECT");
            std::vector<std::string> all = SavedTracks(-1);
            bool used = std::find(all.begin(), all.end(), e.entry) != all.end();
            if (used && e.entry != e.file) {
                e.confirm = EC_OVERWRITE; // "Data already exists" / "Overwrite existing data?"
                e.confirmCursor = 1;
            } else if (EditorSave(e.entry, true)) {
                e.overlay = Editor::O_NONE;
                if (e.run == -1)
                    EditorClose();
            }
        }
        break;
    }
    case Editor::O_HELP:
        if (pressed & (PAD_CROSS | PAD_CIRCLE | PAD_SELECT | PAD_START)) {
            e.overlay = Editor::O_NONE;
            Sound("MENU_BACK");
        }
        break;
    default: break;
    }
}

std::string RaceApp::TracksDir() const
{
    if (m_savePath.empty())
        return "";
    size_t slash = m_savePath.find_last_of("/\\");
    return (slash == std::string::npos ? std::string(".") : m_savePath.substr(0, slash)) + "/tracks";
}

const std::vector<RaceApp::SavedTrack> &RaceApp::SavedList() const
{
    if (m_savedRead)
        return m_saved;
    m_savedRead = true;
    m_saved.clear();
    std::string dir = TracksDir();
    DIR *d = dir.empty() ? nullptr : opendir(dir.c_str());
    if (!d)
        return m_saved;
    while (dirent *de = readdir(d)) {
        std::string n = de->d_name;
        if (n.size() < 5 || Lower(n.substr(n.size() - 4)) != ".trk")
            continue;
        n = n.substr(0, n.size() - 4);
        FILE *f = fopen((dir + "/" + n + ".trk").c_str(), "rb");
        if (!f)
            continue;
        u8 buf[64];
        size_t got = fread(buf, 1, sizeof buf, f);
        fclose(f);
        std::vector<u8> data(buf, buf + got);
        dsEditorTrack::Header h;
        if (!dsEditorTrack::ReadHeader(data, &h) || h.environment > 3)
            continue;
        SavedTrack t;
        t.name = n;
        t.env = h.environment;
        t.platform = h.platform != 0;
        t.times[0] = h.authorTime;
        t.times[1] = h.goldTime;
        t.times[2] = h.silverTime;
        t.times[3] = h.bronzeTime;
        m_saved.push_back(t);
    }
    closedir(d);
    std::sort(m_saved.begin(), m_saved.end(),
              [](const SavedTrack &a, const SavedTrack &b) { return a.name < b.name; });
    return m_saved;
}

std::vector<std::string> RaceApp::SavedTracks(int env) const
{
    std::vector<std::string> out;
    for (const SavedTrack &t : SavedList())
        if (env < 0 || t.env == env)
            out.push_back(t.name);
    return out;
}

bool RaceApp::EditorSave(const std::string &name, bool overwrite)
{
    (void)overwrite;
    Editor &e = *m_ed;
    std::string dir = TracksDir();
#ifdef _WIN32
    _mkdir(dir.c_str());
#else
    mkdir(dir.c_str(), 0777);
#endif
    dsEditorTrack::Header &h = e.ed->header;
    snprintf(h.name, sizeof h.name, "%s", name.c_str());
    snprintf(h.author, sizeof h.author, "%s", m_name.c_str());
    h.environment = (u8)e.env;
    h.timeOfDay = (u8)e.ed->track.info.timeOfDay;
    time_t now = time(nullptr);
    if (const struct tm *t = localtime(&now)) {
        h.date[0] = (u8)(t->tm_year % 100);
        h.date[1] = (u8)(t->tm_mon + 1);
        h.date[2] = (u8)t->tm_mday;
        h.date[3] = (u8)t->tm_hour;
        h.date[4] = (u8)t->tm_min;
        h.date[5] = (u8)t->tm_hour;
    }
    if (!e.validated)
        h.authorTime = h.goldTime = h.silverTime = h.bronzeTime = 0;
    std::vector<u8> data = e.ed->SavePacked();
    FILE *f = fopen((dir + "/" + name + ".trk").c_str(), "wb");
    if (!f || fwrite(data.data(), 1, data.size(), f) != data.size()) {
        if (f)
            fclose(f);
        Sound("MENU_ERROR");
        EditorMessage(FeS("customdatainsufficentspace"));
        return false;
    }
    fclose(f);
    m_savedRead = false;
    e.file = name;
    e.modified = false;
    if (e.validated && !m_editorUnlocked) {
        m_editorUnlocked = true; // "Create, validate and save a track ... to unlock this mode"
        WriteSave();
    }
    EditorMessage(e.validated ? FeFmt("%s : %s", FeS("helpsavetitle"), name.c_str())
                              : std::string(FeS("customdatasavevalidatemsg")));
    return true;
}

bool RaceApp::EditorLoad(const std::string &name)
{
    Editor &e = *m_ed;
    std::vector<u8> data;
    if (!ReadTrackFile(TracksDir() + "/" + name + ".trk", &data) || !e.ed->LoadPacked(data)) {
        Sound("MENU_ERROR");
        return false;
    }
    e.file = name;
    e.modified = false;
    e.validated = e.ed->header.authorTime > 0;
    e.undo.clear();
    if (dsTrackPiece *s = e.ed->StartPiece())
        for (int i = 0; i < 3; i++)
            e.cur[i] = s->cell[i];
    EditorSync();
    EditorMessage(FeFmt("%s : %s", FeS("helploadtitle"), name.c_str()));
    return true;
}

// Test drive (the car dropped at the cursor, heading as the block's rotation) or validation (from the start).
void RaceApp::EditorRace(bool validate)
{
    Editor &e = *m_ed;
    std::vector<dsTrackPiece *> pieces = e.ed->track.pieces;
    dsTrackInfo info = e.ed->track.info;
    info.lapCount = e.ed->header.lapCount;
    race.reset();
    RaceUnloadWorld();
    race.reset(new RaceSession);
    race->skin = m_skin[e.env];
    race->lighting = lighting;
    int mode = e.puzzle >= 0 ? 1 : e.ed->header.platform && validate ? 8 : 6;
    if (!race->LoadTrack(e.env, mode, info, pieces)) {
        EditorBack();
        return;
    }
    if (m_camera[e.env])
        race->ToggleCamera();
    if (!validate) {
        const dsRaceSetup *setup = dsRaceSetup::Get();
        // The arrow piece's position and quarter turns, as the race's start point of a piece without data
        // (dsRaceProgress::StartPoint: direction -z turned by the rotation).
        (void)setup;
        VecFx32 pos = dsEditorTrack::CellPosition(e.env, e.carCell[0], e.carCell[1], e.carCell[2]);
        MtxFx43 r;
        MTX_RotY43_(&r, dsSin(e.carRot * 6434), dsCos(e.carRot * 6434));
        VecFx32 dir = {0, 0, (fx32)0xfffff001};
        dsVec_MulMtx33(&dir, &r, &dir);
        e.dropPos = pos;
        e.dropDir = dir;
        race->progress->Drop(pos, dir);
        race->cams[race->camIndex].Reset();
    }
    m_run = e.puzzle >= 0 ? RUN_PUZZLE : validate ? RUN_VALIDATE : RUN_TEST;
    e.run = m_run;
    screen = PLAYING;
    m_finishFrames = -1;
    m_newRecord = false;
    track = -1;
}

void RaceApp::EditorBack()
{
    if (!m_ed)
        return;
    Editor &e = *m_ed;
    race.reset();
    RaceUnloadWorld();
    dsGameAudio::Get()->EndRace();
    race.reset(new RaceSession);
    race->lighting = lighting;
    dsRaceSetup::Get()->mode = e.puzzle >= 0 ? 7 : 6;
    if (!race->LoadView(e.env, e.ed->track.info.timeOfDay)) {
        EditorClose();
        return;
    }
    dsGameAudio::Get()->StartEditor();
    screen = EDITING;
    m_run = RUN_TRACK;
    e.overlay = Editor::O_NONE;
    EditorSync();
    EditorFrame();
}

void RaceApp::EditorClose()
{
    int env = m_ed ? m_ed->env : 0, puzzle = m_ed ? m_ed->puzzle : -1;
    m_ed.reset();
    m_run = RUN_TRACK;
    track = -1;
    ToTracks();
    if (puzzle >= 0) {
        m_stack.assign(1, {P_MAIN, 0});
        m_stack.push_back({P_SOLO, 1});
        m_stack.push_back({P_PUZZLES, puzzle});
    } else {
        m_stack.assign(1, {P_MAIN, 1});
        m_stack.push_back({P_EDITOR_ENV, env});
    }
}

// A validation run's finish: its time becomes the author time (the medals: + 2 s, + 3 s, + 5 s, main 0x0209fa5c).
void RaceApp::EditorFinished()
{
    Editor &e = *m_ed;
    fx32 t = race->progress->finishTime;
    e.ed->track.info.SetAuthorTime(t);
    dsEditorTrack::Header &h = e.ed->header;
    h.authorTime = e.ed->track.info.authorTime;
    h.goldTime = e.ed->track.info.goldTime;
    h.silverTime = e.ed->track.info.silverTime;
    h.bronzeTime = e.ed->track.info.bronzeTime;
    e.validated = true;
    e.modified = true;
}

int RaceApp::PuzzleMedal(int n) const
{
    const std::vector<PuzzleEntry> &p = Puzzles();
    if (n < 0 || n >= (int)p.size() || n >= (int)m_puzzleBest.size() || m_puzzleBest[(size_t)n] <= 0)
        return 0;
    for (int m = 0; m < 3; m++)
        if (m_puzzleBest[(size_t)n] <= p[(size_t)n].times[m])
            return 3 - m;
    return 0;
}

// My Tracks: a saved track raced with its validation's medal times.
bool RaceApp::StartCustom(const std::string &name)
{
    std::vector<u8> data;
    dsEditorTrack::Header h;
    if (!ReadTrackFile(TracksDir() + "/" + name + ".trk", &data) || !dsEditorTrack::ReadHeader(data, &h) ||
        h.environment > 3)
        return false;
    Unload();
    dsGameAudio *ga = dsGameAudio::Get();
    if (ga->state != dsGameAudio::RACE)
        ga->PlayMusic(dsGameAudio::MUSIC_NONE, 0);
    dsRaceSetup::Get()->mode = 1;
    dsEditorTrack ed(h.environment); // loads the piece catalogue the copies use
    if (!ed.LoadPacked(data))
        return false;
    ed.track.info.SetAuthorTime(h.authorTime);
    ed.track.info.lapCount = h.lapCount;
    race.reset(new RaceSession);
    race->skin = m_skin[h.environment];
    race->lighting = lighting;
    if (!race->LoadTrack(h.environment, h.platform ? 8 : 1, ed.track.info, ed.track.pieces)) {
        ToTracks();
        return false;
    }
    if (m_camera[h.environment])
        race->ToggleCamera();
    m_custom = name;
    m_customData = data;
    m_run = RUN_CUSTOM;
    for (const auto &b : m_customBest)
        if (b.first == name)
            race->recordTime = b.second;
    screen = PLAYING;
    m_finishFrames = -1;
    m_newRecord = false;
    track = -1;
    return true;
}

// ---------------------------------------------------------------------------------------------------------------
// Drawing (480 x 272 canvas over the 3D view).

void RaceApp::DrawEditor(const FeCanvas &fe) const
{
    const Editor &e = *m_ed;
    // Top left: the editor and the track; top right: the budget gauge (the DS's MemoryUsage sprite), or the
    // puzzle's blocks left.
    std::string title = e.puzzle >= 0 ? PuzzleName(e.puzzle) : std::string(FeS("editormsg"));
    std::string sub = e.file.empty() ? std::string(FeS(kEnvKeys[e.env])) : e.file;
    float tw = std::max(fe.TextWidth(FeCanvas::FONT28, title.c_str()), fe.TextWidth(FeCanvas::FONT20, sub.c_str()));
    fe.Panel(-10, 8, tw + 34, 48, 12, Fade(kWhite, 0.92f), kBlue, 2);
    fe.Text(FeCanvas::FONT28, title.c_str(), 10, 15, kInk);
    fe.Text(FeCanvas::FONT20, sub.c_str(), 10, 38, kSoft);
    if (e.puzzle < 0) {
        float u = e.ed->Usage();
        fe.Panel(FeCanvas::W - 128, 10, 118, 26, 8, Fade(kWhite, 0.92f), kEdge, 1);
        fe.Text(FeCanvas::FONT20, FeL("M\xe9""moire", "Memory"), FeCanvas::W - 120, 18, kSoft);
        fe.Rect(FeCanvas::W - 62, 17, 44, 12, FeRgba(210, 210, 215));
        fe.Rect(FeCanvas::W - 62, 17, 44 * u, 12, u > 0.9f ? kRed : kBlue);
    }
    std::string pdiff;
    int pneed, pcoppers;
    fx32 ptimes[3];
    if (e.puzzle >= 0 && PuzzleInfo(e.puzzle, &pdiff, &pneed, ptimes, &pcoppers)) {
        // The puzzle's medal times (config/PuzzleProgression).
        static const char *const kMedalKeys[3] = {"ghostgold", "ghostsilver", "ghostbronze"};
        fe.Panel(FeCanvas::W - 150, 10, 140, 70, 8, Fade(kWhite, 0.92f), kEdge, 1);
        for (int m = 0; m < 3; m++) {
            fe.Cell(FeCanvas::BANK_COPPERS, 2 - m, FeCanvas::W - 146, 13 + m * 21, 0.7f);
            fe.Text(FeCanvas::FONT20, FeS(kMedalKeys[m]), FeCanvas::W - 124, 19 + m * 21, kSoft);
            fe.Text(FeCanvas::FONT20, FeTime(ptimes[m]).c_str(), FeCanvas::W - 18, 19 + m * 21, kInk, 2);
        }
    } else if (e.validated) {
        std::string t = FeFmt("%s %s", FeS("ghostgold"), FeTime(e.ed->header.goldTime).c_str());
        float w = fe.TextWidth(FeCanvas::FONT20, t.c_str()) + 36;
        fe.Panel(FeCanvas::W - w - 10, e.puzzle < 0 ? 40 : 10, w, 24, 8, Fade(kWhite, 0.92f), kEdge, 1);
        fe.Cell(FeCanvas::BANK_COPPERS, 2, FeCanvas::W - w - 6, (e.puzzle < 0 ? 40 : 10) + 1, 0.7f);
        fe.Text(FeCanvas::FONT20, t.c_str(), FeCanvas::W - 18, (e.puzzle < 0 ? 47 : 17), kInk, 2);
    }

    // Bottom: the block menu, or the carried block and the controls.
    G2CellBank *blocks = IconBank(e.env, false), *folders = IconBank(e.env, true);
    if (e.menuOpen) {
        const std::vector<dsBlockMenuEntry> *f = e.menu.Folder(e.folders.back());
        int n = f ? (int)f->size() : 0, c = e.folderCursor.back();
        const float y = FeCanvas::H - 74, size = 40, step = 46;
        fe.Panel(6, y - 22, FeCanvas::W - 12, 76, 10, Fade(kWhite, 0.94f), kBlue, 2);
        // The open folders, the first ones left out while the line would reach the block's name.
        float nameW = n && c < n ? fe.TextWidth(FeCanvas::FONT20, (*f)[(size_t)c].name.c_str()) : 0;
        std::string path;
        for (size_t first = 1; first <= e.folders.size(); first++) {
            path = first > 1 ? "... " : FeS("helpblocktitle");
            for (size_t i = first; i < e.folders.size(); i++)
                path += " > " + e.folders[i];
            if (fe.TextWidth(FeCanvas::FONT20, path.c_str()) + nameW < FeCanvas::W - 50)
                break;
        }
        fe.Text(FeCanvas::FONT20, path.c_str(), 16, y - 16, kSoft);
        int perPage = 9, first = std::max(0, std::min(c - perPage / 2, n - perPage));
        for (int i = first; i < n && i < first + perPage; i++) {
            const dsBlockMenuEntry &b = (*f)[(size_t)i];
            float x = 18 + (i - first) * step;
            bool sel = i == c;
            fe.Panel(x - 3, y - 1, size + 6, size + 6, 6, sel ? FeRgba(220, 235, 255) : kWhite, sel ? kBlue : kEdge,
                     sel ? 3 : 1);
            bool locked = !b.folder && e.puzzle < 0 && BlockLocked(e.env, b.name);
            Icon(fe, b.folder ? folders : blocks, b.icon, x, y + 2, size,
                 locked ? FeRgba(110, 110, 110, 200) : 0xffffffff);
            if (locked)
                fe.Cell(FeCanvas::BANK_COPPERS, 0, x + size - 18, y + size - 16, 0.55f);
            if (e.puzzle >= 0 && !b.folder) {
                std::string cnt = FeFmt("x%d", StockOf(e, b.id));
                fe.Text(FeCanvas::FONT20, cnt.c_str(), x + size + 1, y + size - 10, StockOf(e, b.id) ? kInk : kRed, 2);
            }
        }
        if (n && c < n) {
            const dsBlockMenuEntry &b = (*f)[(size_t)c];
            fe.Text(FeCanvas::FONT20, b.name.c_str(), FeCanvas::W - 18, y - 16, kInk, 2);
        }
    } else {
        const float y = FeCanvas::H - 50;
        fe.Panel(6, y, 150, 44, 10, Fade(kWhite, 0.92f), kEdge, 1);
        if (e.mode == Editor::MODE_DRAW) {
            fe.Text(FeCanvas::FONT20, FeS("helpfreedrawtitle"), 14, y + 8, kInk);
            fe.Text(FeCanvas::FONT20, e.stroke ? FeL("Trac\xe9"" en cours", "Drawing") : FeL("X : tracer", "X: draw"), 14,
                    y + 26, e.stroke ? kBlue : kSoft);
        } else if (e.mode == Editor::MODE_CAR) {
            fe.Text(FeCanvas::FONT20, FeS("helpplacingvehicletitle"), 14, y + 8, kInk, 0, 136);
            fe.Text(FeCanvas::FONT20, FeS("helptesttitle"), 14, y + 26, kSoft);
        } else if (e.block) {
            Icon(fe, blocks, e.blockIcon, 12, y + 4, 36);
            fe.Text(FeCanvas::FONT20, e.blockName.c_str(), 54, y + 8, kInk, 0, 96);
            if (e.puzzle >= 0)
                fe.Text(FeCanvas::FONT20, FeFmt("x%d", StockOf(e, e.block)).c_str(), 150, y + 28, kSoft, 2);
        } else {
            fe.Text(FeCanvas::FONT20, FeS("helpselecttitle"), 14, y + 8, kInk);
            fe.Text(FeCanvas::FONT20, FeL("Triangle : blocs", "Triangle: blocks"), 14, y + 26, kSoft);
        }
        // Controls, on two lines.
        bool fr = dsStringTable::Get()->Language() == dsStringTable::LANG_FR;
        const char *lines[2] = {fr ? "X poser    Carr\xe9"" tourner    Triangle blocs" : "X place    Square rotate    Triangle blocks",
                                fr ? "O effacer    L / R hauteur    START menu" : "O delete    L / R height    START menu"};
        if (e.mode == Editor::MODE_DRAW) {
            lines[0] = fr ? "X maintenu + croix : tracer la route" : "Hold X + d-pad: draw the road";
            lines[1] = fr ? "O fin    L / R hauteur    START menu" : "O done    L / R height    START menu";
        } else if (e.mode == Editor::MODE_CAR) {
            lines[0] = fr ? "X essayer    Carr\xe9"" tourner    L / R hauteur" : "X test    Square turn    L / R height";
            lines[1] = fr ? "Croix : d\xe9""placer    O retour    START menu" : "D-pad: move    O back    START menu";
        }
        fe.Panel(164, y, FeCanvas::W - 170, 44, 10, Fade(kWhite, 0.85f), kEdge, 1);
        for (int i = 0; i < 2; i++)
            fe.Text(FeCanvas::FONT20, lines[i], 174, y + 8 + i * 18, kInk);
    }
    if (e.messageFrames > 0 && !e.message.empty()) {
        float a = std::min(1.0f, e.messageFrames / 15.0f);
        float h = fe.TextHeight(FeCanvas::FONT20, e.message.c_str(), 300) + 20;
        fe.Panel(80, 96, 320, h, 10, Fade(kWhite, 0.95f * a), Fade(kBlue, a), 2);
        fe.Text(FeCanvas::FONT20, e.message.c_str(), 96, 106, Fade(kInk, a), 0, 288);
    }

    // Overlays.
    if (e.overlay != Editor::O_NONE)
        fe.Rect(0, 0, FeCanvas::W, FeCanvas::H, FeRgba(0, 0, 0, 100));
    auto rowList = [&](const std::vector<std::string> &rows, int cursor, float x, float y, float w) {
        const float rh = 22, step = 25;
        int first = std::max(0, std::min(cursor - 3, (int)rows.size() - 8));
        for (int i = first; i < (int)rows.size() && i < first + 8; i++) {
            bool sel = i == cursor;
            float ry = y + (i - first) * step;
            fe.Panel(sel ? x - 3 : x, sel ? ry - 2 : ry, sel ? w + 6 : w, sel ? rh + 4 : rh, 6, kWhite,
                     sel ? kBlue : kEdge, sel ? 3 : 1);
            fe.Text(FeCanvas::FONT20, rows[(size_t)i].c_str(), x + 10, ry + (rh - fe.CapHeight(FeCanvas::FONT20)) / 2,
                    kInk);
        }
    };
    switch (e.overlay) {
    case Editor::O_MENU: {
        std::vector<int> items = EditorMenuItems(e);
        std::vector<std::string> rows;
        for (int it : items)
            rows.push_back(MenuLabel(it));
        rowList(rows, e.cursor, 150, 40, 180);
        break;
    }
    case Editor::O_OBJECTIVES: {
        const dsEditorTrack::Header &h = e.ed->header;
        dsTrackPiece *s = e.ed->StartPiece();
        bool multilap = s && s->type == PIECE_MULTILAP;
        fe.Panel(90, 60, 300, multilap ? 130 : 104, 10, kWhite, kBlue, 2);
        fe.Text(FeCanvas::FONT28, FeS("helpobjectivestitle"), 240, 70, kInk, 1);
        const char *labels[3] = {FeS("challengetype"), FeL("Difficult\xe9", "Difficulty"), FeS("numberoflaps")};
        std::string values[3] = {FeS(h.platform ? "platformbutton" : "racebutton"),
                                 FeS(kDiffKeys[std::min<int>(h.difficulty, 4)]), FeFmt("%d", h.lapCount)};
        for (int i = 0; i < (multilap ? 3 : 2); i++) {
            float y = 102 + i * 28;
            bool sel = i == e.cursor;
            fe.Text(FeCanvas::FONT20, labels[i], 104, y, sel ? kBlue : kInk);
            fe.Text(FeCanvas::FONT20, ("< " + values[i] + " >").c_str(), 376, y, sel ? kBlue : kInk, 2);
        }
        break;
    }
    case Editor::O_LOAD: {
        fe.Panel(110, 22, 260, 228, 10, kWhite, kBlue, 2);
        fe.Text(FeCanvas::FONT28, FeS("customdataloadtracktitle"), 240, 30, kInk, 1);
        if (e.files.empty())
            fe.Text(FeCanvas::FONT20, FeS("customdatanotracksmsg"), 240, 120, kSoft, 1);
        else
            rowList(e.files, e.cursor, 128, 60, 224);
        fe.Text(FeCanvas::FONT20, FeFmt("%s : %s", FeL("Carr\xe9", "Square"), FeS("customdatadeletebutton")).c_str(),
                360, 232, kSoft, 2);
        break;
    }
    case Editor::O_NAME: {
        fe.Panel(60, 30, 360, 222, 10, kWhite, kBlue, 2);
        fe.Text(FeCanvas::FONT28, FeS("filenameentrytitle"), 240, 38, kInk, 1);
        fe.Panel(110, 66, 260, 28, 6, FeRgba(235, 240, 250), kEdge, 1);
        fe.Text(FeCanvas::FONT20, (e.entry + "_").c_str(), 120, 74, kInk);
        for (int k = 0; k < NAME_KEYS + 2; k++) {
            float x = k < NAME_KEYS ? 84 + (k % NAME_COLS) * 32 : 120 + (k - NAME_KEYS) * 130,
                  y = k < NAME_KEYS ? 104 + (k / NAME_COLS) * 28 : 104 + 4 * 28 + 4, w = k < NAME_KEYS ? 28 : 110;
            bool sel = k == e.cursor;
            fe.Panel(x, y, w, 24, 5, sel ? FeRgba(220, 235, 255) : kWhite, sel ? kBlue : kEdge, sel ? 2 : 1);
            std::string t = k < NAME_KEYS ? std::string(1, kNameKeys[k]) : k == NAME_KEYS ? FeL("Effacer", "Delete")
                                                                                         : "OK";
            fe.Text(FeCanvas::FONT20, t.c_str(), x + w / 2, y + 12 - fe.CapHeight(FeCanvas::FONT20) / 2, kInk, 1);
        }
        break;
    }
    case Editor::O_HELP: {
        fe.Panel(30, 16, 420, 240, 10, kWhite, kBlue, 2);
        fe.Text(FeCanvas::FONT28, FeL("Aide de l'\xe9""diteur", "Editor help"), 240, 24, kInk, 1);
        std::string text = e.puzzle >= 0 ? std::string(FeS("helppuzzle1")) + "\n" + FeS("helppuzzle2") + "\n\n" : "";
        text += FeL("Croix : d\xe9""placer le curseur (selon la cam\xe9""ra). L / R : hauteur. Stick : tourner et "
                    "zoomer la cam\xe9""ra.\nX : poser le bloc (sans bloc : prendre celui sous le curseur). Carr\xe9"" : "
                    "tourner. O : effacer le bloc sous le curseur, ou l\xe2""cher le bloc port\xe9"".\nTriangle : menu "
                    "des blocs (dossiers ; haut = retour). START : menu (dessin libre, tester, valider, objectifs, "
                    "sauvegarder, charger, annuler, tout effacer). SELECT : cette aide.\nDessin libre : X maintenu "
                    "en d\xe9""pla\xe7""ant le curseur trace la route. Tester : placez la fl\xe8""che du v\xe9""hicule "
                    "(croix, L / R, carr\xe9""), puis X.",
                    "D-pad: move the cursor (relative to the camera). L / R: height. Stick: turn and zoom the camera.\n"
                    "X: place the block (without one: take the block under the cursor). Square: rotate. O: delete the "
                    "block under the cursor, or drop the carried one.\nTriangle: block menu (folders; up = back). "
                    "START: menu (free draw, test, validate, objectives, save, load, undo, clear all). SELECT: this "
                    "help.\nFree draw: hold X while moving the cursor to draw the road. Test: place the vehicle's "
                    "arrow (d-pad, L / R, square), then X.");
        text += std::string("\n\n") + FeS("helpvalidate1");
        fe.Text(FeCanvas::FONT20, text.c_str(), 46, 54, kInk, 0, 388);
        break;
    }
    default: break;
    }
    if (e.confirm) {
        std::string q;
        switch (e.confirm) {
        case EC_CLEAR: q = std::string(FeS("helpclearalltitle")) + "\n" + FeS("queryclearall"); break;
        case EC_EXIT: q = FeS("exitconfirm"); break;
        case EC_OVERWRITE: q = std::string(FeS("customdatafilenameused")) + "\n" + FeS("customdataoverwriteconfirm"); break;
        case EC_LOAD:
            q = FeL("Le circuit a \xe9""t\xe9"" modifi\xe9"". Charger sans sauvegarder ?",
                    "The track has been modified. Load without saving?");
            break;
        default: q = FeS("customdatadeleteconfirm"); break;
        }
        fe.Rect(0, 0, FeCanvas::W, FeCanvas::H, FeRgba(0, 0, 0, 80));
        float h = fe.TextHeight(FeCanvas::FONT20, q.c_str(), 288) + 74, y = (FeCanvas::H - h) / 2;
        fe.Panel(80, y, 320, h, 10, kWhite, kBlue, 2);
        fe.Text(FeCanvas::FONT20, q.c_str(), 96, y + 16, kInk, 0, 288);
        for (int i = 0; i < 2; i++) {
            bool sel = i == e.confirmCursor;
            float bx = 240 + (i ? 12 : -102), by = y + h - 36;
            fe.Panel(bx, by, 90, 26, 7, kWhite, sel ? kBlue : kEdge, sel ? 3 : 1);
            fe.Text(FeCanvas::FONT20, FeS(i ? "nobuttontext" : "yesbuttontext"), bx + 45,
                    by + 13 - fe.CapHeight(FeCanvas::FONT20) / 2, kInk, 1);
        }
    }
    (void)kGrey;
}
