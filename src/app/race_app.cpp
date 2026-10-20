// Race application: track menu, pause / result menus, best times (see race_app.h).
#include "race_app.h"

#include <stdio.h>
#include <string.h>

namespace {

const RaceTrackEntry kTracks[] = {
    {"practice/0", "ENTRAINEMENT 1"}, {"practice/1", "ENTRAINEMENT 2"}, {"practice/2", "ENTRAINEMENT 3"},
    {"practice/3", "ENTRAINEMENT 4"}, {"practice/4", "ENTRAINEMENT 5"}, {"easy/0", "FACILE 1"},
    {"easy/1", "FACILE 2"},           {"easy/2", "FACILE 3"},           {"easy/3", "FACILE 4"},
    {"easy/4", "FACILE 5"},           {"medium/0", "MOYEN 1"},          {"medium/1", "MOYEN 2"},
    {"medium/2", "MOYEN 3"},          {"medium/3", "MOYEN 4"},          {"medium/4", "MOYEN 5"},
    {"hard/0", "DIFFICILE 1"},        {"hard/1", "DIFFICILE 2"},        {"hard/2", "DIFFICILE 3"},
    {"hard/3", "DIFFICILE 4"},        {"hard/4", "DIFFICILE 5"},        {"extreme/0", "EXTREME 1"},
    {"bonus/0", "BONUS 1"},           {"bonus/1", "BONUS 2"},           {"bonus/2", "BONUS 3"},
    {"bonus/3", "BONUS 4"},           {"bonus/4", "BONUS 5"},
};
const int kTrackCount = (int)(sizeof kTracks / sizeof kTracks[0]);

const char *const kPauseItems[] = {"CONTINUER", "RECOMMENCER", "PISTE SUIVANTE", "MENU DES PISTES"};
const char *const kResultItems[] = {"RECOMMENCER", "PISTE SUIVANTE", "MENU DES PISTES"};

const int VISIBLE_ROWS = 10;
const int RESULT_DELAY = 90; // frames (3 s at 30 Hz) between the finish and the result menu

void FormatTime(fx32 t, char *buf, size_t n)
{
    if (t <= 0) {
        snprintf(buf, n, "-:--.--");
        return;
    }
    int cs = (int)(((s64)(t & 0xfff) * 100) >> 12), sec = t >> 12;
    snprintf(buf, n, "%d:%02d.%02d", sec / 60, sec % 60, cs);
}

float TextWidth(const char *s, float px) { return 6 * px * (float)strlen(s); }

} // namespace

const RaceTrackEntry *RaceTracks(int *count)
{
    if (count)
        *count = kTrackCount;
    return kTracks;
}

RaceApp::RaceApp(const std::string &savePath, const char *okName, const char *backName)
    : m_savePath(savePath), m_ok(okName), m_back(backName), m_best(new fx32[kTrackCount]()),
      m_splits(new std::vector<fx32>[kTrackCount]), m_count(kTrackCount)
{
    LoadSave();
}

RaceApp::~RaceApp() { Unload(); }

int RaceApp::FindTrack(const char *id) const
{
    for (int i = 0; i < m_count; i++)
        if (!strcmp(kTracks[i].id, id))
            return i;
    return -1;
}

// Save file: "last <track>", "best <track> <fx32 time>" and "split <track> <fx32 time>..." lines (text, a few KB).
void RaceApp::LoadSave()
{
    if (m_savePath.empty())
        return;
    FILE *f = fopen(m_savePath.c_str(), "rb");
    if (!f)
        return;
    char line[1024], id[64];
    long t;
    int n;
    while (fgets(line, sizeof line, f)) {
        if (sscanf(line, "split %63s%n", id, &n) == 1) {
            int i = FindTrack(id);
            if (i < 0)
                continue;
            m_splits[i].clear();
            int k;
            for (const char *p = line + n; sscanf(p, "%ld%n", &t, &k) == 1; p += k)
                m_splits[i].push_back((fx32)t);
        } else if (sscanf(line, "best %63s %ld", id, &t) == 2) {
            int i = FindTrack(id);
            if (i >= 0 && t > 0)
                m_best[i] = (fx32)t;
        } else if (sscanf(line, "last %63s", id) == 1) {
            int i = FindTrack(id);
            if (i >= 0)
                selected = i;
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
            if (!m_splits[i].empty()) {
                fprintf(f, "split %s", kTracks[i].id);
                for (fx32 t : m_splits[i])
                    fprintf(f, " %ld", (long)t);
                fprintf(f, "\n");
            }
        }
    fclose(f);
}

void RaceApp::Unload()
{
    if (!race)
        return;
    race.reset();
    RaceUnloadWorld();
}

bool RaceApp::Start(int index)
{
    if (index < 0 || index >= m_count)
        return false;
    Unload();
    race.reset(new RaceSession);
    if (!race->Load(kTracks[index].id)) {
        Unload();
        screen = TRACKS;
        return false;
    }
    race->lighting = race->lighting && lighting;
    race->recordSplits = m_splits[index];
    race->recordTime = m_best[index];
    track = selected = index;
    screen = PLAYING;
    m_finishFrames = -1;
    m_newRecord = false;
    WriteSave(); // remembers the last track
    return true;
}

int RaceApp::ItemCount() const { return screen == PAUSED ? 4 : screen == RESULT ? 3 : 0; }

bool RaceApp::Press(Key k)
{
    switch (screen) {
    case TRACKS:
        if (k == UP)
            selected = (selected + m_count - 1) % m_count;
        else if (k == DOWN)
            selected = (selected + 1) % m_count;
        else if (k == LEFT)
            selected = selected >= 5 ? selected - 5 : 0;
        else if (k == RIGHT)
            selected = selected + 5 < m_count ? selected + 5 : m_count - 1;
        else if (k == OK || k == PAUSE)
            Start(selected);
        else if (k == BACK)
            return false;
        break;
    case PLAYING:
        if (k == PAUSE || k == BACK) {
            screen = PAUSED;
            item = 0;
        }
        break;
    case PAUSED:
    case RESULT: {
        int n = ItemCount();
        if (k == UP)
            item = (item + n - 1) % n;
        else if (k == DOWN)
            item = (item + 1) % n;
        else if ((k == BACK || k == PAUSE) && screen == PAUSED)
            screen = PLAYING;
        else if (k == BACK) {
            selected = track;
            Unload();
            screen = TRACKS;
        } else if (k == OK) {
            const char *what = screen == PAUSED ? kPauseItems[item] : kResultItems[item];
            if (!strcmp(what, "CONTINUER")) {
                screen = PLAYING;
            } else if (!strcmp(what, "RECOMMENCER")) {
                race->Restart();
                m_finishFrames = -1;
                m_newRecord = false;
                screen = PLAYING;
            } else if (!strcmp(what, "PISTE SUIVANTE")) {
                Start((track + 1) % m_count);
            } else {
                selected = track;
                Unload();
                screen = TRACKS;
            }
        }
        break;
    }
    }
    return true;
}

void RaceApp::Frame(const dsCarInput &in)
{
    if (screen != PLAYING || !race)
        return;
    race->Frame(in);
    const dsRaceProgress &rp = *race->progress;
    if (!rp.finished) {
        m_finishFrames = -1;
        return;
    }
    if (m_finishFrames < 0) {
        m_finishFrames = 0;
        if (rp.finishTime > 0 && (m_best[track] <= 0 || rp.finishTime < m_best[track])) {
            m_best[track] = rp.finishTime;
            m_splits[track] = race->hud.splits;
            race->recordSplits = m_splits[track];
            race->recordTime = m_best[track];
            m_newRecord = true;
            WriteSave();
        }
    } else if (++m_finishFrames >= RESULT_DELAY && menus) {
        screen = RESULT;
        item = 0;
    }
}

void RaceApp::Overlay(int w, int h, float s, RaceTextFn text, RaceRectFn rect, RaceSpriteFn sprite,
                      void *user) const
{
    char buf[64], tb[16];
    if (screen == TRACKS) {
        text(40 * s, 16 * s, 5 * s, "TM2PSP - COAST", 1, 0.85f, 0.2f, user);
        int first = selected - VISIBLE_ROWS / 2;
        if (first > m_count - VISIBLE_ROWS)
            first = m_count - VISIBLE_ROWS;
        if (first < 0)
            first = 0;
        for (int r = 0; r < VISIBLE_ROWS && first + r < m_count; r++) {
            int i = first + r;
            float y = (80 + r * 40) * s;
            bool sel = i == selected;
            if (sel)
                rect(28 * s, y - 8 * s, w - 56 * s, 44 * s, 0.1f, 0.2f, 0.5f, 0.75f, user);
            snprintf(buf, sizeof buf, "%s%s", sel ? "> " : "  ", kTracks[i].label);
            text(40 * s, y, 4 * s, buf, 1, 1, sel ? 0.4f : 1, user);
            FormatTime(m_best[i], tb, sizeof tb);
            text(w - 40 * s - TextWidth(tb, 4 * s), y, 4 * s, tb, 0.6f, 0.9f, 1, user);
        }
        snprintf(buf, sizeof buf, "%s JOUER   %s QUITTER", m_ok.c_str(), m_back.c_str());
        text(40 * s, h - 36 * s, 3 * s, buf, 0.8f, 0.8f, 0.8f, user);
        return;
    }
    if (race) {
        RaceHudBacking(w, h, race->hud, rect, user);
        RaceHudSprites(w, h, race->hud, sprite, user);
        RaceHudLayout(w, h, s, *race->progress, text, user);
    }
    if (screen == PLAYING)
        return;

    // Pause / result panel in the middle of the screen.
    int n = ItemCount();
    float pw = 520 * s, ph = (screen == RESULT ? 150 : 90) * s + n * 44 * s;
    float px = (w - pw) / 2, py = (h - ph) / 2;
    rect(px, py, pw, ph, 0, 0, 0, 0.7f, user);
    float y = py + 20 * s;
    if (screen == PAUSED) {
        text(w / 2 - TextWidth("PAUSE", 5 * s) / 2, y, 5 * s, "PAUSE", 1, 0.85f, 0.2f, user);
        y += 70 * s;
    } else {
        FormatTime(race ? race->progress->finishTime : 0, tb, sizeof tb);
        snprintf(buf, sizeof buf, "ARRIVEE %s", tb);
        text(w / 2 - TextWidth(buf, 5 * s) / 2, y, 5 * s, buf, 0.3f, 1, 0.3f, user);
        y += 56 * s;
        if (m_newRecord) {
            snprintf(buf, sizeof buf, "NOUVEAU RECORD!");
        } else {
            FormatTime(track >= 0 ? m_best[track] : 0, tb, sizeof tb);
            snprintf(buf, sizeof buf, "RECORD %s", tb);
        }
        text(w / 2 - TextWidth(buf, 4 * s) / 2, y, 4 * s, buf, 1, 0.9f, 0.3f, user);
        y += 70 * s;
    }
    for (int i = 0; i < n; i++) {
        const char *label = screen == PAUSED ? kPauseItems[i] : kResultItems[i];
        bool sel = i == item;
        if (sel)
            rect(px + 16 * s, y - 8 * s, pw - 32 * s, 40 * s, 0.1f, 0.2f, 0.5f, 0.9f, user);
        snprintf(buf, sizeof buf, "%s%s", sel ? "> " : "  ", label);
        text(px + 40 * s, y, 4 * s, buf, 1, 1, sel ? 0.4f : 1, user);
        y += 44 * s;
    }
}
