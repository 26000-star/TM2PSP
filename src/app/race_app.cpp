// Race application: tracks, race flow, the player's progress and save file (see race_app.h; the menus are in
// fe_menu.cpp).
#include "race_app.h"
#include "editor.h"
#include "fe_text.h"

#include <stdio.h>
#include <string.h>

#include "fs/dsFileSystem.h"
#include "script/dsLua.h"
#include "sound/dsCarAudio.h"
#include "sound/dsGameAudio.h"
#include "sound/dsSound3D.h"
#include "sound/dsSoundManager.h"
#include "text/dsStringTable.h"

namespace {

// The 26 race tracks of each environment (Scenes/<env>/<difficulty>/<n>.dcsc), in the game's environment order
// (dsEnvironment: stadium, island, snow, coast): track i is environment i / 26.
#define TM_ENV_TRACKS(e)                                                                                             \
    {e "/practice/0", "ENTRAINEMENT 1"}, {e "/practice/1", "ENTRAINEMENT 2"}, {e "/practice/2", "ENTRAINEMENT 3"},   \
    {e "/practice/3", "ENTRAINEMENT 4"}, {e "/practice/4", "ENTRAINEMENT 5"}, {e "/easy/0", "FACILE 1"},             \
    {e "/easy/1", "FACILE 2"}, {e "/easy/2", "FACILE 3"}, {e "/easy/3", "FACILE 4"}, {e "/easy/4", "FACILE 5"},     \
    {e "/medium/0", "MOYEN 1"}, {e "/medium/1", "MOYEN 2"}, {e "/medium/2", "MOYEN 3"}, {e "/medium/3", "MOYEN 4"}, \
    {e "/medium/4", "MOYEN 5"}, {e "/hard/0", "DIFFICILE 1"}, {e "/hard/1", "DIFFICILE 2"},                          \
    {e "/hard/2", "DIFFICILE 3"}, {e "/hard/3", "DIFFICILE 4"}, {e "/hard/4", "DIFFICILE 5"},                        \
    {e "/extreme/0", "EXTREME 1"}, {e "/bonus/0", "BONUS 1"}, {e "/bonus/1", "BONUS 2"}, {e "/bonus/2", "BONUS 3"},  \
    {e "/bonus/3", "BONUS 4"}, {e "/bonus/4", "BONUS 5"}
// Platform mode (race mode 8): Scenes/pla/<difficulty>/<n>, environment n, numbered 1..17 in this order by
// config/PlatformProgression.
const RaceTrackEntry kTracks[] = {
    TM_ENV_TRACKS("sta"), TM_ENV_TRACKS("isl"), TM_ENV_TRACKS("sno"), TM_ENV_TRACKS("coa"),
    {"pla/practice/0", "ENTRAINEMENT 1"}, {"pla/practice/1", "ENTRAINEMENT 2"}, {"pla/practice/2", "ENTRAINEMENT 3"},
    {"pla/practice/3", "ENTRAINEMENT 4"}, {"pla/easy/0", "FACILE 1"}, {"pla/easy/1", "FACILE 2"},
    {"pla/easy/2", "FACILE 3"}, {"pla/easy/3", "FACILE 4"}, {"pla/medium/0", "MOYEN 1"}, {"pla/medium/1", "MOYEN 2"},
    {"pla/medium/2", "MOYEN 3"}, {"pla/medium/3", "MOYEN 4"}, {"pla/hard/0", "DIFFICILE 1"},
    {"pla/hard/1", "DIFFICILE 2"}, {"pla/hard/2", "DIFFICILE 3"}, {"pla/hard/3", "DIFFICILE 4"},
    {"pla/extreme/0", "EXTREME 1"}};
#undef TM_ENV_TRACKS
const int kTrackCount = (int)(sizeof kTracks / sizeof kTracks[0]);
const int kPlatformFirst = 104;

// Menu sections for NextTrack: the four environments' race tracks, then the platform tracks.
int SectionFirst(int track) { return track >= kPlatformFirst ? kPlatformFirst : track / 26 * 26; }
int SectionCount(int track) { return track >= kPlatformFirst ? kTrackCount - kPlatformFirst : 26; }
bool IsPlatform(int track) { return track >= kPlatformFirst; }

// Difficulties in the game's order (dsRaceSetup, strings difficulty*): first race track of each in an environment.
const char *const kDiffNames[6] = {"practice", "easy", "medium", "hard", "extreme", "bonus"};
const int kDiffFirst[7] = {0, 5, 10, 15, 20, 21, 26};
int DiffIndex(const char *name)
{
    for (int d = 0; d < 6; d++)
        if (name && !strcmp(name, kDiffNames[d]))
            return d;
    return -1;
}

// The progression and shop tables (Scripts/config, read by the DS front end's screens), loaded once.
struct Config {
    int raceMedals[104] = {}, raceCoppers[104] = {};     // RaceProgression {difficulty, env, n, medals, coppers}
    int platMedals[17] = {}, platCoppers[17] = {};       // PlatformProgression {difficulty, n, medals, gold,
    int platGold[17] = {}, platSilver[17], platBronze[17]; //   silver, bronze (retries at most), coppers}
    struct Shop {
        int price, env, index; // index: the track (TrackShop) or skin 0..9 of the environment (SkinShop)
        bool owned;            // SkinShop: owned from the start
        std::string name;
    };
    std::vector<Shop> tracks, skins, blocks;
};

const Config &Cfg()
{
    static Config c;
    static bool loaded = false;
    if (loaded)
        return c;
    loaded = true;
    for (int i = 0; i < 17; i++) {
        c.platSilver[i] = 7;
        c.platBronze[i] = 10;
    }
    static dsScriptManager scripts;
    dsLuaStateRef st = scripts.Run("config/RaceProgression");
    if (st.get()) {
        dsLuaObject all = st->Global("RaceProgression");
        int seen[6][4] = {};
        for (dsLuaObject k = all.Begin(); !k.IsNil(); k = all.Next(k)) {
            dsLuaObject e = all.Value(k);
            int d = DiffIndex(e.IndexString(0)), env = e.IndexNumber(1) >> 12;
            if (d < 0 || env < 0 || env > 3)
                continue;
            int t = env * 26 + kDiffFirst[d] + seen[d][env]++;
            if (t >= env * 26 + kDiffFirst[d + 1])
                continue;
            c.raceMedals[t] = e.IndexNumber(3) >> 12;
            c.raceCoppers[t] = e.IndexNumber(4) >> 12;
        }
    }
    st = scripts.Run("config/PlatformProgression");
    if (st.get()) {
        dsLuaObject all = st->Global("PlatformProgression");
        for (dsLuaObject k = all.Begin(); !k.IsNil(); k = all.Next(k)) {
            dsLuaObject e = all.Value(k);
            int n = (e.IndexNumber(1) >> 12) - 1;
            if (n < 0 || n >= 17)
                continue;
            c.platMedals[n] = e.IndexNumber(2) >> 12;
            c.platGold[n] = e.IndexNumber(3) >> 12;
            c.platSilver[n] = e.IndexNumber(4) >> 12;
            c.platBronze[n] = e.IndexNumber(5) >> 12;
            c.platCoppers[n] = e.IndexNumber(6) >> 12;
        }
    }
    // TrackShop {n, price, env, 0, rank, "StadiumA5"}: the fifth track of difficulty `rank` (practice..hard) of the
    // environment is sold (the A..D of the names follow the difficulties; port reading, to confirm in ov004).
    st = scripts.Run("config/TrackShop");
    if (st.get()) {
        dsLuaObject all = st->Global("TrackShop");
        for (dsLuaObject k = all.Begin(); !k.IsNil(); k = all.Next(k)) {
            dsLuaObject e = all.Value(k);
            int env = e.IndexNumber(2) >> 12, rank = e.IndexNumber(4) >> 12;
            if (env < 0 || env > 3 || rank < 0 || rank > 3)
                continue;
            const char *name = e.IndexString(5);
            c.tracks.push_back({e.IndexNumber(1) >> 12, env, env * 26 + kDiffFirst[rank] + 4, false, name ? name : ""});
        }
    }
    // SkinShop {n, price, env, owned, "StadiumSkin1"}: ten skins per environment, Cars/<env>/0..9.
    st = scripts.Run("config/SkinShop");
    if (st.get()) {
        dsLuaObject all = st->Global("SkinShop");
        int seen[4] = {};
        for (dsLuaObject k = all.Begin(); !k.IsNil(); k = all.Next(k)) {
            dsLuaObject e = all.Value(k);
            int env = e.IndexNumber(2) >> 12;
            if (env < 0 || env > 3 || seen[env] >= 10)
                continue;
            const char *name = e.IndexString(4);
            c.skins.push_back({e.IndexNumber(1) >> 12, env, seen[env]++, (e.IndexNumber(3) >> 12) != 0,
                               name ? name : ""});
        }
    }
    // BlockShop {Name "STA_Multilap", Env, Price, ViewScale} (main 0x020b1b4c): the editor's blocks for sale.
    st = scripts.Run("config/BlockShop");
    if (st.get()) {
        dsLuaObject all = st->Global("BlockShop");
        int seen[4] = {};
        for (int i = 0, n = all.Count(); i < n; i++) {
            dsLuaObject e = all.Index(i);
            int env = e.FieldNumber("Env") >> 12;
            const char *name = e.FieldString("Name");
            if (env < 0 || env > 3 || !name)
                continue;
            c.blocks.push_back({e.FieldNumber("Price") >> 12, env, seen[env]++, false, name});
        }
    }
    printf("fe config: %d shop tracks, %d skins, %d blocks\n", (int)c.tracks.size(), (int)c.skins.size(),
           (int)c.blocks.size());
    return c;
}

const int RESULT_DELAY = 90; // frames (3 s at 30 Hz) between the finish and the result menu

} // namespace

const RaceTrackEntry *RaceTracks(int *count)
{
    if (count)
        *count = kTrackCount;
    return kTracks;
}

RaceApp::RaceApp(const std::string &savePath, const char *, const char *)
    : m_savePath(savePath), m_best(new fx32[kTrackCount]()), m_splits(new std::vector<fx32>[kTrackCount]),
      m_bestRetries(new int[kTrackCount]), m_medalTimes(new fx32[104 * 3]), m_count(kTrackCount)
{
    for (int i = 0; i < kTrackCount; i++)
        m_bestRetries[i] = -1;
    for (int i = 0; i < 104 * 3; i++)
        m_medalTimes[i] = -1;
    const Config &c = Cfg();
    m_ownedTracks.assign(c.tracks.size(), 0);
    m_ownedSkins.assign(c.skins.size(), 0);
    m_ownedBlocks.assign(c.blocks.size(), 0);
    for (size_t i = 0; i < c.skins.size(); i++)
        m_ownedSkins[i] = c.skins[i].owned;
    m_language = dsStringTable::Get()->Language();
    m_puzzleBest.assign(17, 0);
    LoadSave();
    m_stack.push_back({P_MAIN, 0});
}

RaceApp::~RaceApp() { Unload(); }

// A track id without its environment ("easy/0": the save files and options of the Coast-only versions) is a
// Coast track.
int RaceApp::FindTrack(const char *id) const
{
    for (int i = 0; i < m_count; i++)
        if (!strcmp(kTracks[i].id, id) || (!strncmp(kTracks[i].id, "coa/", 4) && !strcmp(kTracks[i].id + 4, id)))
            return i;
    return -1;
}

int RaceApp::NextTrack(int i) const
{
    int first = SectionFirst(i);
    return first + (i - first + 1) % SectionCount(i);
}

void RaceApp::SetLanguage(int language)
{
    if (m_languageSaved)
        language = m_language;
    m_language = language;
    dsStringTable *st = dsStringTable::Get();
    st->Load(language, dsStringTable::FILE_BACKEND);
    st->LoadAlso(dsStringTable::FILE_FRONTEND);
}

// Save file: text lines "last <track>", "best <track> <fx32 time>", "split <track> <fx32 time>...",
// "retries <track> <n>", then the player's data: "coppers <n>", "owntrack <n>", "ownskin <n>", "skin <env> <n>",
// "camera <env> <n>", "ghosts <opponent> <player>", (the player's best runs: ghost_<track>.bin beside it)
// "lang <n>", "music <0|1>", "sfx <0|1>", "races <n>", "finishes <n>", the editor: "editor 1" (a validated track
// saved), "puzzle <n> <fx32 time>", "custom <fx32 time> <track name>" (a few KB).
void RaceApp::LoadSave()
{
    if (m_savePath.empty())
        return;
    FILE *f = fopen(m_savePath.c_str(), "rb");
    if (!f)
        return;
    char line[1024], id[64];
    long t;
    int n, v;
    while (fgets(line, sizeof line, f)) {
        if (sscanf(line, "split %63s%n", id, &n) == 1) {
            int i = FindTrack(id);
            if (i < 0)
                continue;
            m_splits[i].clear();
            int k;
            for (const char *p = line + n; sscanf(p, "%ld%n", &t, &k) == 1; p += k)
                m_splits[i].push_back((fx32)t);
        } else if (sscanf(line, "retries %63s %d", id, &n) == 2) {
            int i = FindTrack(id);
            if (i >= 0 && n >= 0)
                m_bestRetries[i] = n;
        } else if (sscanf(line, "best %63s %ld", id, &t) == 2) {
            int i = FindTrack(id);
            if (i >= 0 && t > 0)
                m_best[i] = (fx32)t;
        } else if (sscanf(line, "last %63s", id) == 1) {
            int i = FindTrack(id);
            if (i >= 0)
                selected = i;
        } else if (sscanf(line, "coppers %d", &n) == 1) {
            m_coppers = n > 0 ? n : 0;
        } else if (sscanf(line, "owntrack %d", &n) == 1) {
            if (n >= 0 && n < (int)m_ownedTracks.size())
                m_ownedTracks[n] = 1;
        } else if (sscanf(line, "ownblock %d", &n) == 1) {
            if (n >= 0 && n < (int)m_ownedBlocks.size())
                m_ownedBlocks[n] = 1;
        } else if (sscanf(line, "ownskin %d", &n) == 1) {
            if (n >= 0 && n < (int)m_ownedSkins.size())
                m_ownedSkins[n] = 1;
        } else if (sscanf(line, "skin %d %d", &n, &v) == 2) {
            if (n >= 0 && n < 4 && v >= 0 && v < 10)
                m_skin[n] = v;
        } else if (sscanf(line, "camera %d %d", &n, &v) == 2) {
            if (n >= 0 && n < 4)
                m_camera[n] = v & 1;
        } else if (sscanf(line, "lang %d", &n) == 1) {
            if (n >= 0 && n < dsStringTable::LANG_COUNT) {
                m_language = n;
                m_languageSaved = true;
            }
        } else if (sscanf(line, "music %d", &n) == 1) {
            m_musicOn = n != 0;
        } else if (sscanf(line, "sfx %d", &n) == 1) {
            m_sfxOn = n != 0;
        } else if (sscanf(line, "ghosts %d %d", &n, &v) == 2) {
            if (n == 0)
                m_ghostChoice = GHOSTS_NONE; // the earlier yes / no option
            m_ghostPlayer = v != 0;
        } else if (sscanf(line, "ghostchoice %d", &n) == 1) {
            if (n >= 0 && n < GHOSTS_COUNT)
                m_ghostChoice = n;
        } else if (sscanf(line, "races %d", &n) == 1) {
            m_races = n;
        } else if (sscanf(line, "finishes %d", &n) == 1) {
            m_finishes = n;
        } else if (sscanf(line, "editor %d", &n) == 1) {
            m_editorUnlocked = n != 0;
        } else if (sscanf(line, "puzzle %d %ld", &n, &t) == 2) {
            if (n >= 0 && n < (int)m_puzzleBest.size() && t > 0)
                m_puzzleBest[(size_t)n] = (fx32)t;
        } else if (sscanf(line, "custom %ld %n", &t, &n) == 1) {
            std::string name = line + n;
            while (!name.empty() && (name.back() == '\n' || name.back() == '\r'))
                name.pop_back();
            if (!name.empty() && t > 0)
                m_customBest.push_back({name, (fx32)t});
        }
    }
    fclose(f);
}

void RaceApp::WriteSave() const
{
    if (m_savePath.empty())
        return;
    FILE *f = fopen(m_savePath.c_str(), "wb");
    if (!f)
        return;
    if (track >= 0)
        fprintf(f, "last %s\n", kTracks[track].id);
    for (int i = 0; i < m_count; i++)
        if (m_best[i] > 0) {
            fprintf(f, "best %s %ld\n", kTracks[i].id, (long)m_best[i]);
            if (m_bestRetries[i] >= 0)
                fprintf(f, "retries %s %d\n", kTracks[i].id, m_bestRetries[i]);
            if (!m_splits[i].empty()) {
                fprintf(f, "split %s", kTracks[i].id);
                for (fx32 t : m_splits[i])
                    fprintf(f, " %ld", (long)t);
                fprintf(f, "\n");
            }
        }
    fprintf(f, "coppers %d\n", m_coppers);
    for (size_t i = 0; i < m_ownedTracks.size(); i++)
        if (m_ownedTracks[i])
            fprintf(f, "owntrack %d\n", (int)i);
    for (size_t i = 0; i < m_ownedBlocks.size(); i++)
        if (m_ownedBlocks[i])
            fprintf(f, "ownblock %d\n", (int)i);
    const Config &c = Cfg();
    for (size_t i = 0; i < m_ownedSkins.size(); i++)
        if (m_ownedSkins[i] && !c.skins[i].owned)
            fprintf(f, "ownskin %d\n", (int)i);
    for (int e = 0; e < 4; e++)
        fprintf(f, "skin %d %d\ncamera %d %d\n", e, m_skin[e], e, m_camera[e]);
    if (m_languageSaved)
        fprintf(f, "lang %d\n", m_language);
    fprintf(f, "music %d\nsfx %d\nghosts %d %d\nghostchoice %d\nraces %d\nfinishes %d\n", m_musicOn, m_sfxOn,
            m_ghostChoice != GHOSTS_NONE, m_ghostPlayer, m_ghostChoice, m_races, m_finishes);
    if (m_editorUnlocked)
        fprintf(f, "editor 1\n");
    for (size_t i = 0; i < m_puzzleBest.size(); i++)
        if (m_puzzleBest[i] > 0)
            fprintf(f, "puzzle %d %ld\n", (int)i, (long)m_puzzleBest[i]);
    for (const auto &b : m_customBest)
        fprintf(f, "custom %ld %s\n", (long)b.second, b.first.c_str());
    fclose(f);
}

// Ghosts/<Env>/<difficulty>/<n>/<medal>.bin of a race track (medal 1 bronze .. 3 gold).
static void GhostName(int index, int medal, char *path, size_t n)
{
    static const char *const kEnv[4] = {"Sta", "Isl", "Sno", "Coa"};
    static const char *const kMedal[4] = {"", "bronze", "silver", "gold"};
    int env = index / 26, k = index % 26, d = 0;
    while (k >= kDiffFirst[d + 1])
        d++;
    snprintf(path, n, "Ghosts/%s/%s/%d/%s.bin", kEnv[env], kDiffNames[d], k - kDiffFirst[d], kMedal[medal]);
}

// The medal times are the finish times of the provided ghosts, Ghosts/<Env>/<difficulty>/<n>/<medal>.bin (path
// format at ov004 0x020fa3ec), header +8 (fx32) when the version at +4 is 6: the DS gives a medal for beating that
// ghost. The challenge screen (ov004 0x020fa49c) reads gold, silver and bronze this way for each challenge.
// @port ov004:020fa3ec done
fx32 RaceApp::MedalTime(int index, int medal) const
{
    if (index < 0 || index >= kPlatformFirst || medal < 1 || medal > 3)
        return 0;
    fx32 &t = m_medalTimes[index * 3 + medal - 1];
    if (t < 0) {
        char path[96];
        GhostName(index, medal, path, sizeof path);
        t = 0;
        dsFile *f = dsFileSystem::Get()->Open(path);
        u8 hd[12];
        if (f && f->IsOpen() && f->Read(hd, 12) == 12 && hd[4] == 6)
            t = (fx32)(hd[8] | hd[9] << 8 | hd[10] << 16 | (u32)hd[11] << 24);
        delete f;
    }
    return t;
}

int RaceApp::MedalFor(int index, fx32 time, int retries) const
{
    if (index < 0 || time <= 0)
        return 0;
    const Config &c = Cfg();
    if (IsPlatform(index)) {
        int n = index - kPlatformFirst;
        return retries < 0 ? 0 : retries <= c.platGold[n] ? 3 : retries <= c.platSilver[n] ? 2
                                                     : retries <= c.platBronze[n] ? 1 : 0;
    }
    for (int m = 3; m >= 1; m--) {
        fx32 t = MedalTime(index, m);
        if (t > 0 && time <= t)
            return m;
    }
    return 0;
}

int RaceApp::Medal(int index) const
{
    if (index < 0 || index >= m_count || m_best[index] <= 0)
        return 0;
    return MedalFor(index, m_best[index], m_bestRetries[index]);
}

// Medals to unlock a difficulty: each medal of a track counts (gold = 3), race and platform apart (port reading of
// RaceProgression: practice 0, easy 40, medium 90, hard 140, extreme 190, bonus 200 out of 312).
int RaceApp::MedalsTotal(bool platform) const
{
    int n = 0;
    for (int i = platform ? kPlatformFirst : 0; i < (platform ? kTrackCount : kPlatformFirst); i++)
        n += Medal(i);
    return n;
}

int RaceApp::RequiredMedals(int index) const
{
    const Config &c = Cfg();
    return IsPlatform(index) ? c.platMedals[index - kPlatformFirst] : c.raceMedals[index];
}

int RaceApp::Coppers(int index) const
{
    const Config &c = Cfg();
    return IsPlatform(index) ? c.platCoppers[index - kPlatformFirst] : c.raceCoppers[index];
}

int RaceApp::ShopTrack(int index) const
{
    const Config &c = Cfg();
    for (size_t i = 0; i < c.tracks.size(); i++)
        if (c.tracks[i].index == index)
            return (int)i;
    return -1;
}

bool RaceApp::Owned(int kind, int n) const
{
    const std::vector<char> &v = kind == 3 ? m_ownedBlocks : kind ? m_ownedSkins : m_ownedTracks;
    return n >= 0 && n < (int)v.size() && v[n];
}

// @port main:020b21e4 done (the cheat flag, settings +0x04 bit 2, is not ported)
bool RaceApp::BlockLocked(int env, const std::string &name) const
{
    const std::vector<Config::Shop> &b = Cfg().blocks;
    for (size_t i = 0; i < b.size(); i++) {
        if (b[i].env != env || b[i].name.size() < 5)
            continue;
        const char *x = b[i].name.c_str() + 4, *y = name.c_str(); // "STA_" left out
        while (*x && *y && tolower((unsigned char)*x) == tolower((unsigned char)*y)) {
            x++;
            y++;
        }
        if (!*x && !*y)
            return !m_ownedBlocks[i];
    }
    return false;
}

bool RaceApp::ShopEntry(int kind, int n, int *env, int *price, int *index, std::string *name) const
{
    const std::vector<Config::Shop> &v = kind == 3 ? Cfg().blocks : kind ? Cfg().skins : Cfg().tracks;
    if (n < 0 || n >= (int)v.size())
        return false;
    *env = v[n].env;
    *price = v[n].price;
    *index = v[n].index;
    *name = v[n].name;
    return true;
}

int RaceApp::MedalRetries(int n, int medal) const
{
    const Config &c = Cfg();
    if (n < 0 || n >= 17)
        return 0;
    return medal == 3 ? c.platGold[n] : medal == 2 ? c.platSilver[n] : c.platBronze[n];
}

bool RaceApp::Unlocked(int index) const
{
    if (index < 0 || index >= m_count)
        return false;
    if (MedalsTotal(IsPlatform(index)) < RequiredMedals(index))
        return false;
    int shop = ShopTrack(index);
    return shop < 0 || Owned(0, shop);
}

bool RaceApp::PlatformUnlocked() const
{
    for (int env = 0; env < 4; env++) {
        bool any = false;
        for (int i = env * 26; i < env * 26 + 26 && !any; i++)
            any = Medal(i) > 0;
        if (!any)
            return false;
    }
    return true;
}

std::string RaceApp::PlayerGhostPath(int index) const
{
    size_t slash = m_savePath.find_last_of("/\\");
    if (m_savePath.empty() || index < 0 || index >= m_count)
        return "";
    std::string name = std::string("ghost_") + kTracks[index].id + ".bin";
    for (char &c : name)
        if (c == '/')
            c = '_';
    return (slash == std::string::npos ? std::string() : m_savePath.substr(0, slash + 1)) + name;
}

// The race's ghosts: the opponent of the next medal to win (the gold one once won; race challenges only, as the
// DS's "Beat Ghost opponents to win Medals") and the player's best run, each when its option is on.
void RaceApp::LoadGhosts(int index)
{
    race->ghosts.clear();
    std::vector<u8> d;
    if (m_ghostChoice != GHOSTS_NONE && !IsPlatform(index) && m_mode != MODE_QUICK) {
        for (int medal = 3; medal >= 1; medal--) {
            if (m_ghostChoice != GHOSTS_ALL && m_ghostChoice != 3 - medal) // GHOSTS_GOLD = 0 is medal 3
                continue;
            char path[96];
            GhostName(index, medal, path, sizeof path);
            std::unique_ptr<dsGhost> g(new dsGhost);
            if (RaceReadFile(path, &d) && g->Load(d))
                race->ghosts.push_back(std::move(g));
        }
    }
    std::string mine = PlayerGhostPath(index);
    if (m_ghostPlayer && !mine.empty()) {
        if (FILE *f = fopen(mine.c_str(), "rb")) {
            fseek(f, 0, SEEK_END);
            long n = ftell(f);
            fseek(f, 0, SEEK_SET);
            d.assign(n > 0 && n < (1 << 20) ? (size_t)n : 0, 0);
            bool ok = !d.empty() && fread(d.data(), 1, d.size(), f) == d.size();
            fclose(f);
            std::unique_ptr<dsGhost> g(new dsGhost);
            if (ok && g->Load(d))
                race->ghosts.push_back(std::move(g));
        }
    }
    printf("ghosts: %d (opponents %s)\n", (int)race->ghosts.size(), GhostChoiceKey(m_ghostChoice));
}

const char *RaceApp::GhostChoiceKey(int choice)
{
    static const char *const kKeys[GHOSTS_COUNT] = {"ghostgold", "ghostsilver", "ghostbronze", "ghostall", "ghostnone"};
    return kKeys[choice >= 0 && choice < GHOSTS_COUNT ? choice : 0];
}

bool RaceApp::InitAudio()
{
    if (!dsSoundManager::Get()->LoadArchive("audio/sound_data.sdat"))
        return false;
    dsGameAudio *ga = dsGameAudio::Get();
    ga->musicOn = ga->musicOn && m_musicOn;
    ga->StartFrontEnd();
    ga->PlayMusic(dsGameAudio::MUSIC_FRONT_END, 0);
    return true;
}

// The front end's menu sounds (the UI plays them through *0x020d35cc +0x40..+0x48, given by main 0x0208c8ec).
void RaceApp::Sound(const char *name) const
{
    if (m_sfxOn)
        dsGameAudio::Get()->PlaySe(name);
}

void RaceApp::Unload()
{
    if (!race)
        return;
    race.reset();
    RaceUnloadWorld();
    dsGameAudio::Get()->EndRace(); // main 0x0208bf50, from the race's unload (main 0x02097aa0 area)
}

void RaceApp::ToTracks()
{
    if (track >= 0)
        selected = track;
    Unload();
    m_ed.reset();
    m_run = RUN_TRACK;
    screen = TRACKS;
    BuildContextFor(selected);
    dsGameAudio *ga = dsGameAudio::Get();
    ga->StartFrontEnd();
    ga->PlayMusic(dsGameAudio::MUSIC_FRONT_END, 0);
}

void RaceApp::Horn()
{
    if (screen == PLAYING && race)
        race->Horn();
}

bool RaceApp::Start(int index)
{
    if (index < 0 || index >= m_count)
        return false;
    Unload();
    // The front end's music stops before the race's (the front end's exit is not ported; StartRace keeps a race's
    // music for the next race).
    dsGameAudio *ga = dsGameAudio::Get();
    if (ga->state != dsGameAudio::RACE)
        ga->PlayMusic(dsGameAudio::MUSIC_NONE, 0);
    race.reset(new RaceSession);
    int env = IsPlatform(index) ? -1 : index / 26; // platform tracks: the scene's car, skin 0
    if (env >= 0) {
        race->skin = m_skin[env];
        race->timeOfDay = m_mode == MODE_QUICK ? m_tod : -1;
    }
    if (!race->Load(kTracks[index].id)) {
        ToTracks();
        return false;
    }
    race->lighting = race->lighting && lighting;
    if (env >= 0 && m_camera[env])
        race->ToggleCamera(); // the display options' camera of the environment
    race->recordSplits = m_splits[index];
    race->recordTime = m_best[index];
    LoadGhosts(index);
    track = selected = index;
    screen = PLAYING;
    m_finishFrames = -1;
    m_newRecord = false;
    if (menus)
        m_races++;
    WriteSave(); // remembers the last track
    return true;
}

void RaceApp::Restart()
{
    race->Restart();
    if (m_run == RUN_TEST && m_ed) { // the test drive starts again where the car was dropped
        race->progress->Drop(m_ed->dropPos, m_ed->dropDir);
        race->cams[race->camIndex].Reset();
    }
    m_finishFrames = -1;
    m_newRecord = false;
    screen = PLAYING;
}


void RaceApp::Frame(const dsCarInput &in)
{
    m_frame++;
    if (m_anim > 0)
        m_anim--;
    RaceFrame(in);
    if (screen == EDITING)
        EditorFrame();
    // The game audio's update runs in the race update (dsRace_Update -> main 0x0208c730); the 3D sources and the
    // sound manager (NNS_SndMain) once per frame of the main loop.
    if (race)
        race->AudioUpdate();
    else
        dsGameAudio::Get()->Update(nullptr, nullptr);
    dsSound3D::Get()->Update();
    dsSoundManager::Get()->Update();
}

void RaceApp::RaceFrame(const dsCarInput &in)
{
    if (screen != PLAYING || !race)
        return;
    race->Frame(in);
    const dsRaceProgress &rp = *race->progress;
    if (!rp.finished) {
        m_finishFrames = -1;
        return;
    }
    if (m_run != RUN_TRACK) {
        // The editor's runs: a validation sets the author time, both go back to the editor after the delay; a
        // puzzle and a saved track give their medals (PuzzleProgression times / the validation's) and the results.
        if (m_finishFrames < 0) {
            m_finishFrames = 0;
            if (m_run == RUN_VALIDATE)
                EditorFinished();
            if (m_run == RUN_PUZZLE || m_run == RUN_CUSTOM) {
                fx32 times[3] = {};
                int coppers = 0, puzzle = m_ed ? m_ed->puzzle : -1;
                std::string diff;
                int need;
                fx32 *best = nullptr;
                if (m_run == RUN_PUZZLE && PuzzleInfo(puzzle, &diff, &need, times, &coppers)) {
                    best = &m_puzzleBest[(size_t)puzzle];
                } else if (m_run == RUN_CUSTOM) {
                    times[0] = race->track.info.goldTime;
                    times[1] = race->track.info.silverTime;
                    times[2] = race->track.info.bronzeTime;
                    for (auto &b : m_customBest)
                        if (b.first == m_custom)
                            best = &b.second;
                    if (!best) {
                        m_customBest.push_back({m_custom, 0});
                        best = &m_customBest.back().second;
                    }
                }
                fx32 t = rp.finishTime;
                auto medal = [&](fx32 v) {
                    for (int m = 0; m < 3; m++)
                        if (v > 0 && times[m] > 0 && v <= times[m])
                            return 3 - m;
                    return 0;
                };
                int before = best ? medal(*best) : 0;
                m_runMedal = medal(t);
                m_medalsWon = m_runMedal > before ? m_runMedal - before : 0;
                m_coppersWon = m_run == RUN_PUZZLE ? m_medalsWon * coppers : 0;
                m_coppers += m_coppersWon;
                m_newRecord = best && t > 0 && (*best <= 0 || t < *best);
                if (m_newRecord)
                    *best = t;
                race->recordTime = best ? *best : 0;
                WriteSave();
            }
        } else if (++m_finishFrames >= RESULT_DELAY && (menus || m_run == RUN_TEST || m_run == RUN_VALIDATE)) {
            if (m_run == RUN_TEST || m_run == RUN_VALIDATE) {
                EditorBack();
                if (m_ed && m_ed->validated)
                    EditorMessage(FeFmt("%s\n%s %s   %s %s   %s %s", FeS("helpvalidatetitle"), FeS("ghostgold"),
                                        FeTime(m_ed->ed->header.goldTime).c_str(), FeS("ghostsilver"),
                                        FeTime(m_ed->ed->header.silverTime).c_str(), FeS("ghostbronze"),
                                        FeTime(m_ed->ed->header.bronzeTime).c_str()));
            } else {
                screen = RESULT;
                item = 0;
                dsCarAudio::Get()->FadeOut(15);
            }
        }
        return;
    }
    if (m_finishFrames < 0) {
        m_finishFrames = 0;
        // Medals won: the run's medal above the best one so far; coppers: the track's amount per medal won (the
        // quick race gives none). Port reading of RaceProgression / PlatformProgression, to confirm in ov004.
        int retries = IsPlatform(track) ? rp.retries : -1;
        m_runMedal = MedalFor(track, rp.finishTime, retries);
        int before = Medal(track);
        m_medalsWon = m_mode == MODE_QUICK || m_runMedal <= before ? 0 : m_runMedal - before;
        m_coppersWon = m_medalsWon * Coppers(track);
        m_coppers += m_coppersWon;
        if (menus)
            m_finishes++;
        bool better;
        if (IsPlatform(track)) {
            // Platform: fewer retries first, then the time.
            better = rp.finishTime > 0 && (m_bestRetries[track] < 0 || rp.retries < m_bestRetries[track] ||
                                           (rp.retries == m_bestRetries[track] && rp.finishTime < m_best[track]));
        } else {
            better = rp.finishTime > 0 && (m_best[track] <= 0 || rp.finishTime < m_best[track]);
        }
        if (better) {
            if (IsPlatform(track))
                m_bestRetries[track] = rp.retries;
            m_best[track] = rp.finishTime;
            m_splits[track] = race->hud.splits;
            race->recordSplits = m_splits[track];
            race->recordTime = m_best[track];
            m_newRecord = true;
            // The best run becomes the player's ghost.
            std::string path = PlayerGhostPath(track);
            if (!path.empty() && race->recorder.Recording()) {
                std::vector<u8> g = race->recorder.Save(rp.finishTime, race->hud.splits, 1, (int)race->hud.splits.size());
                if (FILE *f = fopen(path.c_str(), "wb")) {
                    fwrite(g.data(), 1, g.size(), f);
                    fclose(f);
                }
            }
        }
        WriteSave();
    } else if (++m_finishFrames >= RESULT_DELAY && menus) {
        screen = RESULT;
        item = 0;
        dsCarAudio::Get()->FadeOut(15); // port choice: the car falls silent under the result menu
    }
}

void RaceApp::Overlay(const FeCanvas &fe, int w, int h, float s, RaceTextFn text, RaceRectFn rect,
                      RaceSpriteFn sprite, void *user) const
{
    if (screen == TRACKS) {
        DrawFrontEnd(fe);
        return;
    }
    if (screen == EDITING) {
        if (m_ed)
            DrawEditor(fe);
        return;
    }
    if (race) {
        RaceHudBacking(w, h, race->hud, rect, user);
        RaceHudSprites(w, h, race->hud, sprite, user);
        RaceHudLayout(w, h, s, *race->progress, text, user);
    }
    if (screen != PLAYING)
        DrawRaceMenu(fe);
}
