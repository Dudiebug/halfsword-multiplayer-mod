// The text rules of the whole UI (match/hsm_ui_text.h, docs/UI_STYLE.md): how a hint is measured, how a count takes its unit,
// the units in the Score limit and Time limit values, the one name Carnage, and a walk over every hint the scenes build,
// each held to kHintMax characters.
#include "match/hsm_ui_text.h"
#include "match/hsm_class.h"
#include "match/hsm_lobby_mods.h"
#include "match/hsm_lobby_scene.h"
#include "match/hsm_mp_scene.h"
#include "match/hsm_scenarios.h"
#include "match/hsm_win_by.h"
#include <algorithm>
#include <cstdio>

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++g_fail; } } while (0)
using namespace hsm;

// A line of exactly n characters (ASCII).
static std::string Chars(size_t n) { return std::string(n, 'x'); }

// The cap is 120 characters a line: 120 fits, 121 does not; an empty hint fits.
static void TestHintFits() {
    CHECK(kHintMax == 120);
    CHECK(HintFits("") && HintFits("Short.") && HintFits(Chars(119)) && HintFits(Chars(120)));
    CHECK(!HintFits(Chars(121)) && !HintFits(Chars(500)));
    // several lines: each line on its own, so two lines of 100 fit although together they do not
    CHECK(HintFits(Chars(100) + "\n" + Chars(100)) && HintFits(Chars(120) + "\n" + Chars(120) + "\n" + Chars(1)));
    CHECK(!HintFits(Chars(10) + "\n" + Chars(121)) && !HintFits(Chars(121) + "\n" + Chars(10)) && !HintFits(Chars(5) + "\n" + Chars(5) + "\n" + Chars(121)));
    CHECK(HintFits("\n") && HintFits("a\n") && HintFits("\na"));
}

// Characters, not bytes: the middle dot is two bytes and one character, a curly quote three bytes and one.
static void TestChars() {
    CHECK(TextChars("") == 0 && TextChars("abc") == 3);
    CHECK(TextChars("a\xC2\xB7" "b") == 3 && TextChars("\xE2\x80\x99") == 1 && TextChars("\xF0\x9F\x98\x80") == 1);
    const std::string dots = [] { std::string s; for (int i = 0; i < 120; ++i) s += "\xC2\xB7"; return s; }();   // 240 bytes, 120 characters
    CHECK(dots.size() == 240 && TextChars(dots) == 120 && HintFits(dots) && !HintFits(dots + "\xC2\xB7"));
}

static void TestCountOf() {
    CHECK(CountOf(0, "kill", "kills") == "0 kills" && CountOf(1, "kill", "kills") == "1 kill" && CountOf(2, "kill", "kills") == "2 kills");
    CHECK(CountOf(11, "round", "rounds") == "11 rounds" && CountOf(100, "win", "wins") == "100 wins");
}

// The Score limit keeps one label; the unit is in the value, and the unit follows the mode.
static void TestLimitValue() {
    CHECK(LimitValue(Format::FreeForAll, 10) == "10 kills" && LimitValue(Format::FreeForAll, 1) == "1 kill");
    CHECK(LimitValue(Format::TwoTeams, 20) == "20 kills");
    CHECK(LimitValue(Format::Duel, 3) == "3 rounds" && LimitValue(Format::Duel, 1) == "1 round" && LimitValue(Format::LastManStanding, 5) == "5 rounds");
    CHECK(LimitValue(Format::Roulette, 6) == "6 points" && LimitValue(Format::Roulette, 1) == "1 point");
    CHECK(LimitValue(Format::PitBridgeDuel, 5) == "5 rounds" && LimitValue(Format::PitBridgeDuel, 1) == "1 round");   // an older host's Pit has no wins unit of its own any more
    // every format's limit options read with a unit and never as a bare number
    for (int i = 0; i < kFormatCount; ++i) {
        const Format f = Format(i);
        for (int n : LimitOptions(f)) {
            const std::string v = LimitValue(f, n);
            CHECK(v.size() > std::to_string(n).size() + 1 && v.rfind(std::to_string(n) + " ", 0) == 0);
        }
    }
}

static void TestTimeValue() {
    CHECK(TimeValue(Format::FreeForAll, 0) == "No limit" && TimeValue(Format::HideSeek, 0) == "No limit");
    CHECK(TimeValue(Format::FreeForAll, 600) == "10 min" && TimeValue(Format::Duel, 300) == "5 min" && TimeValue(Format::Infection, 900) == "15 min");
    CHECK(TimeValue(Format::HideSeek, 300) == "5 min a round" && TimeValue(Format::HideSeek, 120) == "2 min a round");
}

// Free-for-all is Carnage in everything a player reads; the format id and enumerator stay.
static void TestCarnage() {
    CHECK(std::string(FormatName(Format::FreeForAll)) == "Carnage" && std::string(FormatName(Format(kFormatCount))) == "Carnage");
    auto found = [](const char* mode) { LobbyFound f; f.mode = mode; f.players = 3; f.maxPlayers = 8; return f; };
    CHECK(BrowserModeShown(found("Free-for-all")) == "Carnage" && BrowserModeShown(found("Carnage")) == "Carnage" && BrowserModeShown(found("Teams (3)")) == "Teams (3)");
    CHECK(ModeMatches(found("Free-for-all"), "Carnage") && ModeMatches(found("Carnage"), "Carnage") && !ModeMatches(found("Free-for-all"), "Duel"));
    {   // the browser's mode filter offers Carnage and no longer Free-for-all
        const auto& modes = BrowserModes();
        CHECK(std::find(modes.begin(), modes.end(), "Carnage") != modes.end() && std::find(modes.begin(), modes.end(), "Free-for-all") == modes.end());
    }
    const LobbyScene s = BuildLobbyScene([] { LobbyLocal l; l.phase = LobbyPhase::InLobby; l.host = true; l.version = "0.2.0"; l.myName = "Ann"; return l; }());
    bool card = false;
    for (const ModeCard& m : s.modes) {
        CHECK(m.name.find("FREE-FOR-ALL") == std::string::npos && m.description.find("Free-for-all") == std::string::npos);
        if (m.format == Format::FreeForAll && !m.brawl && !m.crypts && !m.progression) { card = true; CHECK(m.name == "CARNAGE" && m.description == "Everyone for themselves."); }
    }
    CHECK(card);
}

static LobbyLocal Lobby(bool host, Format f, Gear g) {
    LobbyLocal l;
    l.phase = LobbyPhase::InLobby; l.host = host; l.version = "0.2.0"; l.myName = host ? "Ann" : "Bob";
    l.config.format = f; l.config.gear = g; l.config.scoreLimit = uint16_t(DefaultScoreLimit(f));
    LobbyPlayer a; a.name = "Ann"; a.host = true; a.me = host; a.team = 0;
    LobbyPlayer b; b.name = "Bob"; b.me = !host; b.team = 1; b.hostOverride = host ? kNoTeam : 0;   // the member was moved by the host
    LobbyPlayer c; c.name = "Cat"; c.team = 0;
    l.players = {a, b, c};
    return l;
}

static int g_hints = 0;
static void Hint(const char* where, const std::string& hint) {
    ++g_hints;
    if (!HintFits(hint)) { std::printf("FAIL hint over %zu characters (%zu) in %s: %s\n", kHintMax, TextChars(hint), where, hint.c_str()); ++g_fail; }
}
static void SceneHints(const LobbyScene& s) {
    for (const SettingRow& r : s.settings) Hint("a setting row", r.hint);
    for (const ModeCard& m : s.modes) { Hint("a mode card", m.description); Hint("a mode card hint", m.hint); }
    for (const MapCard& m : s.maps) { Hint("a map tag", m.tag); Hint("a map reason", m.reason); }
    Hint("the rules note", s.rulesNote);
}

// Every row, mode card and map card of every lobby the scene can build: each format and gear, host and member, each shuffle,
// bodies, Crypts run and Sync, Progression, a team moved by the host, bots and a class budget.
static void TestEveryLobbyHint() {
    for (int fi = 0; fi < kFormatCount; ++fi)
        for (int gi = 0; gi <= int(Gear::Crypts); ++gi)
            for (int host = 0; host < 2; ++host)
                for (int variant = 0; variant < 12; ++variant) {
                    LobbyLocal l = Lobby(host != 0, Format(fi), Gear(gi));
                    l.config.shuffle = TeamShuffle(variant % 3);
                    l.config.ownBodies = (variant & 1) != 0;
                    l.config.cryptsTogether = (variant & 2) != 0;
                    l.config.hostPhysics = (variant & 4) != 0;
                    l.classBudget = (variant & 8) != 0;
                    SceneHints(BuildLobbyScene(l));
                }
    for (int host = 0; host < 2; ++host) {
        LobbyLocal p = Lobby(host != 0, Format::FreeForAll, Gear::Own);
        p.progression = true;
        const LobbyScene s = BuildLobbyScene(p);
        SceneHints(s);
        CHECK(s.rulesNote == "The game's own rules, in the Tavern.");   // Progression says why RULES holds three rows
        for (const ModeCard& m : s.modes) if (m.progression) CHECK(m.description == "The game's Progression, together.");
    }
    // the rest of the modes say nothing in the note
    CHECK(BuildLobbyScene(Lobby(true, Format::Duel, Gear::Own)).rulesNote.empty());
    CHECK(g_hints > 20000);
}

static void TestSceneRowsWithSetting() {
    // The row hints that stay are the short ones the mock keeps: Crypts run, Wave break, Shuffle teams, My team, Bodies, Sync.
    LobbyLocal l = Lobby(true, Format::PlayersVsBots, Gear::Own);
    const LobbyScene s = BuildLobbyScene(l);
    bool wave = false;
    for (const SettingRow& r : s.settings) if (r.id == LobbyRow::WaveBreak) { wave = true; CHECK(r.hint == "Hold G to vote to skip it."); }
    CHECK(wave);
}

// The hints outside the lobby scene: the class fields, the Mods rules, the voice rows, the scenarios, Win by.
static void TestOtherHints() {
    for (int i = 0; i < kClassFieldCount; ++i) Hint("a class field", ClassFieldInfo(i));
    for (int p = 0; p <= int(kMaxModPreset); ++p)
        for (int cm = 0; cm <= int(kMaxClassesMode); ++cm)
            for (int host = 0; host < 2; ++host)
                for (int picked = 0; picked < 2; ++picked) {
                    LobbyModsLocal m; m.host = host != 0; m.classPicked = picked != 0;
                    m.chosen.preset = ModPreset(p); m.rule.preset = ModPreset(p);
                    m.chosen.classes = ClassesMode(cm); m.rule.classes = ClassesMode(cm);
                    Hint("a preset hint", ModPresetHint(ModPreset(p), host != 0));
                    Hint("a classes hint", ClassesModeHint(ClassesMode(cm)));
                    const LobbyModsScene s = BuildLobbyMods(m);
                    for (const ModRuleView& r : s.rules) Hint("a mods rule", r.hint);
                    m.rule.blockHse = false; m.chosen.blockHse = true; m.rule.officialOnly = false; m.chosen.officialOnly = true;
                    for (const ModRuleView& r : BuildLobbyMods(m).rules) Hint("a mods rule override", r.hint);
                }
    for (int reach = 0; reach < 3; ++reach)
        for (int bits = 0; bits < 16; ++bits)
            for (int binding = 0; binding < 2; ++binding) {
                MpVoice v; v.session = (bits & 1) != 0; v.host = (bits & 2) != 0; v.server = (bits & 4) != 0; v.monitor = (bits & 8) != 0;
                v.reach = voice::Reach(reach); v.mic = voice::Mic(reach);
                for (const VoiceRowView& r : BuildVoiceScene(v, binding != 0).rows) Hint("a voice row", r.hint);
            }
    Hint("the Blossfechten card", kBlossfechtenBlurb);                      // the scenario hints are gone with the Scenario row (0.9.0)
    for (size_t i = 0; i < kModeOrderCount; ++i) Hint("a mode card", FormatBlurb(kModeOrder[i].format));
    for (int w = 0; w <= int(WinBy::Knockdown); ++w) Hint("Win by", WinByHint(WinBy(w)));
    // the words that must not come back
    CHECK(std::string(kBlossfechtenBlurb).find("Experimental") == std::string::npos);
}

int main() {
    TestHintFits();
    TestChars();
    TestCountOf();
    TestLimitValue();
    TestTimeValue();
    TestCarnage();
    TestEveryLobbyHint();
    TestSceneRowsWithSetting();
    TestOtherHints();
    if (g_fail) { std::printf("%d check(s) failed\n", g_fail); return 1; }
    std::printf("ok\n");
    return 0;
}
