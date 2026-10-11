// HUD text for the match (AC-1 support): status line per phase/format, kill feed line, scoreboard.
#include "match/hsm_hud_text.h"
#include <cstdio>

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++g_fail; } } while (0)
using namespace hsm;

static bool Has(const std::string& s, const char* part) { return s.find(part) != std::string::npos; }

static MatchInfo Teams() {
    MatchInfo m; m.format = uint8_t(Format::TwoTeams); m.scoreLimit = 10; m.phase = uint8_t(MatchPhase::Live); m.timeLeftMs = 151000;
    m.scores = {{0, 3}, {1, 5}};
    m.fighters = {{1, 0, 3, 1, "Host"}, {2, 1, 5, 0, "Ann"}, {3, 0, 0, 4, "Bo"}};
    return m;
}

static void TestStatus() {
    MatchInfo m = Teams();
    std::string s = StatusLine(m, 1);
    CHECK(Has(s, "RED 3") && Has(s, "BLUE 5") && Has(s, "first to 10") && Has(s, "2:31"));
    m.timeLeftMs = 0; CHECK(!Has(StatusLine(m, 1), ":"));                       // no time limit: no clock
    m.phase = uint8_t(MatchPhase::Countdown); m.timeLeftMs = 2400;
    CHECK(Has(StatusLine(m, 1), "starts in 3"));
    m.phase = uint8_t(MatchPhase::SuddenDeath); CHECK(Has(StatusLine(m, 1), "SUDDEN DEATH"));
    m.phase = uint8_t(MatchPhase::Results); m.winner = 1; m.timeLeftMs = 9100;
    s = StatusLine(m, 1); CHECK(Has(s, "BLUE WINS") && Has(s, "rematch in 10"));
    m.phase = uint8_t(MatchPhase::Idle); CHECK(StatusLine(m, 1).empty());
    MatchInfo f; f.format = uint8_t(Format::FreeForAll); f.scoreLimit = 5; f.phase = uint8_t(MatchPhase::Live);
    f.scores = {{1, 2}, {2, 4}}; f.fighters = {{1, -1, 2, 0, "Host"}, {2, -1, 4, 1, "Ann"}};
    s = StatusLine(f, 1); CHECK(Has(s, "You 2") && Has(s, "Ann 4") && Has(s, "first to 5"));
    f.phase = uint8_t(MatchPhase::Results); f.winner = 2; CHECK(Has(StatusLine(f, 1), "ANN WINS"));
    f.winner = 1; CHECK(Has(StatusLine(f, 1), "YOU WIN"));
    MatchInfo b; b.format = uint8_t(Format::PlayersVsBots); b.scoreLimit = 20; b.phase = uint8_t(MatchPhase::Results); b.winner = kBotTeam;
    CHECK(Has(StatusLine(b, 1), "BOTS WIN"));
    b.phase = uint8_t(MatchPhase::Live); b.scores = {{0, 7}}; CHECK(Has(StatusLine(b, 1), "Wave 8") && Has(StatusLine(b, 1), "7 cleared"));
}

static void TestKillLine() {
    MatchInfo m = Teams();
    CHECK(KillLine(m, {1, 2, 0}) == "Host killed Ann");
    CHECK(KillLine(m, {1, 3, 1}) == "Host killed teammate Bo");
    CHECK(KillLine(m, {3, 3, 2}) == "Bo killed themselves");
    CHECK(KillLine(m, {0, 2, 4}) == "Ann died");
    CHECK(KillLine(m, {9, 2, 0}) == "#9 killed Ann");                           // unknown entity: number
}

static void TestScoreboard() {
    std::string s = Scoreboard(Teams());
    CHECK(Has(s, "Host") && Has(s, "Ann") && Has(s, "Bo") && Has(s, "RED") && Has(s, "BLUE"));
    CHECK(s.find("Ann") < s.find("Host"));                                       // sorted by kills
    CHECK(Has(s, "5 / 0") && Has(s, "0 / 4"));
}

static void TestLobbyText() {
    CHECK(FormatName(Format::FreeForAll) == "Free-for-all" && FormatName(Format::PlayersVsBots) == "Survival");
    CHECK(FormatName(Format::Zombies) == "Zombies" && FormatName(Format::Duel) == "Duel");
    {
        MatchInfo z; z.format = uint8_t(Format::Zombies); z.phase = uint8_t(MatchPhase::Results); z.winner = kBotTeam;
        CHECK(Has(StatusLine(z, 1), "OVERRUN"));
        z.phase = uint8_t(MatchPhase::Live); z.scores = {{0, 2}}; CHECK(Has(StatusLine(z, 1), "Wave 3"));
        MatchInfo d; d.format = uint8_t(Format::Duel); d.scoreLimit = 3; d.phase = uint8_t(MatchPhase::Live);
        d.fighters = {{1, -1, 0, 0, "Ann"}, {2, -1, 0, 0, "Bob"}}; d.scores = {{1, 2}, {2, 1}};
        CHECK(Has(StatusLine(d, 1), "Rounds") && Has(StatusLine(d, 1), "You 2") && Has(StatusLine(d, 1), "Bob 1") && Has(StatusLine(d, 1), "best of 5"));
        d.phase = uint8_t(MatchPhase::Results); d.winner = 1; CHECK(Has(StatusLine(d, 1), "YOU WIN"));
    }
    CHECK(FormatName(Format(9)) == "Free-for-all");
    CHECK(NextOf({3, 5, 10}, 5) == 10 && NextOf({3, 5, 10}, 10) == 3 && NextOf({3, 5, 10}, 7) == 3);
    CHECK(NextOf({}, 4) == 4);
    CHECK(NextTeamPick(kNoTeam, 2) == 0 && NextTeamPick(0, 2) == 1 && NextTeamPick(1, 2) == kNoTeam);   // Auto -> Red -> Blue -> Auto
    CHECK(NextTeamPick(2, 3) == kNoTeam && NextTeamPick(5, 2) == kNoTeam && NextTeamPick(0, 0) == kNoTeam);
    CHECK(TeamLabel(kNoTeam) == "Auto" && TeamLabel(0) == "Red" && TeamLabel(2) == "Green");
    CHECK(TimeLabel(0) == "off" && TimeLabel(600) == "10 min");
}

int main() {
    TestLobbyText();
    TestStatus(); TestKillLine(); TestScoreboard();
    std::printf(g_fail ? "%d FAILED\n" : "all hud text tests passed\n", g_fail);
    return g_fail ? 1 : 0;
}
