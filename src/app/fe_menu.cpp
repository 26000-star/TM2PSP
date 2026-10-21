// The front end's pages and the race's pause and results menus (see race_app.h). Content of the DS screens of ov004
// (titles and buttons by their keys in strings/main, the Scripts/config progression and shop tables), drawn in the
// TrackMania Wii style validated with tools/fe/mockup_main.py: the user's background, a white title tab, white
// rounded cards and buttons, the selected one enlarged with a blue outline, the PSP's button hints.
#include <ctype.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include <map>

#include "fe_text.h"
#include "race_app.h"
#include "editor.h"
#include "sound/dsCarAudio.h"
#include "sound/dsGameAudio.h"
#include "text/dsStringTable.h"

namespace {

const char *S(const char *key)
{
    const char *t = dsStringTable::Get()->Text(key);
    return *t ? t : key;
}

// Port texts (no DS string): French when the strings are French, else English.
const char *L(const char *fr, const char *en) { return dsStringTable::Get()->Language() == dsStringTable::LANG_FR ? fr : en; }

std::string Fmt(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
std::string Fmt(const char *fmt, ...)
{
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    return buf;
}

std::string Time(fx32 t)
{
    if (t <= 0)
        return "-:--.--";
    int cs = (int)(((s64)(t & 0xfff) * 100) >> 12), sec = t >> 12;
    return Fmt("%d:%02d.%02d", sec / 60, sec % 60, cs);
}

// The DS hints break their lines with '@' for the DS screen: spaces here, the panels wrap them.
std::string Lines(const char *s)
{
    std::string r = s;
    for (char &c : r)
        if (c == '@')
            c = ' ';
    return r;
}

const char *const kEnvKeys[4] = {"environmentstadiumbutton", "environmentislandbutton", "environmentsnowbutton",
                                 "environmentcoastbutton"};
const char *const kEnvPictures[4] = {"stadiumtopbg", "islandtopbg", "snowtopbg", "coasttopbg"};
const char *const kShopPictures[4] = {"shopstadiumtopbg", "shopislandtopbg", "shopsnowtopbg", "shopcoasttopbg"};
const char *const kSkinKeys[4] = {"stadiumskin%d", "islandskin%d", "snowskin%d", "coastskin%d"};
const char *const kCameraKeys[4] = {"stadiumcamera", "islandcamera", "snowcamera", "coastcamera"};
const char *const kDiffKeys[6] = {"difficultypracticebutton", "difficultyeasybutton", "difficultymediumbutton",
                                  "difficultyhardbutton", "difficultyextremebutton", "difficultyinsanebutton"};
const char *const kDiffBackKeys[6] = {"difficultypractice", "difficultyeasy", "difficultymedium", "difficultyhard",
                                      "difficultyextreme", "difficultyinsane"};
const char *const kLangKeys[6] = {"languageenglish", "languageenglish", "languagefrench", "languageitalian",
                                  "languagegerman", "languagespanish"};
const int kDiffFirst[7] = {0, 5, 10, 15, 20, 21, 26};
const int kPlatFirst[6] = {0, 4, 8, 12, 16, 17}; // platform difficulties practice..extreme (config order)

const u32 kInk = FeRgba(40, 40, 45), kGrey = FeRgba(150, 150, 155), kBlue = FeRgba(60, 140, 230),
          kWhite = FeRgba(250, 250, 250), kEdge = FeRgba(195, 195, 200), kSoft = FeRgba(90, 90, 100);

u32 Fade(u32 c, float a) { return (c & 0xffffff) | (u32)((c >> 24) * a) << 24; }

enum Action {
    A_NONE, A_SOLO, A_EDITOR, A_MULTI, A_SHOP, A_PROFILE, A_OPTIONS, A_RACE, A_PUZZLE, A_PLATFORM, A_QUICK,
    A_MYTRACKS, A_ENV, A_DIFF, A_TRACK, A_TOD, A_SKIN, A_VEHICLES, A_STATS, A_LANG, A_CONTROLS, A_DISPLAY, A_SOUND,
    A_DATA, A_CREDITS, A_MUSIC, A_SFX, A_SETLANG, A_CAMERA, A_DELGHOSTS, A_DELALL, A_SHOPTRACKS, A_SHOPSKINS,
    A_SHOPBLOCKS, A_SHOPENV, A_BUY, A_PAUSE_RESUME, A_PAUSE_RESTART, A_PAUSE_SKIP, A_PAUSE_QUIT, A_RES_RETRY,
    A_RES_NEXT, A_RES_CHANGE, A_RES_MENU, A_GHOSTOPP, A_GHOSTME, A_EDITENV, A_PUZZLEPICK, A_CUSTOM, A_EDIT,
    A_MODIFY, A_NEXTPUZZLE, A_PAUSE_GHOSTS
};
enum Confirm { C_NONE, C_QUIT, C_DELGHOSTS, C_DELALL, C_BUY };

const int ROWS = 5; // list rows shown at once

} // namespace

const char *FeS(const char *key) { return S(key); }
const char *FeL(const char *fr, const char *en) { return L(fr, en); }
std::string FeFmt(const char *fmt, ...)
{
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    return buf;
}
std::string FeTime(fx32 t) { return Time(t); }

struct RaceApp::Item {
    int action = A_NONE, arg = 0;
    std::string label, value;   // value: right-hand text (time, price, setting)
    const char *picture = nullptr;
    int iconBank = -1, icon = 0; // a FE cell before the label
    int blockEnv = -1, blockIcon = -1; // an editor block icon (Editor/Cell/<Env>Blocks) before the label
    int medal = -1;              // track rows: the best medal (0 none .. 3 gold)
    bool locked = false;
    std::string info;            // the right-hand panel's text, also the popup of a locked entry
    int track = -1;
};

// The context of a track: the menus it belongs to (after an auto start or a race, the front end opens there).
void RaceApp::BuildContextFor(int index)
{
    int count = 0;
    RaceTracks(&count);
    if (index < 0 || index >= count)
        return;
    m_stack.assign(1, {P_MAIN, 0});
    if (index >= 104) {
        int n = index - 104, d = 0;
        while (n >= kPlatFirst[d + 1])
            d++;
        if (m_mode == MODE_QUICK)
            m_mode = MODE_RACE;
        m_mode = MODE_PLATFORM;
        m_diff = d;
        m_stack.push_back({P_SOLO, 2});
        m_stack.push_back({P_DIFF, d});
        m_stack.push_back({P_TRACK, n - kPlatFirst[d]});
        return;
    }
    m_env = index / 26;
    int k = index % 26, d = 0;
    while (k >= kDiffFirst[d + 1])
        d++;
    m_diff = d;
    if (m_mode == MODE_QUICK) {
        m_stack.push_back({P_SOLO, 3});
        m_stack.push_back({P_ENV, m_env});
        m_stack.push_back({P_TRACK, k});
    } else {
        m_mode = MODE_RACE;
        m_stack.push_back({P_SOLO, 0});
        m_stack.push_back({P_ENV, m_env});
        m_stack.push_back({P_DIFF, d});
        m_stack.push_back({P_TRACK, k - kDiffFirst[d]});
    }
}

void RaceApp::Items(int page, std::vector<Item> *out) const
{
    out->clear();
    auto add = [&](int action, const char *label, int arg = 0) -> Item & {
        out->push_back(Item());
        Item &it = out->back();
        it.action = action;
        it.arg = arg;
        it.label = label;
        return it;
    };
    switch (page) {
    case P_MAIN:
        add(A_SOLO, S("solobutton")).picture = "solo";
        add(A_EDITOR, S("editorbutton")).picture = "track";
        add(A_MULTI, S("networkbutton")).picture = "multi";
        add(A_SHOP, S("shopbutton")).picture = "shop";
        add(A_PROFILE, S("profilebutton")).picture = "profile";
        add(A_OPTIONS, S("optionsbutton")).picture = "options";
        out->at(2).locked = true;
        out->at(2).info = L("Bient\xf4""t disponible.", "Coming soon.");
        break;
    case P_SOLO: {
        Item &race = add(A_RACE, S("racebutton"));
        race.picture = "racetopbg";
        race.info = Lines(S("hnt_srace2"));
        Item &puzzle = add(A_PUZZLE, S("puzzlebutton"));
        puzzle.picture = "puzzletopbg";
        puzzle.locked = !m_editorUnlocked;
        puzzle.info = puzzle.locked ? S("lockedpuzzlemode") : S("welcometopuzzlemsg");
        Item &plat = add(A_PLATFORM, S("platformbutton"));
        plat.picture = "platformtopbg";
        plat.locked = !PlatformUnlocked();
        plat.info = plat.locked ? S("lockedplatformmode") : Lines(S("hnt_prace1"));
        Item &quick = add(A_QUICK, S("quickracebutton"));
        quick.picture = "welcometoquickracetopbg";
        quick.info = Lines(S("hnt_qrace1"));
        Item &mine = add(A_MYTRACKS, S("customracebutton"));
        mine.picture = "mytrackstopbg";
        mine.locked = !m_editorUnlocked;
        mine.info = S("lockedcustommode");
        break;
    }
    case P_ENV:
    case P_SHOP_ENV:
        for (int e = 0; e < 4; e++) {
            Item &it = add(page == P_ENV ? A_ENV : A_SHOPENV, S(kEnvKeys[e]), e);
            it.picture = page == P_ENV ? kEnvPictures[e] : kShopPictures[e];
            it.iconBank = FeCanvas::BANK_ENV;
            it.icon = e;
            if (page == P_ENV) {
                int medals = 0;
                for (int i = e * 26; i < e * 26 + 26; i++)
                    medals += Medal(i);
                it.info = Fmt("%s : %d / 78", S("statisticsmedals"), medals);
            }
        }
        break;
    case P_DIFF: {
        bool plat = m_mode == MODE_PLATFORM;
        int have = MedalsTotal(plat);
        for (int d = 0; d < (plat ? 5 : 6); d++) {
            Item &it = add(A_DIFF, S(kDiffKeys[d]), d);
            int first = plat ? 104 + kPlatFirst[d] : m_env * 26 + kDiffFirst[d];
            int last = plat ? 104 + kPlatFirst[d + 1] : m_env * 26 + kDiffFirst[d + 1];
            int medals = 0;
            for (int i = first; i < last; i++)
                medals += Medal(i);
            it.value = Fmt("%d / %d", medals, (last - first) * 3);
            it.iconBank = FeCanvas::BANK_DIFFICULTY;
            it.icon = d < 5 ? d : 4;
            it.picture = plat ? "platformtopbg" : "difficultyselecttopbg";
            int need = RequiredMedals(first);
            it.locked = have < need;
            if (it.locked)
                it.info = Fmt(S("lockedrequirementbronze"), (unsigned)need) + " " +
                          S(plat ? "lockedrequirementplatformmedals" : "lockedrequirementracemedals") + " " +
                          S("lockedrequirementdifficulty") + Fmt("  (%d / %d)", have, need);
            else
                it.info = Fmt("%s : %d", S(plat ? "lockedrequirementplatformmedals" : "lockedrequirementracemedals"),
                              have);
        }
        break;
    }
    case P_TRACK: {
        int first, last;
        if (m_mode == MODE_PLATFORM) {
            first = 104 + kPlatFirst[m_diff];
            last = 104 + kPlatFirst[m_diff + 1];
        } else if (m_mode == MODE_QUICK) {
            first = m_env * 26;
            last = first + 26;
        } else {
            first = m_env * 26 + kDiffFirst[m_diff];
            last = m_env * 26 + kDiffFirst[m_diff + 1];
        }
        for (int i = first; i < last; i++) {
            int d = m_diff, n = i - first;
            if (m_mode == MODE_QUICK) {
                int k = i % 26;
                for (d = 0; k >= kDiffFirst[d + 1]; d++) {
                }
                n = k - kDiffFirst[d];
            }
            Item &it = add(A_TRACK, Fmt("%s %d", S(kDiffKeys[d]), n + 1).c_str(), i);
            it.track = i;
            it.medal = Medal(i);
            it.value = i >= 104 && m_bestRetries[i] >= 0 ? Fmt("%d", m_bestRetries[i]) : Time(m_best[i]);
            it.locked = !Unlocked(i);
            int shop = ShopTrack(i);
            if (it.locked)
                it.info = shop >= 0 && MedalsTotal(i >= 104) >= RequiredMedals(i) ? S("lockedrequirementpurchase")
                                                                                  : S("lockedgenericmessage");
        }
        break;
    }
    case P_EDITOR_ENV:
        for (int e = 0; e < 4; e++) {
            Item &it = add(A_EDITENV, S(kEnvKeys[e]), e);
            it.picture = kEnvPictures[e];
            it.iconBank = FeCanvas::BANK_ENV;
            it.icon = e;
            int n = (int)SavedTracks(e).size();
            it.info = Fmt("%s : %d", S("customdatanumtracks"), n);
        }
        break;
    case P_PUZZLES: {
        // config/PuzzleProgression: unlocked by the puzzle medals won ("Win %u Puzzle Medals").
        int have = 0;
        for (int i = 0; i < PuzzleCount(); i++)
            have += PuzzleMedal(i);
        std::map<std::string, int> perDiff;
        for (int i = 0; i < PuzzleCount(); i++) {
            std::string diff;
            int need, coppers;
            fx32 times[3];
            PuzzleInfo(i, &diff, &need, times, &coppers);
            static const char *const kPuzzleDiffs[5] = {"practice", "easy", "medium", "hard", "extreme"};
            int d = 0;
            while (d < 4 && diff != kPuzzleDiffs[d])
                d++;
            int k = perDiff[diff]++;
            Item &it = add(A_PUZZLEPICK, Fmt("%s %d", S(kDiffKeys[d]), k + 1).c_str(), i);
            it.iconBank = FeCanvas::BANK_ENV;
            it.icon = k % 4;
            it.medal = PuzzleMedal(i);
            it.value = Time(i < (int)m_puzzleBest.size() ? m_puzzleBest[(size_t)i] : 0);
            it.locked = have < need;
            it.info = it.locked ? Fmt(S("lockedrequirementbronze"), (unsigned)need) + " " +
                                      S("lockedrequirementpuzzlemedals") + Fmt("  (%d / %d)", have, need)
                                : Fmt("%s %s\n%s %s\n%s %s", S("ghostgold"), Time(times[0]).c_str(), S("ghostsilver"),
                                      Time(times[1]).c_str(), S("ghostbronze"), Time(times[2]).c_str());
        }
        break;
    }
    case P_MYTRACKS:
        for (const SavedTrack &t : SavedList()) {
            Item &it = add(A_CUSTOM, t.name.c_str());
            it.iconBank = FeCanvas::BANK_ENV;
            it.icon = t.env;
            fx32 best = 0;
            for (const auto &b : m_customBest)
                if (b.first == t.name)
                    best = b.second;
            it.value = Time(best);
            it.locked = t.times[0] <= 0;
            it.info = it.locked ? S("customdatasavevalidatemsg")
                                : Fmt("%s %s\n%s %s\n%s %s\n%s : %s", S("ghostgold"), Time(t.times[1]).c_str(),
                                      S("ghostsilver"), Time(t.times[2]).c_str(), S("ghostbronze"),
                                      Time(t.times[3]).c_str(), S("challengetype"),
                                      S(t.platform ? "platformbutton" : "racebutton"));
        }
        if (out->empty()) {
            Item &it = add(A_NONE, S("customdatanotracksmsg"));
            it.locked = true;
        }
        break;
    case P_DAYTIME: {
        static const char *const kKeys[3] = {"daytimesunrisebutton", "daytimedaytimbutton", "daytimesunsetbutton"};
        static const char *const kPics[3] = {"sunrisetopbg", "daytimetopbg", "sunsettopbg"};
        for (int t = 0; t < 3; t++)
            add(A_TOD, S(kKeys[t]), t).picture = kPics[t];
        break;
    }
    case P_SKIN:
        for (int s = 0; s < (int)m_ownedSkins.size(); s++) {
            int env, price, n;
            std::string name;
            if (!ShopEntry(1, s, &env, &price, &n, &name) || env != m_env)
                continue;
            Item &it = add(A_SKIN, S(Fmt(kSkinKeys[m_env], n + 1).c_str()), n);
            it.picture = "vehicleselecttopbg";
            it.locked = !Owned(1, s);
            it.info = it.locked ? S("lockedrequirementpurchase") : "";
            if (n == m_skin[m_env])
                it.value = "*";
        }
        break;
    case P_DISPLAY:
        for (int e = 0; e < 4; e++) {
            Item &it = add(A_CAMERA, S(kCameraKeys[e]), e);
            it.value = S(m_camera[e] ? "cameratypechase2" : "cameratypechase1");
            it.picture = "displaytopbg";
            it.iconBank = FeCanvas::BANK_ENV;
            it.icon = e;
        }
        {
            Item &g = add(A_GHOSTOPP, S("ghostopponentsprefix"));
            g.value = S(GhostChoiceKey(m_ghostChoice));
            g.picture = "displaytopbg";
            g.info = S("welcometoracemsg");
            Item &p = add(A_GHOSTME, S("playerghostprefix"));
            p.value = S(m_ghostPlayer ? "playerghoston" : "playerghostoff");
            p.picture = "displaytopbg";
        }
        break;
    case P_PROFILE: {
        Item &name = add(A_NONE, S("profilenamebutton"));
        name.value = m_name;
        name.picture = "profiletopbg";
        add(A_VEHICLES, S("profilevehiclebutton")).picture = "vehicleselecttopbg";
        add(A_STATS, S("profilestatisticsbutton")).picture = "profiletopbg";
        break;
    }
    case P_STATS: {
        auto row = [&](const char *key, const std::string &v) {
            Item &it = add(A_NONE, S(key));
            it.value = v;
            it.picture = "profiletopbg";
        };
        row("statisticsname", m_name);
        row("statisticsnumraces", Fmt("%d", m_races));
        row("statisticsfinishcount", Fmt("%d", m_finishes));
        row("statisticsmedals", Fmt("%d", MedalsTotal(false) + MedalsTotal(true)));
        row("statisticscoppers", Fmt("%d", m_coppers));
        int records = 0;
        for (int i = 0; i < m_count; i++)
            records += m_best[i] > 0;
        row("statisticsracerecordstitle", Fmt("%d", records));
        break;
    }
    case P_OPTIONS: {
        auto opt = [&](int a, const char *key, const char *help, const char *pic) {
            Item &it = add(a, S(key));
            it.info = help ? S(help) : "";
            it.picture = pic;
        };
        opt(A_LANG, "optionslangselectbutton", "optionslangselecthelp", "optionstopscreenbg");
        opt(A_CONTROLS, "optionscontroloptionsbutton", "optionscontroloptionshelp", "controlstopbg");
        opt(A_DISPLAY, "optionsdisplayoptionsbutton", "optionsdisplayoptionshelp", "displaytopbg");
        opt(A_SOUND, "optionssoundbutton", "optionssoundhelp", "soundtopbg");
        opt(A_DATA, "optionsdeletedatabutton", "optionsdeletedatahelp", "deletegamedatatopbg");
        opt(A_CREDITS, "optionscreditsbutton", nullptr, "creditstopbg");
        break;
    }
    case P_AUDIO: {
        Item &m = add(A_MUSIC, S("menumusicmodeprefix"));
        m.value = S(m_musicOn ? "menumusicon" : "menumusicoff");
        m.picture = "soundtopbg";
        Item &s = add(A_SFX, S("menusfxmodeprefix"));
        s.value = S(m_sfxOn ? "menusfxon" : "menusfxoff");
        s.picture = "soundtopbg";
        break;
    }
    case P_LANG:
        // English, French, Italian, German, Spanish (the DS's gb / fr / it / de / es files).
        for (int l : {0, 2, 3, 4, 5}) {
            Item &it = add(A_SETLANG, S(kLangKeys[l]), l);
            it.picture = "optionstopscreenbg";
            if (l == m_language)
                it.value = "*";
        }
        break;
    case P_DATA: {
        Item &g = add(A_DELGHOSTS, S("deleteghostsbutton"));
        g.picture = "deletegamedatatopbg";
        g.info = L("Efface les meilleurs temps et les records.", "Erases the best times and records.");
        Item &a = add(A_DELALL, S("deleteallbutton"));
        a.picture = "deletegamedatatopbg";
        a.info = S("optionsdeletedatahelp");
        break;
    }
    case P_SHOP: {
        Item &t = add(A_SHOPTRACKS, S("shoptracksbutton"));
        t.picture = "shoptopscreenbg";
        t.info = Lines(S("hnt_coppers2"));
        Item &b = add(A_SHOPBLOCKS, S("shopblocksbutton"));
        b.picture = "shoptopscreenbg";
        b.info = L("Des blocs de plus pour l'\xe9""diteur de circuits.", "More blocks for the track editor.");
        Item &s = add(A_SHOPSKINS, S("shopskinsbutton"));
        s.picture = "shoptopscreenbg";
        s.info = Lines(S("hnt_coppers1"));
        break;
    }
    case P_SHOP_ITEMS: {
        // Tracks (config/TrackShop) or skins (config/SkinShop) of the environment, with their price.
        int count = (int)(m_shopKind == 3 ? m_ownedBlocks.size() : m_shopKind ? m_ownedSkins.size() : m_ownedTracks.size());
        for (int n = 0; n < count; n++) {
            int env, price, index;
            std::string name;
            if (!ShopEntry(m_shopKind, n, &env, &price, &index, &name) || env != m_env)
                continue;
            std::string key = name;
            for (char &c : key)
                c = (char)tolower((unsigned char)c);
            // Blocks: their PieceData name after the environment's prefix, and their editor icon.
            Item &it = add(A_BUY, m_shopKind == 3 ? name.substr(4).c_str() : S(key.c_str()), n);
            it.picture = kShopPictures[m_env];
            if (m_shopKind == 3) {
                it.blockEnv = m_env;
                it.blockIcon = BlockIconOf(m_env, name.substr(4));
                it.picture = nullptr;
            }
            bool owned = Owned(m_shopKind, n);
            it.value = owned ? L("Achet\xe9", "Owned") : Fmt("%d", price);
            it.iconBank = owned || m_shopKind == 3 ? -1 : FeCanvas::BANK_COPPERS;
            it.icon = 0;
            it.locked = owned;
            if (!m_shopKind)
                it.info = Fmt("%s %d", S(kDiffKeys[(index % 26) / 5]), 5);
            if (m_shopKind == 3)
                it.info = owned ? L("Disponible dans l'\xe9""diteur.", "Available in the editor.")
                                : Fmt("%s : %d", L("Prix", "Price"), price);
        }
        break;
    }
    }
}

void RaceApp::Open(int page, int cursor)
{
    m_stack.push_back({page, cursor});
    m_anim = 6;
    m_animDir = 1;
}

void RaceApp::Back()
{
    if (m_stack.size() > 1)
        m_stack.pop_back();
    m_anim = 6;
    m_animDir = -1;
}

void RaceApp::Popup(const std::string &text, int confirm)
{
    m_popup = text;
    m_confirm = confirm;
    m_confirmCursor = 1;
}

void RaceApp::StartChosen(int index)
{
    Sound("MENU_SELECT");
    Start(index);
}

void RaceApp::Choose(const Item &it)
{
    if (it.locked && it.action != A_BUY) {
        Sound("MENU_BACK");
        if (!it.info.empty())
            Popup(it.info);
        return;
    }
    Sound("MENU_SELECT");
    switch (it.action) {
    case A_SOLO: Open(P_SOLO); break;
    case A_EDITOR: Open(P_EDITOR_ENV, m_env); break;
    case A_EDITENV:
        m_env = it.arg;
        if (!OpenEditor(it.arg))
            Popup(L("Impossible d'ouvrir l'\xe9""diteur.", "Cannot open the editor."));
        break;
    case A_PUZZLE: Open(P_PUZZLES, 0); break;
    case A_PUZZLEPICK:
        if (!OpenEditor(-1, it.arg))
            Popup(L("Puzzle introuvable.", "Puzzle missing."));
        break;
    case A_MYTRACKS: Open(P_MYTRACKS, 0); break;
    case A_CUSTOM:
        if (!StartCustom(it.label))
            Popup(S("customdatanotracksmsg"));
        break;
    case A_SHOP: Open(P_SHOP); break;
    case A_PROFILE: Open(P_PROFILE); break;
    case A_OPTIONS: Open(P_OPTIONS); break;
    case A_RACE: m_mode = MODE_RACE; Open(P_ENV, m_env); break;
    case A_QUICK: m_mode = MODE_QUICK; Open(P_ENV, m_env); break;
    case A_PLATFORM: m_mode = MODE_PLATFORM; Open(P_DIFF, 0); break;
    case A_ENV:
        m_env = it.arg;
        if (m_mode == MODE_QUICK)
            Open(P_TRACK, 0);
        else
            Open(P_DIFF, 0);
        break;
    case A_DIFF: m_diff = it.arg; Open(P_TRACK, 0); break;
    case A_TRACK:
        selected = it.track;
        if (m_mode == MODE_PLATFORM)
            Start(it.track);
        else if (m_mode == MODE_QUICK)
            Open(P_DAYTIME, m_tod < 0 ? 1 : m_tod);
        else
            Open(P_SKIN, m_skin[m_env]);
        break;
    case A_TOD: m_tod = it.arg; Open(P_SKIN, m_skin[m_env]); break;
    case A_SKIN:
        m_skin[m_env] = it.arg;
        WriteSave();
        if (m_stack.size() >= 2 && m_stack[m_stack.size() - 2].page == P_PROFILE)
            Back();
        else
            Start(selected);
        break;
    case A_VEHICLES: Open(P_SHOP_ENV, m_env); m_shopKind = 2; break;
    case A_STATS: Open(P_STATS); break;
    case A_LANG: Open(P_LANG, m_language >= 2 ? m_language - 1 : 0); break; // the list skips us
    case A_CONTROLS:
        Popup(L("Course : croix directionnelle ou stick pour tourner, X acc\xe9""l\xe8""re, carr\xe9"" freine, triangle revient au "
                "dernier checkpoint, START pause, SELECT cam\xe9""ra, L klaxon.\nMenus : croix, X choisir, O retour.",
                "Race: d-pad or stick to steer, X accelerates, square brakes, triangle goes back to the last "
                "checkpoint, START pause, SELECT camera, L horn.\nMenus: d-pad, X choose, O back."));
        break;
    case A_DISPLAY: Open(P_DISPLAY); break;
    case A_SOUND: Open(P_AUDIO); break;
    case A_DATA: Open(P_DATA); break;
    case A_CREDITS:
        Popup("TrackMania Turbo (DS)\nFirebrand Games - Nadeo - Focus Home Interactive\n\n" +
              std::string(L("Portage PSP non officiel : TM2PSP", "Unofficial PSP port: TM2PSP")));
        break;
    case A_MUSIC: {
        m_musicOn = !m_musicOn;
        dsGameAudio *ga = dsGameAudio::Get();
        ga->musicOn = m_musicOn;
        ga->PlayMusic(m_musicOn ? dsGameAudio::MUSIC_FRONT_END : dsGameAudio::MUSIC_NONE, 0);
        WriteSave();
        break;
    }
    case A_SFX: m_sfxOn = !m_sfxOn; WriteSave(); break;
    case A_SETLANG:
        m_languageSaved = false;
        SetLanguage(it.arg);
        m_languageSaved = true;
        WriteSave();
        break;
    case A_CAMERA: m_camera[it.arg] ^= 1; WriteSave(); break;
    case A_GHOSTOPP: m_ghostChoice = (m_ghostChoice + 1) % GHOSTS_COUNT; WriteSave(); break;
    case A_GHOSTME: m_ghostPlayer = !m_ghostPlayer; WriteSave(); break;
    case A_DELGHOSTS: Popup(std::string(S("deleteghostconfirmmsg")) + "\n" + S("deleteghostconfirmmsg2"), C_DELGHOSTS); break;
    case A_DELALL: Popup(std::string(S("deleteallbutton")) + " ?\n" + S("deletetrackconfirmmsg2"), C_DELALL); break;
    case A_SHOPTRACKS: m_shopKind = 0; Open(P_SHOP_ENV, m_env); break;
    case A_SHOPSKINS: m_shopKind = 1; Open(P_SHOP_ENV, m_env); break;
    case A_SHOPBLOCKS: m_shopKind = 3; Open(P_SHOP_ENV, m_env); break;
    case A_SHOPENV:
        m_env = it.arg;
        if (m_shopKind == 2)
            Open(P_SKIN, m_skin[m_env]);
        else
            Open(P_SHOP_ITEMS, 0);
        break;
    case A_BUY: {
        if (it.locked)
            break;
        int env, price, index;
        std::string name;
        ShopEntry(m_shopKind, it.arg, &env, &price, &index, &name);
        if (m_coppers < price) {
            Popup(S("shopinsufficentfunds"));
        } else {
            m_buy = it.arg;
            Popup(Fmt(S("shopconfirmpurchase"), it.label.c_str(), price), C_BUY);
        }
        break;
    }
    default: break;
    }
}

void RaceApp::Confirmed(int action)
{
    switch (action) {
    case C_QUIT: ToTracks(); break;
    case C_DELGHOSTS:
        for (int i = 0; i < m_count; i++) {
            m_best[i] = 0;
            m_bestRetries[i] = -1;
            m_splits[i].clear();
        }
        WriteSave();
        break;
    case C_DELALL: {
        for (int i = 0; i < m_count; i++) {
            m_best[i] = 0;
            m_bestRetries[i] = -1;
            m_splits[i].clear();
        }
        m_coppers = m_races = m_finishes = 0;
        std::fill(m_ownedTracks.begin(), m_ownedTracks.end(), 0);
        std::fill(m_ownedBlocks.begin(), m_ownedBlocks.end(), 0);
        for (size_t i = 0; i < m_ownedSkins.size(); i++) {
            int env, price, index;
            std::string name;
            ShopEntry(1, (int)i, &env, &price, &index, &name);
            m_ownedSkins[i] = price == 0;
        }
        for (int e = 0; e < 4; e++)
            m_skin[e] = m_camera[e] = 0;
        WriteSave();
        m_stack.assign(1, {P_MAIN, 0});
        break;
    }
    case C_BUY: {
        int env, price, index;
        std::string name;
        if (ShopEntry(m_shopKind, m_buy, &env, &price, &index, &name) && m_coppers >= price) {
            m_coppers -= price;
            (m_shopKind == 3 ? m_ownedBlocks : m_shopKind ? m_ownedSkins : m_ownedTracks)[m_buy] = 1;
            WriteSave();
        }
        break;
    }
    }
}

std::vector<int> RaceApp::RaceMenu() const
{
    bool paused = screen == PAUSED;
    switch (m_run) {
    case RUN_TEST:
    case RUN_VALIDATE: return {A_PAUSE_RESUME, A_PAUSE_RESTART, A_EDIT};
    case RUN_PUZZLE:
        if (paused)
            return {A_PAUSE_RESUME, A_PAUSE_RESTART, A_MODIFY, A_PAUSE_QUIT};
        return {A_RES_RETRY, A_MODIFY, A_NEXTPUZZLE, A_RES_MENU};
    case RUN_CUSTOM:
        if (paused)
            return {A_PAUSE_RESUME, A_PAUSE_RESTART, A_PAUSE_QUIT};
        return {A_RES_RETRY, A_RES_CHANGE, A_RES_MENU};
    default:
        if (paused && m_mode == MODE_RACE && track >= 0 && track < 104)
            return {A_PAUSE_RESUME, A_PAUSE_RESTART, A_PAUSE_GHOSTS, A_PAUSE_SKIP, A_PAUSE_QUIT};
        if (paused)
            return {A_PAUSE_RESUME, A_PAUSE_RESTART, A_PAUSE_SKIP, A_PAUSE_QUIT};
        return {A_RES_RETRY, A_RES_NEXT, A_RES_CHANGE, A_RES_MENU};
    }
}

int RaceApp::ItemCount() const { return screen == PAUSED || screen == RESULT ? (int)RaceMenu().size() : 0; }

void RaceApp::PressScript(const char *keys)
{
    for (const char *p = keys; p && *p; p++) {
        static const char kLetters[] = "UDLROBP";
        const char *k = strchr(kLetters, *p);
        if (k && *p)
            Press((Key)(k - kLetters));
    }
}

bool RaceApp::PressMenu(Key k)
{
    PageState &ps = m_stack.back();
    std::vector<Item> items;
    Items(ps.page, &items);
    int n = (int)items.size();
    if (ps.cursor >= n)
        ps.cursor = n ? n - 1 : 0;
    int c = ps.cursor;
    bool cards = ps.page == P_MAIN || ps.page == P_ENV || ps.page == P_SHOP_ENV || ps.page == P_DAYTIME ||
                 ps.page == P_EDITOR_ENV;
    if (ps.page == P_MAIN) {
        // Two rows of three: the cards, then the buttons.
        if (k == LEFT)
            c = c % 3 ? c - 1 : c + 2;
        else if (k == RIGHT)
            c = c % 3 < 2 ? c + 1 : c - 2;
        else if (k == UP || k == DOWN)
            c = (c + 3) % 6;
    } else if (n) {
        if ((cards && k == LEFT) || (!cards && k == UP))
            c = (c + n - 1) % n;
        else if ((cards && k == RIGHT) || (!cards && k == DOWN))
            c = (c + 1) % n;
        else if (!cards && k == LEFT)
            c = c >= ROWS ? c - ROWS : 0;
        else if (!cards && k == RIGHT)
            c = c + ROWS < n ? c + ROWS : n - 1;
    }
    if (c != ps.cursor) {
        Sound("MENU_MOVE");
        ps.cursor = c;
    }
    if (k == OK && n)
        Choose(items[(size_t)ps.cursor]);
    else if (k == BACK) {
        Sound("MENU_BACK");
        if (m_stack.size() <= 1)
            return false;
        Back();
    }
    return true;
}

bool RaceApp::Press(Key k)
{
    // A popup takes the keys first: a message closes with OK / BACK, a question moves between yes and no.
    if (!m_popup.empty() && screen != PLAYING) {
        if (m_confirm && (k == LEFT || k == RIGHT)) {
            m_confirmCursor ^= 1;
            Sound("MENU_MOVE");
        } else if (k == OK || k == BACK) {
            int action = k == OK && m_confirm && m_confirmCursor == 0 ? m_confirm : C_NONE;
            Sound(k == OK ? "MENU_SELECT" : "MENU_BACK");
            m_popup.clear();
            m_confirm = 0;
            if (action)
                Confirmed(action);
        }
        return true;
    }
    // Menu sounds: MENU_MOVE, MENU_SELECT, MENU_BACK. The pause fades the car's sounds out over 30 frames and back
    // in (ov003 0x0210dd64 / 0x0210dbec).
    switch (screen) {
    case EDITING: break; // the editor reads the pad (EditorPad)
    case TRACKS:
        if (k == PAUSE)
            k = OK;
        return PressMenu(k);
    case PLAYING:
        if (k == PAUSE || k == BACK) {
            screen = PAUSED;
            item = 0;
            m_ghostMenu = false;
            dsCarAudio::Get()->FadeOut(30);
        }
        break;
    case PAUSED:
        if (m_ghostMenu) {
            // "Concurrents": all, gold, silver, bronze, none (the DS's order); the choice applies at once.
            static const int kOrder[GHOSTS_COUNT] = {GHOSTS_ALL, GHOSTS_GOLD, GHOSTS_SILVER, GHOSTS_BRONZE, GHOSTS_NONE};
            if (k == UP || k == DOWN) {
                Sound("MENU_MOVE");
                item = (item + (k == UP ? GHOSTS_COUNT - 1 : 1)) % GHOSTS_COUNT;
            } else if (k == BACK || k == PAUSE) {
                Sound("MENU_BACK");
                m_ghostMenu = false;
                item = 2;
            } else if (k == OK) {
                Sound("MENU_SELECT");
                m_ghostChoice = kOrder[item];
                WriteSave();
                if (race && track >= 0)
                    LoadGhosts(track);
                m_ghostMenu = false;
                item = 2;
            }
            break;
        }
        // fall through
    case RESULT: {
        int n = ItemCount();
        if (k == UP || k == DOWN) {
            Sound("MENU_MOVE");
            item = (item + (k == UP ? n - 1 : 1)) % n;
        } else if ((k == BACK || k == PAUSE) && screen == PAUSED) {
            Sound("MENU_BACK");
            screen = PLAYING;
            dsCarAudio::Get()->FadeIn(30);
        } else if (k == BACK) {
            Sound("MENU_BACK");
            ToTracks();
        } else if (k == OK) {
            Sound("MENU_SELECT");
            std::vector<int> menu = RaceMenu();
            int a = item < (int)menu.size() ? menu[(size_t)item] : A_PAUSE_RESUME;
            switch (a) {
            case A_PAUSE_GHOSTS: {
                static const int kPos[GHOSTS_COUNT] = {1, 2, 3, 0, 4}; // gold, silver, bronze, all, none
                m_ghostMenu = true;
                item = kPos[m_ghostChoice];
                break;
            }
            case A_EDIT:
            case A_MODIFY: EditorBack(); break;
            case A_NEXTPUZZLE: {
                int next = m_ed ? (m_ed->puzzle + 1) % PuzzleCount() : 0;
                if (!OpenEditor(-1, next))
                    ToTracks();
                break;
            }
            case A_PAUSE_RESUME:
                screen = PLAYING;
                dsCarAudio::Get()->FadeIn(30);
                break;
            case A_PAUSE_RESTART:
            case A_RES_RETRY: Restart(); break;
            case A_PAUSE_SKIP:
            case A_RES_NEXT: {
                // The next track of the list that is open to the player (the same one when none is).
                int next = NextTrack(track);
                while (next != track && !Unlocked(next) && menus)
                    next = NextTrack(next);
                Start(next);
                break;
            }
            case A_PAUSE_QUIT: Popup(S("quitconfirmmsg"), C_QUIT); break;
            case A_RES_CHANGE:
                if (m_run == RUN_CUSTOM) {
                    ToTracks();
                    m_stack.assign(1, {P_MAIN, 0});
                    m_stack.push_back({P_SOLO, 4});
                    m_stack.push_back({P_MYTRACKS, 0});
                } else {
                    ToTracks();
                }
                break;
            default:
                ToTracks();
                m_stack.assign(1, {P_MAIN, 0});
                break;
            }
        }
        break;
    }
    }
    return true;
}

// ---------------------------------------------------------------------------------------------------------------
// Drawing.

void RaceApp::DrawTitle(const FeCanvas &fe, const char *title, float a) const
{
    float w = fe.TextWidth(FeCanvas::FONT28, title);
    fe.Panel(-20, 18, w + 54, 34, 17, Fade(kWhite, a), Fade(kBlue, a), 2);
    fe.Text(FeCanvas::FONT28, title, 14, 18 + (34 - fe.CapHeight(FeCanvas::FONT28)) * 0.5f, Fade(kInk, a));
}

void RaceApp::DrawHints(const FeCanvas &fe, bool back) const
{
    float y = FeCanvas::H - 22, x = FeCanvas::W - 14;
    std::string ok = S("okbuttontext"), bk = S("backbuttontext");
    if (back) {
        x -= fe.TextWidth(FeCanvas::FONT20, bk.c_str());
        fe.Text(FeCanvas::FONT20, bk.c_str(), x, y, kSoft);
        x -= 6 + fe.TextWidth(FeCanvas::FONT20, "O");
        fe.Text(FeCanvas::FONT20, "O", x, y, FeRgba(220, 60, 60));
        x -= 18;
    }
    x -= fe.TextWidth(FeCanvas::FONT20, ok.c_str());
    fe.Text(FeCanvas::FONT20, ok.c_str(), x, y, kSoft);
    x -= 6 + fe.TextWidth(FeCanvas::FONT20, "X");
    fe.Text(FeCanvas::FONT20, "X", x, y, kBlue);
}

static void DrawCard(const FeCanvas &fe, float x0, float y0, float x1, float y1, const char *picture,
                     const char *label, bool sel, bool locked, const char *badge, float a)
{
    float w = x1 - x0, h = y1 - y0;
    fe.RoundRect(x0 + 2, y0 + 3, w, h, 8, Fade(FeRgba(0, 0, 0, 70), a));
    fe.Panel(x0, y0, w, h, 8, Fade(kWhite, a), Fade(sel ? kBlue : kEdge, a), sel ? 3 : 1);
    float ih = floorf(h * 0.72f);
    if (const FeImage *img = picture ? FeImageNamed(picture) : nullptr)
        fe.ImageCover(*img, x0 + 4, y0 + 4, w - 8, ih - 4, Fade(locked ? FeRgba(150, 150, 150) : 0xffffffff, a));
    if (badge) {
        float bw = fe.TextWidth(FeCanvas::FONT20, badge) + 14;
        fe.RoundRect(x1 - bw - 8, y0 + 8, bw, 18, 9, Fade(FeRgba(30, 30, 35, 200), a));
        fe.Text(FeCanvas::FONT20, badge, x1 - bw / 2 - 8, y0 + 17 - fe.CapHeight(FeCanvas::FONT20) / 2,
                Fade(kWhite, a), 1);
    }
    fe.Text(FeCanvas::FONT20, label, (x0 + x1) * 0.5f, y0 + ih + (h - ih - fe.CapHeight(FeCanvas::FONT20)) * 0.5f,
            Fade(locked ? kGrey : kInk, a), 1);
}

void RaceApp::DrawPanel(const FeCanvas &fe, const Item &it, float x, float y, float w, float h) const
{
    fe.Panel(x, y, w, h, 8, kWhite, kEdge, 1);
    float ty = y + 10;
    if (it.track >= 0) {
        // Track: its medal times (the provided ghosts) or target retries, and the best run.
        bool plat = it.track >= 104;
        fe.Text(FeCanvas::FONT28, it.label.c_str(), x + 10, ty, kInk);
        ty += 30;
        static const char *const kMedalKeys[4] = {"", "ghostbronze", "ghostsilver", "ghostgold"};
        for (int m = 3; m >= 1; m--) {
            fe.Cell(FeCanvas::BANK_COPPERS, m - 1, x + 10, ty - 6, 0.75f, Fade(0xffffffff, Medal(it.track) >= m ? 1 : 0.45f));
            fe.Text(FeCanvas::FONT20, S(kMedalKeys[m]), x + 34, ty, kInk);
            std::string v;
            if (plat) {
                int n = it.track - 104;
                int target = MedalRetries(n, m);
                v = Fmt("%d", target);
            } else {
                v = Time(MedalTime(it.track, m));
            }
            fe.Text(FeCanvas::FONT20, v.c_str(), x + w - 10, ty, kInk, 2);
            ty += 24;
        }
        ty += 4;
        fe.Rect(x + 10, ty - 6, w - 20, 1, kEdge);
        fe.Text(FeCanvas::FONT20, S(plat ? "recordedtries" : "besttext"), x + 10, ty + 2, kSoft);
        std::string best = plat ? (m_bestRetries[it.track] >= 0 ? Fmt("%d", m_bestRetries[it.track]) : "-")
                                : Time(m_best[it.track]);
        fe.Text(FeCanvas::FONT20, best.c_str(), x + w - 10, ty + 2, kInk, 2);
        ty += 24;
        if (it.locked)
            fe.Text(FeCanvas::FONT20, it.info.c_str(), x + 10, ty + 2, FeRgba(200, 60, 50), 0, w - 20);
        return;
    }
    if (const FeImage *img = it.picture ? FeImageNamed(it.picture) : nullptr) {
        fe.ImageCover(*img, x + 8, y + 8, w - 16, floorf((w - 16) / 2), it.locked ? FeRgba(160, 160, 160) : 0xffffffff);
        ty = y + 8 + floorf((w - 16) / 2) + 10;
    } else if (it.blockIcon >= 0) {
        FeBlockIcon(fe, it.blockEnv, it.blockIcon, x + w / 2 - 40, y + 10, 80, 0xffffffff);
        fe.Text(FeCanvas::FONT20, it.label.c_str(), x + w / 2, y + 96, kInk, 1);
        ty = y + 120;
    }
    if (!it.info.empty())
        fe.Text(FeCanvas::FONT20, it.info.c_str(), x + 10, ty, it.locked ? FeRgba(200, 60, 50) : kInk, 0, w - 20);
}

void RaceApp::DrawPage(const FeCanvas &fe, int page, int cursor, float dx, float a) const
{
    std::vector<Item> items;
    Items(page, &items);
    int n = (int)items.size();
    if (cursor >= n)
        cursor = n ? n - 1 : 0;
    static const char *const kTitles[] = {
        "mainmenutitle", "solomenutitle", "raceenviromentselectmenutitle", "difficultymenutitle", "racemodetitle",
        "daytimemenutitle", "liveryselecttitle", "profilemenutitle", "statisticsmenutitle", "optionsmenutitle",
        "audiomenutitle", "languagemenutitle", "datamenutitle", "shopmenutitle", "raceenviromentselectmenutitle",
        "shopmenutitle", "optionscreditsbutton", "displayoptionsmenutitle", "editormenutitle", "puzzlemodetitle",
        "customracebutton"};
    const char *title = S(kTitles[page]);
    if (page == P_TRACK)
        title = S(m_mode == MODE_PLATFORM ? "platformmodetitle" : m_mode == MODE_QUICK ? "quickracebutton"
                                                                                         : "racemodetitle");
    DrawTitle(fe, title, a);
    if (page == P_MAIN) {
        static const float xs[3] = {32, 183, 325};
        for (int i = 0; i < 3 && i < n; i++) {
            float x = xs[i] + dx;
            bool sel = i == cursor;
            const char *badge = items[i].locked ? L("Bient\xf4""t", "Soon") : nullptr;
            if (sel)
                DrawCard(fe, x - 6, 62, x + 134, 204, items[i].picture, items[i].label.c_str(), true, false, badge, a);
            else
                DrawCard(fe, x, 72, x + 116, 194, items[i].picture, items[i].label.c_str(), false, false, badge, a);
        }
        for (int i = 3; i < 6 && i < n; i++) {
            float x = xs[i - 3] + 4 + dx;
            bool sel = i == cursor;
            float x0 = sel ? x - 3 : x, y0 = sel ? 212 : 214, x1 = sel ? x + 121 : x + 118, y1 = sel ? 244 : 242;
            fe.Panel(x0, y0, x1 - x0, y1 - y0, 7, Fade(kWhite, a), Fade(sel ? kBlue : kEdge, a), sel ? 3 : 1);
            float tx = x0 + 8;
            if (const FeImage *ic = FeImageNamed(items[i].picture)) {
                float ih = y1 - y0 - 8, iw = ic->w * ih / ic->h;
                fe.Image(*ic, tx, y0 + 4, iw, ih, Fade(0xffffffff, a));
                tx += iw + 6;
            }
            fe.Text(FeCanvas::FONT20, items[i].label.c_str(), tx, (y0 + y1 - fe.CapHeight(FeCanvas::FONT20)) * 0.5f,
                    Fade(kInk, a));
        }
        return;
    }
    if (page == P_ENV || page == P_SHOP_ENV || page == P_DAYTIME || page == P_EDITOR_ENV) {
        float cw = n == 3 ? 128 : 100, gap = 12, total = n * cw + (n - 1) * gap, x0 = (FeCanvas::W - total) / 2;
        for (int i = 0; i < n; i++) {
            float x = x0 + i * (cw + gap) + dx;
            bool sel = i == cursor;
            if (sel)
                DrawCard(fe, x - 6, 66, x + cw + 6, 202, items[i].picture, items[i].label.c_str(), true,
                         items[i].locked, nullptr, a);
            else
                DrawCard(fe, x, 76, x + cw, 192, items[i].picture, items[i].label.c_str(), false, items[i].locked,
                         nullptr, a);
        }
        if (n && !items[cursor].info.empty())
            fe.Text(FeCanvas::FONT20, items[cursor].info.c_str(), FeCanvas::W / 2 + dx, 214, Fade(kInk, a), 1);
        return;
    }
    // A list on the left, the selected entry's panel on the right.
    int first = cursor - ROWS / 2;
    if (first > n - ROWS)
        first = n - ROWS;
    if (first < 0)
        first = 0;
    const float lx = 24 + dx, lw = 222, rh = 28, step = 33, ly = 66;
    for (int r = 0; r < ROWS && first + r < n; r++) {
        const Item &it = items[(size_t)(first + r)];
        bool sel = first + r == cursor;
        float y = ly + r * step;
        float x0 = sel ? lx - 3 : lx, y0 = sel ? y - 2 : y, w = sel ? lw + 6 : lw, h = sel ? rh + 4 : rh;
        fe.Panel(x0, y0, w, h, 7, Fade(kWhite, a), Fade(sel ? kBlue : kEdge, a), sel ? 3 : 1);
        float tx = lx + 10, cy = y + (rh - fe.CapHeight(FeCanvas::FONT20)) * 0.5f;
        if (it.blockIcon >= 0) {
            FeBlockIcon(fe, it.blockEnv, it.blockIcon, tx - 4, y + (rh - 22) * 0.5f, 22, Fade(0xffffffff, a));
            tx += 22;
        }
        if (it.iconBank >= 0) {
            float k = it.iconBank == FeCanvas::BANK_DIFFICULTY ? 1 : 0.62f;
            fe.Cell(it.iconBank, it.icon, tx - 2, y + (rh - 16 * (k < 1 ? 32 * k / 16 : 1)) * 0.5f, k, Fade(0xffffffff, a));
            tx += it.iconBank == FeCanvas::BANK_DIFFICULTY ? 12 : 22;
        }
        fe.Text(FeCanvas::FONT20, it.label.c_str(), tx, cy, Fade(it.locked && it.action != A_BUY ? kGrey : kInk, a));
        float vx = lx + lw - 10;
        if (it.medal >= 0) {
            if (it.medal > 0)
                fe.Cell(FeCanvas::BANK_COPPERS, it.medal - 1, vx - 18, y + 5, 0.75f, Fade(0xffffffff, a));
            vx -= 24;
        }
        if (!it.value.empty())
            vx -= fe.Text(FeCanvas::FONT20, it.value.c_str(), vx, cy, Fade(it.locked ? kGrey : kSoft, a), 2);
        if (it.action == A_BUY && it.iconBank >= 0)
            fe.Cell(FeCanvas::BANK_COPPERS, 0, vx - 20, y + 5, 0.75f, Fade(0xffffffff, a));
    }
    // Scroll marks.
    if (first > 0)
        fe.Rect(lx + lw / 2 - 10, ly - 8, 20, 3, Fade(kBlue, a));
    if (first + ROWS < n)
        fe.Rect(lx + lw / 2 - 10, ly + ROWS * step - 1, 20, 3, Fade(kBlue, a));
    if (n && a > 0.99f)
        DrawPanel(fe, items[(size_t)cursor], 262 + dx, 62, 196, 176);
}

void RaceApp::DrawPopup(const FeCanvas &fe) const
{
    if (m_popup.empty())
        return;
    fe.Rect(0, 0, FeCanvas::W, FeCanvas::H, FeRgba(0, 0, 0, 110));
    const float x = 80, w = 320;
    float th = fe.TextHeight(FeCanvas::FONT20, m_popup.c_str(), w - 32);
    float h = fminf(th + 34 + (m_confirm ? 40 : 0), 200), y = floorf((FeCanvas::H - h) / 2) - 6;
    fe.RoundRect(x + 2, y + 3, w, h, 10, FeRgba(0, 0, 0, 80));
    fe.Panel(x, y, w, h, 10, kWhite, kBlue, 2);
    fe.Text(FeCanvas::FONT20, m_popup.c_str(), x + 16, y + 16, kInk, 0, w - 32);
    if (m_confirm) {
        const char *yes = S("yesbuttontext"), *no = S("nobuttontext");
        for (int i = 0; i < 2; i++) {
            bool sel = i == m_confirmCursor;
            float bx = x + w / 2 + (i ? 12 : -102), by = y + h - 36;
            fe.Panel(bx, by, 90, 26, 7, kWhite, sel ? kBlue : kEdge, sel ? 3 : 1);
            fe.Text(FeCanvas::FONT20, i ? no : yes, bx + 45, by + 13 - fe.CapHeight(FeCanvas::FONT20) / 2, kInk, 1);
        }
    }
}

void RaceApp::DrawFrontEnd(const FeCanvas &fe) const
{
    if (const FeImage *bg = FeImageNamed("background"))
        fe.Image(*bg, 0, 0, FeCanvas::W, FeCanvas::H);
    else
        fe.Rect(0, 0, FeCanvas::W, FeCanvas::H, FeRgba(200, 210, 225));
    // Coppers, top right (the DS shows them on the shop and profile screens; here on every page).
    std::string coins = Fmt("%d", m_coppers);
    float cw = fe.TextWidth(FeCanvas::FONT20, coins.c_str()) + 40;
    fe.Panel(FeCanvas::W - cw - 10, 22, cw, 26, 13, kWhite, kEdge, 1);
    fe.Cell(FeCanvas::BANK_COPPERS, 0, FeCanvas::W - cw - 4, 25, 0.83f);
    fe.Text(FeCanvas::FONT20, coins.c_str(), FeCanvas::W - 20, 35 - fe.CapHeight(FeCanvas::FONT20) / 2, kInk, 2);
    const PageState &ps = m_stack.back();
    float t = m_anim / 6.0f;
    DrawPage(fe, ps.page, ps.cursor, t * t * 40 * m_animDir, 1 - t * 0.8f);
    DrawHints(fe, m_stack.size() > 1);
    DrawPopup(fe);
}

void RaceApp::DrawRaceMenu(const FeCanvas &fe) const
{
    fe.Rect(0, 0, FeCanvas::W, FeCanvas::H, FeRgba(0, 0, 0, 90));
    bool plat = track >= 104;
    if (m_ghostMenu && screen == PAUSED) {
        // "Concurrents": each provided opponent with its finish time, the current choice ticked.
        DrawTitle(fe, S("selectghosttitle"), 1);
        static const int kOrder[GHOSTS_COUNT] = {GHOSTS_ALL, GHOSTS_GOLD, GHOSTS_SILVER, GHOSTS_BRONZE, GHOSTS_NONE};
        const float lx = 24, lw = 260, rh = 28, step = 33, ly = 70;
        for (int i = 0; i < GHOSTS_COUNT; i++) {
            int c = kOrder[i];
            bool sel = i == item;
            float y = ly + i * step;
            fe.Panel(sel ? lx - 3 : lx, sel ? y - 2 : y, sel ? lw + 6 : lw, sel ? rh + 4 : rh, 7, kWhite,
                     sel ? kBlue : kEdge, sel ? 3 : 1);
            float ty = y + (rh - fe.CapHeight(FeCanvas::FONT20)) * 0.5f;
            if (c <= GHOSTS_BRONZE) {
                fe.Cell(FeCanvas::BANK_COPPERS, 2 - c, lx + 8, y + 3, 0.7f);
                fe.Text(FeCanvas::FONT20, Time(MedalTime(track, 3 - c)).c_str(), lx + lw - 34, ty, kSoft, 2);
            }
            fe.Text(FeCanvas::FONT20, S(GhostChoiceKey(c)), lx + 34, ty, kInk);
            if (c == m_ghostChoice)
                fe.Text(FeCanvas::FONT20, "*", lx + lw - 12, ty, kBlue, 2);
        }
        DrawHints(fe, true);
        DrawPopup(fe);
        return;
    }
    DrawTitle(fe, S(screen == PAUSED ? "pausemsg" : "raceresultstitle"), 1);
    std::vector<int> menu = RaceMenu();
    std::vector<const char *> items;
    for (int a : menu) {
        switch (a) {
        case A_PAUSE_RESUME: items.push_back(S("resumemsg")); break;
        case A_PAUSE_RESTART: items.push_back(S("restartracemsg")); break;
        case A_PAUSE_SKIP: items.push_back(S("skiptrackmsg")); break;
        case A_PAUSE_QUIT: items.push_back(S("quitmsg")); break;
        case A_PAUSE_GHOSTS: items.push_back(S("selectghosttitle")); break;
        case A_RES_RETRY: items.push_back(S("retrybutton")); break;
        case A_RES_NEXT: items.push_back(S(plat ? "nextplatformbutton" : "nextracebutton")); break;
        case A_RES_CHANGE: items.push_back(S("changetrackbutton")); break;
        case A_EDIT: items.push_back(S("edittrackbutton")); break;
        case A_MODIFY: items.push_back(S("modifypuzzlebutton")); break;
        case A_NEXTPUZZLE: items.push_back(S("nextpuzzlebutton")); break;
        default: items.push_back(S("mainmenubutton")); break;
        }
    }
    const float lx = 24, lw = 200, rh = 28, step = 33, ly = 80;
    for (int i = 0; i < (int)items.size(); i++) {
        bool sel = i == item;
        float y = ly + i * step;
        fe.Panel(sel ? lx - 3 : lx, sel ? y - 2 : y, sel ? lw + 6 : lw, sel ? rh + 4 : rh, 7, kWhite, sel ? kBlue : kEdge,
                 sel ? 3 : 1);
        fe.Text(FeCanvas::FONT20, items[i], lx + 10, y + (rh - fe.CapHeight(FeCanvas::FONT20)) * 0.5f, kInk);
    }
    // Right panel: the track and, on the results, the run's time, record, medals and coppers.
    const float x = 244, y = 62, w = 214, h = 182;
    fe.Panel(x, y, w, h, 8, kWhite, kEdge, 1);
    float ty = y + 12;
    if (track >= 0) {
        int count = 0;
        const RaceTrackEntry *tr = RaceTracks(&count);
        int k = plat ? track - 104 : track % 26, d = 0;
        const int *firsts = plat ? kPlatFirst : kDiffFirst;
        while (k >= firsts[d + 1])
            d++;
        std::string name = plat ? Fmt("%s %s %d", S("platformmodetitle"), S(kDiffBackKeys[d]), k - firsts[d] + 1)
                                : Fmt("%s %s %d", S(kEnvKeys[track / 26]), S(kDiffBackKeys[d]), k - firsts[d] + 1);
        (void)tr;
        fe.Text(FeCanvas::FONT20, name.c_str(), x + 10, ty, kSoft);
        ty += 22;
    }
    if (m_run != RUN_TRACK) {
        // The editor's runs: the track, the run's time and medals (puzzle, My Tracks) or its medal times.
        std::string name = m_run == RUN_CUSTOM ? m_custom
                           : m_ed && m_ed->puzzle >= 0 ? PuzzleName(m_ed->puzzle)
                           : m_ed && !m_ed->file.empty() ? m_ed->file : std::string(S("editormsg"));
        fe.Text(FeCanvas::FONT20, name.c_str(), x + 10, ty, kSoft);
        ty += 22;
        if (screen == RESULT && race) {
            fe.Text(FeCanvas::FONT20, S("recordedtime"), x + 10, ty, kSoft);
            fe.Text(FeCanvas::FONT32, Time(race->progress->finishTime).c_str(), x + w - 10, ty + 14, kInk, 2);
            ty += 46;
            fe.Text(FeCanvas::FONT20, S("besttext"), x + 10, ty, kSoft);
            std::string best = Time(race->recordTime);
            if (m_newRecord)
                best = "* " + best;
            fe.Text(FeCanvas::FONT20, best.c_str(), x + w - 10, ty, m_newRecord ? kBlue : kInk, 2);
            ty += 24;
            fe.Text(FeCanvas::FONT20, S("medalsawarded"), x + 10, ty, kSoft);
            ty += 18;
            if (m_medalsWon > 0) {
                for (int m = 0; m < m_medalsWon; m++)
                    fe.Cell(FeCanvas::BANK_COPPERS, m_runMedal - m_medalsWon + m,
                            x + w - 28 - (m_medalsWon - 1 - m) * 20, ty - 5, 0.75f);
            } else {
                fe.Text(FeCanvas::FONT20, S("medalsawardednone"), x + w - 10, ty, kInk, 2);
            }
            ty += 22;
            fe.Text(FeCanvas::FONT20, S("coppersawarded"), x + 10, ty, kSoft);
            fe.Text(FeCanvas::FONT20, Fmt("+%d", m_coppersWon).c_str(), x + w - 30, ty, kInk, 2);
            fe.Cell(FeCanvas::BANK_COPPERS, 0, x + w - 26, ty - 6, 0.75f);
        } else if (race) {
            static const char *const kMedalKeys[3] = {"ghostgold", "ghostsilver", "ghostbronze"};
            fx32 times[3] = {race->track.info.goldTime, race->track.info.silverTime, race->track.info.bronzeTime};
            std::string diff;
            int need, coppers;
            if (m_ed && m_ed->puzzle >= 0)
                PuzzleInfo(m_ed->puzzle, &diff, &need, times, &coppers);
            for (int m = 0; m < 3; m++) {
                fe.Cell(FeCanvas::BANK_COPPERS, 2 - m, x + 10, ty - 6, 0.75f);
                fe.Text(FeCanvas::FONT20, S(kMedalKeys[m]), x + 34, ty, kInk);
                fe.Text(FeCanvas::FONT20, Time(times[m]).c_str(), x + w - 10, ty, kInk, 2);
                ty += 24;
            }
        }
    } else if (screen == RESULT && race) {
        const dsRaceProgress &rp = *race->progress;
        fe.Text(FeCanvas::FONT20, S("recordedtime"), x + 10, ty, kSoft);
        fe.Text(FeCanvas::FONT32, Time(rp.finishTime).c_str(), x + w - 10, ty + 14, kInk, 2);
        ty += 46;
        if (plat) {
            fe.Text(FeCanvas::FONT20, S("recordedtries"), x + 10, ty, kSoft);
            fe.Text(FeCanvas::FONT20, Fmt("%d", rp.retries).c_str(), x + w - 10, ty, kInk, 2);
            ty += 22;
        }
        fe.Text(FeCanvas::FONT20, S("besttext"), x + 10, ty, kSoft);
        std::string best = Time(track >= 0 ? m_best[track] : 0);
        if (m_newRecord)
            best = "* " + best;
        fe.Text(FeCanvas::FONT20, best.c_str(), x + w - 10, ty, m_newRecord ? kBlue : kInk, 2);
        ty += 24;
        fe.Text(FeCanvas::FONT20, S("medalsawarded"), x + 10, ty, kSoft);
        ty += 18; // the medals on the next line, under their long label
        if (m_medalsWon > 0) {
            for (int m = 0; m < m_medalsWon; m++)
                fe.Cell(FeCanvas::BANK_COPPERS, m_runMedal - m_medalsWon + m, x + w - 28 - (m_medalsWon - 1 - m) * 20,
                        ty - 5, 0.75f);
        } else {
            fe.Text(FeCanvas::FONT20, S("medalsawardednone"), x + w - 10, ty, kInk, 2);
        }
        ty += 22;
        fe.Text(FeCanvas::FONT20, S("coppersawarded"), x + 10, ty, kSoft);
        fe.Text(FeCanvas::FONT20, Fmt("+%d", m_coppersWon).c_str(), x + w - 30, ty, kInk, 2);
        fe.Cell(FeCanvas::BANK_COPPERS, 0, x + w - 26, ty - 6, 0.75f);
    } else if (track >= 0 && !plat) {
        static const char *const kMedalKeys[4] = {"", "ghostbronze", "ghostsilver", "ghostgold"};
        for (int m = 3; m >= 1; m--) {
            fe.Cell(FeCanvas::BANK_COPPERS, m - 1, x + 10, ty - 6, 0.75f, Fade(0xffffffff, Medal(track) >= m ? 1 : 0.45f));
            fe.Text(FeCanvas::FONT20, S(kMedalKeys[m]), x + 34, ty, kInk);
            fe.Text(FeCanvas::FONT20, Time(MedalTime(track, m)).c_str(), x + w - 10, ty, kInk, 2);
            ty += 24;
        }
        fe.Text(FeCanvas::FONT20, S("besttext"), x + 10, ty + 4, kSoft);
        fe.Text(FeCanvas::FONT20, Time(m_best[track]).c_str(), x + w - 10, ty + 4, kInk, 2);
    }
    DrawHints(fe, screen == PAUSED);
    DrawPopup(fe);
}
