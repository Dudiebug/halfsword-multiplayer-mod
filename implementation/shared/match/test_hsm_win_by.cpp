// The Win by row (match/hsm_win_by.h, #1407): which formats take it, the rule a match runs, the lose condition it puts
// on a fighter, the stepper, the lobby row and Sanitize.
#include "match/hsm_lobby_scene.h"
#include "match/hsm_win_by.h"
#include <cstdio>
#include <cstring>
#include <string>

using namespace hsm;

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++g_fail; } } while (0)

static LobbyLocal Lobby(Format f, bool host = true) {
    LobbyLocal l;
    l.phase = LobbyPhase::InLobby; l.host = host;
    l.config = SelectFormat(l.config, f);
    l.mapPick = "/Game/Maps/Arenas/Map_Arena_Pit.Map_Arena_Pit";
    LobbyPlayer me; me.name = "Me"; me.me = true; me.host = host;
    l.players.push_back(me);
    if (!host) { LobbyPlayer h; h.name = "Host"; h.host = true; l.players.push_back(h); }
    return l;
}
static bool Listed(const LobbyScene& s, LobbyRow id) { for (const SettingRow& r : s.settings) if (r.id == id) return true; return false; }
static SettingRow Row(const LobbyScene& s, LobbyRow id) { for (const SettingRow& r : s.settings) if (r.id == id) return r; CHECK(false); return SettingRow{}; }   // a copy: the scene is often a temporary

int main() {
    // The formats: every round format but Tag, and only under per player sync.
    for (Format f : {Format::Duel, Format::LastManStanding, Format::TeamDuel, Format::Riot, Format::Juggernaut, Format::Tournament, Format::PitBridgeDuel}) {
        CHECK(WinByApplies(f, false));
        CHECK(!WinByApplies(f, true));
    }
    for (Format f : {Format::Tag, Format::FreeForAll, Format::TwoTeams, Format::PlayersVsBots, Format::Zombies, Format::Infection, Format::CrossbowTDM})
        CHECK(!WinByApplies(f, false));
    {
        MatchConfig c = SelectFormat(MatchConfig{}, Format::Duel);
        CHECK(WinByApplies(c));
        c.hostPhysics = true;
        CHECK(!WinByApplies(c));
    }

    // The rule a match runs: the pick where it applies, else Death; out of range reads Death.
    CHECK(EffectiveWinBy(Format::Duel, false, 1) == WinBy::FirstBlood);
    CHECK(EffectiveWinBy(Format::Duel, false, 2) == WinBy::Knockdown);
    CHECK(EffectiveWinBy(Format::Duel, false, 0) == WinBy::Death);
    CHECK(EffectiveWinBy(Format::Duel, false, 3) == WinBy::Death);
    CHECK(EffectiveWinBy(Format::Duel, false, 255) == WinBy::Death);
    CHECK(EffectiveWinBy(Format::Duel, true, 1) == WinBy::Death);        // host physics: the guard is off
    CHECK(EffectiveWinBy(Format::Tag, false, 2) == WinBy::Death);        // the fuse decides Tag
    CHECK(EffectiveWinBy(Format::FreeForAll, false, 1) == WinBy::Death);

    // The lose condition: Willie_BP's 1 (first blood) and 2 (knockdown); Death leaves the fighter's own.
    CHECK(WinByLoseCondition(WinBy::FirstBlood) == 1);
    CHECK(WinByLoseCondition(WinBy::Knockdown) == 2);
    CHECK(WinByLoseCondition(WinBy::Death) == -1);
    CHECK(WinByLoseCondition(WinBy(7)) == -1);

    // The stepper wraps both ways and a stale value starts from Death.
    CHECK(StepWinBy(0, 1) == WinBy::FirstBlood && StepWinBy(1, 1) == WinBy::Knockdown && StepWinBy(2, 1) == WinBy::Death);
    CHECK(StepWinBy(0, -1) == WinBy::Knockdown && StepWinBy(2, -1) == WinBy::FirstBlood && StepWinBy(1, -1) == WinBy::Death);
    CHECK(StepWinBy(9, 1) == WinBy::FirstBlood && StepWinBy(9, -1) == WinBy::Knockdown);
    CHECK(StepWinBy(10, 1) == WinBy::FirstBlood && StepWinBy(10, -1) == WinBy::Knockdown);
    {
        MatchConfig c = SelectFormat(MatchConfig{}, Format::Duel);
        c = StepSetting(c, LobbyRow::WinBy, 1);
        CHECK(c.winBy == 1);
        c = StepSetting(c, LobbyRow::WinBy, 1);
        CHECK(c.winBy == 2);
        c = StepSetting(c, LobbyRow::WinBy, 1);
        CHECK(c.winBy == 0);
        c = StepSetting(c, LobbyRow::WinBy, -1);
        CHECK(c.winBy == 2);
    }

    // Names and hints.
    CHECK(std::strcmp(WinByName(WinBy::Death), "Death") == 0);
    CHECK(std::strcmp(WinByName(WinBy::FirstBlood), "First blood") == 0);
    CHECK(std::strcmp(WinByName(WinBy::Knockdown), "Knockdown") == 0);
    CHECK(std::string(WinByHint(WinBy::Knockdown)).find("1.5 s") != std::string::npos);
    CHECK(std::string(WinByHint(WinBy::FirstBlood)).find("bleed") != std::string::npos);
    CHECK(std::string(WinByHint(WinBy::Death)).find("die") != std::string::npos);

    // Sanitize keeps the three values and drops anything else.
    for (uint8_t v : {uint8_t(0), uint8_t(1), uint8_t(2)}) {
        MatchConfig c = SelectFormat(MatchConfig{}, Format::Duel);
        c.winBy = v;
        CHECK(Sanitize(c).winBy == v);
    }
    for (uint8_t v : {uint8_t(3), uint8_t(4), uint8_t(200), uint8_t(255)}) {
        MatchConfig c = SelectFormat(MatchConfig{}, Format::Duel);
        c.winBy = v;
        CHECK(Sanitize(c).winBy == 0);
    }
    CHECK(MatchConfig{}.winBy == 0);                                   // Death by default: lobbies run as before

    // The row: in the round formats but Tag, in MATCH.
    CHECK(SectionOf(LobbyRow::WinBy) == LobbySection::Match);
    CHECK(size_t(LobbyRow::WinBy) < size_t(LobbyRow::Sync));
    {
        LobbyLocal l = Lobby(Format::Duel);
        l.config.winBy = 2;
        LobbyScene s = BuildLobbyScene(l);
        CHECK(Listed(s, LobbyRow::WinBy));
        CHECK(Row(s, LobbyRow::WinBy).id == LobbyRow::WinBy && Row(s, LobbyRow::WinBy).label == "Win by" && Row(s, LobbyRow::WinBy).value == "Knockdown" && Row(s, LobbyRow::WinBy).enabled);
        CHECK(Row(s, LobbyRow::WinBy).hint == WinByHint(WinBy::Knockdown));
        LobbyLocal d = Lobby(Format::Duel);
        CHECK(BuildLobbyScene(d).settings.size() == s.settings.size());   // the pick changes no other row
        CHECK(Row(BuildLobbyScene(d), LobbyRow::WinBy).value == "Death");
    }
    {
        LobbyLocal l = Lobby(Format::Duel, false);                     // a member reads it, cannot step it
        l.config.winBy = 1;
        LobbyScene s = BuildLobbyScene(l);
        CHECK(Listed(s, LobbyRow::WinBy) && Row(s, LobbyRow::WinBy).value == "First blood" && !Row(s, LobbyRow::WinBy).enabled);
    }
    for (Format f : {Format::Tag, Format::FreeForAll, Format::PlayersVsBots}) {
        LobbyLocal l = Lobby(f);
        l.config.winBy = 1;
        LobbyScene s = BuildLobbyScene(l);
        CHECK(!Listed(s, LobbyRow::WinBy));
    }
    {
        LobbyLocal l = Lobby(Format::Duel);
        l.config.hostPhysics = true;
        l.config.winBy = 1;
        CHECK(!Listed(BuildLobbyScene(l), LobbyRow::WinBy));
        l = Lobby(Format::Duel);
        l.progression = true;
        CHECK(!Listed(BuildLobbyScene(l), LobbyRow::WinBy));
    }

    if (g_fail) { std::printf("%d failure(s)\n", g_fail); return 1; }
    std::printf("hsm_win_by: ok\n");
    return 0;
}
