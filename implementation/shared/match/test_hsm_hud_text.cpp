// HUD text for the match (AC-1 support): status line per phase/format, the info line cap, the scoreboard words.
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
    CHECK(!Has(StatusLine(m, 1, true), "Tavern"));                              // a team match always has the rematch
    m.phase = uint8_t(MatchPhase::Idle); CHECK(StatusLine(m, 1).empty());
    MatchInfo p; p.format = uint8_t(Format::PlayersVsBots); p.phase = uint8_t(MatchPhase::Results); p.winner = kHumanTeam; p.timeLeftMs = 2100;
    CHECK(Has(StatusLine(p, 1), "rematch in 3") && !Has(StatusLine(p, 1), "Tavern"));
    s = StatusLine(p, 1, true); CHECK(Has(s, "FIGHT WON") && Has(s, "hold G to go on (or in 3)") && !Has(s, "rematch"));   // won: waits for G
    p.winner = kBotTeam;
    s = StatusLine(p, 1, true); CHECK(Has(s, "BOTS WIN") && Has(s, "back to the Tavern in 3") && !Has(s, "rematch"));      // lost: back by itself
    MatchInfo f; f.format = uint8_t(Format::FreeForAll); f.scoreLimit = 5; f.phase = uint8_t(MatchPhase::Live);
    f.scores = {{1, 2}, {2, 4}}; f.fighters = {{1, -1, 2, 0, "Host"}, {2, -1, 4, 1, "Ann"}};
    s = StatusLine(f, 1); CHECK(Has(s, "You 2") && Has(s, "Ann 4") && Has(s, "first to 5"));
    f.phase = uint8_t(MatchPhase::Results); f.winner = 2; CHECK(Has(StatusLine(f, 1), "ANN WINS"));
    f.winner = 1; CHECK(Has(StatusLine(f, 1), "YOU WIN"));
    MatchInfo b; b.format = uint8_t(Format::PlayersVsBots); b.scoreLimit = 20; b.phase = uint8_t(MatchPhase::Results); b.winner = kBotTeam;
    CHECK(Has(StatusLine(b, 1), "BOTS WIN"));
    b.phase = uint8_t(MatchPhase::Live); b.scores = {{0, 7}}; CHECK(Has(StatusLine(b, 1), "Wave 8") && Has(StatusLine(b, 1), "7 cleared"));
}

// Four teams: the status line scores every team the match has; Last Man Standing reads like Duel with more fighters.
static void TestFourTeamsAndLms() {
    MatchInfo m = Teams(); m.teams = 4; m.scores = {{0, 3}, {1, 5}, {2, 1}, {3, 8}};
    m.fighters.push_back({4, 3, 8, 0, "Di"});
    std::string s = StatusLine(m, 1);
    CHECK(Has(s, "RED 3") && Has(s, "BLUE 5") && Has(s, "GREEN 1") && Has(s, "GOLD 8"));
    m.teams = 3; s = StatusLine(m, 1); CHECK(Has(s, "GREEN 1") && !Has(s, "GOLD"));
    m.teams = 2; s = StatusLine(m, 1); CHECK(Has(s, "BLUE 5") && !Has(s, "GREEN"));
    m.teams = 4; m.phase = uint8_t(MatchPhase::Results); m.winner = 3; m.timeLeftMs = 9100;
    CHECK(Has(StatusLine(m, 1), "GOLD WINS"));
    MatchInfo old = Teams(); old.format = uint8_t(Format::ThreeTeams); old.scores = {{0, 1}, {2, 4}};
    CHECK(Has(StatusLine(old, 1), "GREEN 4"));                                     // the old value is three teams

    MatchInfo l; l.format = uint8_t(Format::LastManStanding); l.scoreLimit = 3; l.phase = uint8_t(MatchPhase::Live); l.round = 2;
    l.scores = {{1, 1}, {3, 2}}; l.fighters = {{1, -1, 0, 0, "Host"}, {2, -1, 0, 0, "Ann"}, {3, -1, 0, 0, "Bo"}};
    s = StatusLine(l, 1);
    CHECK(Has(s, "Rounds") && Has(s, "You 1") && Has(s, "Bo 2") && Has(s, "first to 3") && !Has(s, "best of"));
    MatchInfo d = l; d.format = uint8_t(Format::Duel);
    CHECK(Has(StatusLine(d, 1), "best of 5"));
    l.roundGapLeftMs = 2500; l.roundWinner = 3;                                     // the gap names who took the round
    s = StatusLine(l, 1); CHECK(Has(s, "Round 2 to Bo") && Has(s, "next round in 3"));
    CHECK(InRoundGap(l));
    l.roundGapLeftMs = 0; l.phase = uint8_t(MatchPhase::Results); l.winner = 3; l.timeLeftMs = 9100;
    CHECK(Has(StatusLine(l, 1), "BO WINS") && Has(StatusLine(l, 3), "YOU WIN"));
}

// A drawn Duel round: the words that follow the names in the kill feed (MakeFeedRow, hsm_hud_scene.h).
static void TestDrawnWhy() {
    CHECK(DrawnWhy({3, 2, 16}) == " drew the round (both down)" && DrawnWhy({3, 2, 16 | 8}) == " drew the round (both down)");
    CHECK(DrawnWhy({3, 2, 32}) == " drew the round (no hits)" && DrawnWhy({0, 2, 32 | 16}) == " drew the round (no hits)");
}

// The info line under the pill: 80 characters at most. Whole parts (set apart by three spaces) go from the end; the first
// part alone, if too long, is cut with "...".
static void TestFitInfo() {
    CHECK(kInfoMaxChars == 80);
    CHECK(FitInfo("").empty() && FitInfo("Damage 150%   Dead weight 20%") == "Damage 150%   Dead weight 20%");
    const std::string q = "Quarter-final 2/4: Annabelle vs Bartholomew - first to 2 (1-0)";                  // 62 characters
    CHECK(q.size() == 62 && FitInfo(q + "   Next: Cy vs Dix") == q + "   Next: Cy vs Dix");                 // exactly 80: whole
    CHECK(FitInfo(q + "   Next: Cy vs Dixx") == q);                                                             // 81: what is next goes
    CHECK(FitInfo(q + "   Next: Cyrano vs Dionysus") == q);
    CHECK(FitInfo(q + "   Next: Cy vs Dix   more") == q + "   Next: Cy vs Dix");                                   // a front of exactly 80 stays
    CHECK(FitInfo(std::string(76, 'a') + " " + std::string(10, 'b')) == std::string(76, 'a') + "...");        // no space before the dots
    const std::string exact = std::string(80, 'a');
    CHECK(FitInfo(exact) == exact && FitInfo(exact + "b") == std::string(77, 'a') + "...");                   // 81, one part: cut
    CHECK(Utf8Chars(FitInfo(std::string(200, 'a'))) == 80);
    const std::string three = "Ann: 3 in a row   Ann vs Bo   Up next: " + std::string(60, 'x');
    CHECK(FitInfo(three) == "Ann: 3 in a row   Ann vs Bo");                                                    // streak and pairing stay
    const std::string two = std::string(50, 'a') + "   " + std::string(26, 'b') + "   cc";
    CHECK(FitInfo(two) == std::string(50, 'a') + "   " + std::string(26, 'b'));                                // 84: the last part only
    CHECK(Utf8Chars(FitInfo(two)) == 79 && FitInfo(FitInfo(two)) == FitInfo(two));
    const std::string ue = "\xC3\xA9";                                                                          // two bytes, one character
    std::string multi; for (int i = 0; i < 90; ++i) multi += ue;
    const std::string cut = FitInfo(multi);
    CHECK(Utf8Chars(cut) == 80 && cut.substr(cut.size() - 3) == "..." && cut.size() == 77 * 2 + 3);            // never inside a character
    std::string fits; for (int i = 0; i < 80; ++i) fits += ue;
    CHECK(FitInfo(fits) == fits && fits.size() == 160);                                                        // 80 characters, 160 bytes
    CHECK(FitInfo("   a    b") == "   a    b");                                                              // short: untouched
    CHECK(FitInfo(std::string(100, ' ') + "x") == "x");                                                        // over 80 by gaps: the parts are kept
    CHECK(FitInfo("abcdefghij", 8) == "abcde..." && FitInfo("abc def", 8) == "abc def");
    CHECK(Utf8Chars("abc\xC3\xA9") == 4 && Utf8Chars("") == 0);
}

// The board's column heads and the legend under it say what the columns hold, in whole words.
static void TestBoardWords() {
    CHECK(std::string(kBoardHeadPlayer) == "PLAYER" && std::string(kBoardHeadKills) == "KILLS" && std::string(kBoardHeadDeaths) == "DEATHS");
    CHECK(std::string(kBoardHeadScore) == "SCORE" && std::string(kBoardHeadPing) == "PING");
    CHECK(std::string(kBoardLegendDown) == "DOWN: can still get up" && std::string(kBoardLegendDead) == "DEAD: out of the fight");
}

static void TestLobbyText() {
    CHECK(FormatName(Format::FreeForAll) == "Carnage" && FormatName(Format::PlayersVsBots) == "Survival");
    CHECK(FormatName(Format::Zombies) == "Zombies" && FormatName(Format::Duel) == "Duel");
    CHECK(FormatName(Format::LastManStanding) == "Last man standing" && FormatName(Format::TwoTeams) == "Teams" && FormatName(Format::ThreeTeams) == "Teams");
    {
        MatchInfo z; z.format = uint8_t(Format::Zombies); z.phase = uint8_t(MatchPhase::Results); z.winner = kBotTeam;
        CHECK(Has(StatusLine(z, 1), "OVERRUN"));
        z.phase = uint8_t(MatchPhase::Live); z.scores = {{0, 2}}; CHECK(Has(StatusLine(z, 1), "Wave 3"));
        MatchInfo d; d.format = uint8_t(Format::Duel); d.scoreLimit = 3; d.phase = uint8_t(MatchPhase::Live);
        d.fighters = {{1, -1, 0, 0, "Ann"}, {2, -1, 0, 0, "Bob"}}; d.scores = {{1, 2}, {2, 1}};
        CHECK(Has(StatusLine(d, 1), "Rounds") && Has(StatusLine(d, 1), "You 2") && Has(StatusLine(d, 1), "Bob 1") && Has(StatusLine(d, 1), "best of 5"));
        d.phase = uint8_t(MatchPhase::Results); d.winner = 1; CHECK(Has(StatusLine(d, 1), "YOU WIN"));
    }
    CHECK(FormatName(Format(kFormatCount)) == "Carnage");
    CHECK(NextOf({3, 5, 10}, 5) == 10 && NextOf({3, 5, 10}, 10) == 3 && NextOf({3, 5, 10}, 7) == 3);
    CHECK(NextOf({}, 4) == 4);
    CHECK(NextTeamPick(kNoTeam, 2) == 0 && NextTeamPick(0, 2) == 1 && NextTeamPick(1, 2) == kNoTeam);   // Auto -> Red -> Blue -> Auto
    CHECK(NextTeamPick(2, 3) == kNoTeam && NextTeamPick(5, 2) == kNoTeam && NextTeamPick(0, 0) == kNoTeam);
    CHECK(TeamLabel(kNoTeam) == "Auto" && TeamLabel(0) == "Red" && TeamLabel(2) == "Green");
    CHECK(TimeLabel(0) == "off" && TimeLabel(600) == "10 min");
}

// Rounds plan 1.2 / 3.2: Duel's gap names who took the round and counts the next one in; the gear label says when the gear
// changes.
static void TestRoundsText() {
    MatchInfo d; d.format = uint8_t(Format::Duel); d.scoreLimit = 3; d.phase = uint8_t(MatchPhase::Live);
    d.fighters = {{1, -1, 0, 0, "Bob"}, {2, -1, 1, 0, "Ann"}}; d.scores = {{1, 0}, {2, 1}};
    d.round = 2; d.roundWinner = 2; d.roundGapLeftMs = 2100;
    CHECK(StatusLine(d, 1) == "Round 2 to Ann   next round in 3");
    CHECK(StatusLine(d, 2) == "Round 2 to you   next round in 3");
    d.roundWinner = 0; CHECK(StatusLine(d, 1) == "Round 2 drawn   next round in 3");
    d.roundWinner = 2; d.round = 4; d.roundGapLeftMs = 1;
    CHECK(StatusLine(d, 1) == "Round 4 to Ann   next round in 1");
    d.phase = uint8_t(MatchPhase::SuddenDeath); CHECK(StatusLine(d, 1) == "Round 4 to Ann   next round in 1");
    d.phase = uint8_t(MatchPhase::Live); d.roundGapLeftMs = 0;                  // the round is being fought: the score line
    CHECK(Has(StatusLine(d, 1), "Rounds") && Has(StatusLine(d, 1), "You 0") && Has(StatusLine(d, 1), "Ann 1") && !Has(StatusLine(d, 1), "next round"));
    d.roundGapLeftMs = 4000; d.phase = uint8_t(MatchPhase::Results); d.winner = 2; d.timeLeftMs = 9000;
    CHECK(Has(StatusLine(d, 1), "ANN WINS") && !Has(StatusLine(d, 1), "next round"));   // results, not a gap
    MatchInfo f; f.format = uint8_t(Format::FreeForAll); f.scoreLimit = 5; f.phase = uint8_t(MatchPhase::Live);
    f.scores = {{1, 2}, {2, 4}}; f.fighters = {{1, -1, 2, 0, "Host"}, {2, -1, 4, 1, "Ann"}}; f.roundGapLeftMs = 3000; f.round = 2;
    CHECK(!Has(StatusLine(f, 1), "next round") && Has(StatusLine(f, 1), "You 2"));   // rounds are Duel's


    CHECK(GearLabel(Gear::Own, Format::Duel) == "Own kits" && GearLabel(Gear::Own, Format::FreeForAll) == "Own kits");
    CHECK(GearLabel(Gear::Mirror, Format::Duel) == "Everyone identical, new each round");
    for (Format f : {Format::Duel, Format::FreeForAll, Format::TwoTeams, Format::Zombies}) CHECK(GearLabel(Gear::HostKit, f) == "The host's kit");   // #777
    CHECK(GearLabel(Gear::HostKit, Format::Riot) == "The host's kit");            // allowed anywhere; Sanitize is what makes Riot roll by role
    CHECK(GearLabel(Gear::Mirror, Format::FreeForAll) == "Everyone identical" && GearLabel(Gear::Mirror, Format::TwoTeams) == "Everyone identical");
    CHECK(GearLabel(Gear::RandomTier, Format::Duel) == "Random by tier, new each round");
    CHECK(GearLabel(Gear::RandomTier, Format::FreeForAll) == "Random by tier, new each life");
    CHECK(GearLabel(Gear::RandomTier, Format::LastManStanding) == "Random by tier, new each round" && GearLabel(Gear::Mirror, Format::LastManStanding) == "Everyone identical, new each round");
    CHECK(GearLabel(Gear::RandomTier, Format::ThreeTeams) == "Random by tier, new each life");
    CHECK(GearLabel(Gear::Mirror, Format::Zombies) == "Everyone identical, kept all run" && GearLabel(Gear::RandomTier, Format::PlayersVsBots) == "Random by tier, kept all run");
    CHECK(GearLabel(Gear::Own, Format::Zombies) == "Own kits" && GearLabel(Gear::Own, Format::PlayersVsBots) == "Own kits");
    CHECK(GearLabel(Gear(9), Format::Duel) == "Own kits");                  // an unknown setting is Own
    CHECK(GearLabel(Gear::Fists, Format::FreeForAll) == "Bare fists" && GearLabel(Gear::Fists, Format::Zombies) == "Own kits");
    // Brawl is a free-for-all with bare fists: it has its own name wherever the mode is named.
    CHECK(ModeName(Format::FreeForAll, Gear::Fists) == "Brawl" && ModeName(uint8_t(Format::FreeForAll), uint8_t(Gear::Fists)) == "Brawl");
    CHECK(ModeName(Format::FreeForAll, Gear::Own) == "Carnage" && ModeName(Format::Duel, Gear::Fists) == "Duel");
    CHECK(ModeName(uint8_t(Format::TwoTeams), uint8_t(Gear::Mirror)) == "Teams" && ModeName(uint8_t(Format::TwoTeams), uint8_t(Gear::Mirror), 3) == "Teams (3)");
}

// MatchInfo comes once a second (and at every change); the clocks run on in between so countdowns move every frame.
static void TestExtrapolate() {
    MatchInfo m; m.format = uint8_t(Format::Duel); m.phase = uint8_t(MatchPhase::Live); m.timeLeftMs = 10000; m.roundGapLeftMs = 3000;
    m.round = 2; m.roundWinner = 1; m.fighters = {{1, -1, 0, 0, "Bob"}, {2, -1, 1, 0, "Ann"}};
    MatchInfo e = Extrapolate(m, 1000, 3500);
    CHECK(e.timeLeftMs == 7500 && e.roundGapLeftMs == 500);
    CHECK(e.round == 2 && e.roundWinner == 1 && e.phase == m.phase && e.fighters.size() == 2 && e.fighters[1].name == "Ann");
    e = Extrapolate(m, 1000, 5000); CHECK(e.timeLeftMs == 6000 && e.roundGapLeftMs == 0);      // runs out, never wraps
    e = Extrapolate(m, 1000, 1000); CHECK(e.timeLeftMs == 10000 && e.roundGapLeftMs == 3000);
    e = Extrapolate(m, 1000, 999); CHECK(e.timeLeftMs == 10000 && e.roundGapLeftMs == 3000);   // a clock behind: no time
    e = Extrapolate(m, 0xFFFFFF00u, 0x100u); CHECK(e.timeLeftMs == 9488 && e.roundGapLeftMs == 2488);   // tick count wrap
    e = Extrapolate(m, 0, 40000); CHECK(e.timeLeftMs == 0 && e.roundGapLeftMs == 0);
    m.timeLeftMs = 0; m.roundGapLeftMs = 0;
    e = Extrapolate(m, 0, 500); CHECK(e.timeLeftMs == 0 && e.roundGapLeftMs == 0);            // no clock stays no clock
    MatchInfo r; r.format = uint8_t(Format::FreeForAll); r.phase = uint8_t(MatchPhase::Results); r.winner = 2; r.timeLeftMs = 9100;
    r.fighters = {{1, -1, 2, 0, "Host"}, {2, -1, 4, 1, "Ann"}};
    CHECK(Has(StatusLine(Extrapolate(r, 0, 0), 1), "rematch in 10"));
    CHECK(Has(StatusLine(Extrapolate(r, 0, 700), 1), "rematch in 9"));
    CHECK(Has(StatusLine(Extrapolate(r, 0, 8200), 1), "rematch in 1"));
    MatchInfo d = m; d.roundGapLeftMs = 2100; d.fighters = {{1, -1, 0, 0, "Bob"}, {2, -1, 1, 0, "Ann"}};
    CHECK(StatusLine(Extrapolate(d, 50, 1150), 1) == "Round 2 to you   next round in 1");
    CHECK(!Has(StatusLine(Extrapolate(d, 50, 2150), 1), "next round"));                     // the gap is over
}

// Survival's loot break between waves. The bot formats carry its time left in roundGapLeftMs and how many players voted
// to skip it in roundWinner (Duel-only fields until now: an older client still decodes the message and ignores them).
static void TestWaveBreakText() {
    MatchInfo s; s.format = uint8_t(Format::PlayersVsBots); s.phase = uint8_t(MatchPhase::Live); s.scores = {{kHumanTeam, 2}};
    s.fighters = {{1, kHumanTeam, 0, 0, "Bob"}, {2, kHumanTeam, 0, 0, "Ann"}, {10, kBotTeam, 0, 0, "Bot"}};
    CHECK(!InWaveBreak(s) && Has(StatusLine(s, 1), "Wave 3"));                  // fighting
    s.roundGapLeftMs = 44100; s.roundWinner = 1;
    CHECK(InWaveBreak(s) && !InRoundGap(s));
    CHECK(StatusLine(s, 1) == "Wave 2 cleared   next wave in 45   hold G to skip (1/2 ready)");   // bots do not vote
    s.roundWinner = 0; CHECK(StatusLine(s, 2) == "Wave 2 cleared   next wave in 45   hold G to skip (0/2 ready)");
    s.format = uint8_t(Format::Zombies); s.roundGapLeftMs = 9000;
    CHECK(InWaveBreak(s) && Has(StatusLine(s, 1), "next wave in 9"));
    s.phase = uint8_t(MatchPhase::Results); CHECK(!InWaveBreak(s));
    MatchInfo d; d.format = uint8_t(Format::Duel); d.phase = uint8_t(MatchPhase::Live); d.roundGapLeftMs = 2000;
    CHECK(!InWaveBreak(d) && InRoundGap(d));                                    // Duel's gap is no wave break
}

// The player reads a tier by the game's own name for it (Enum_Ranks), never as "tier N".
static void TestTierName() {
    const char* const names[] = {"Beggar", "Peasant", "Commoner", "Militia", "Soldier", "Veteran", "Man-at-Arms", "Knight", "Lord"};
    for (int t = 0; t < 9; ++t) CHECK(std::string(TierName(t)) == names[t]);
    CHECK(std::string(TierName(-1)) == "Beggar" && std::string(TierName(-1000)) == "Beggar");   // below the first: the first
    CHECK(std::string(TierName(9)) == "Lord" && std::string(TierName(255)) == "Lord" && std::string(TierName(100000)) == "Lord");   // past the last: the last
    for (int t = 0; t < 8; ++t) CHECK(TierName(t) != TierName(t + 1));
    CHECK(std::string(TierName(kBrawlTier)) == "Veteran" && std::string(TierName(MatchConfig{}.tier)) == "Commoner");   // Brawl's fixed tier and the default
}

// Crypts (#3199): the gather pill reads the clock while looting, the kit kept after a death or an exit, and nothing off.
static void TestCryptsLine() {
    CHECK(CryptsLine(GatherPhase::Off, GatherSelf::Gathering, 60000).empty());
    CHECK(CryptsLine(GatherPhase::Gathering, GatherSelf::Gathering, 754000) == "CRYPTS 12:34 - gather your kit");
    CHECK(CryptsLine(GatherPhase::Gathering, GatherSelf::Opening, 600000) == "CRYPTS 10:00 - opening your Crypts");
    CHECK(CryptsLine(GatherPhase::Gathering, GatherSelf::Died, 250000) == "Kit kept, waiting 4:10");
    CHECK(CryptsLine(GatherPhase::Gathering, GatherSelf::Extracted, 5000) == "Kit kept, waiting 0:05");
    CHECK(CryptsLine(GatherPhase::Gathering, GatherSelf::Waiting, 30000) == "Fresh character, waiting 0:30");
    CHECK(CryptsLine(GatherPhase::Gathering, GatherSelf::None, 30000) == "Fresh character, waiting 0:30");
    for (GatherSelf s : {GatherSelf::None, GatherSelf::Opening, GatherSelf::Gathering, GatherSelf::Died, GatherSelf::Extracted, GatherSelf::Waiting})
        CHECK(CryptsLine(GatherPhase::Collecting, s, 9000) == "Battle starting");
    // Together: the shared crypt's lines; the rest as Apart, and nothing with no gather.
    CHECK(CryptsLine(GatherPhase::Gathering, GatherSelf::Gathering, 754000, true) == "CRYPTS 12:34 - loot together, then fight in what you carry");
    CHECK(CryptsLine(GatherPhase::Gathering, GatherSelf::Opening, 600000, true) == "CRYPTS 10:00 - into the Crypts together");
    CHECK(CryptsLine(GatherPhase::Gathering, GatherSelf::Waiting, 30000, true) == "Fresh character, waiting 0:30");
    CHECK(CryptsLine(GatherPhase::Collecting, GatherSelf::Gathering, 9000, true) == "Battle starting");
    CHECK(CryptsLine(GatherPhase::Off, GatherSelf::Gathering, 9000, true).empty());
}

int main() {
    TestLobbyText(); TestTierName(); TestCryptsLine(); TestRoundsText(); TestWaveBreakText(); TestExtrapolate();
    TestStatus(); TestFourTeamsAndLms(); TestDrawnWhy(); TestFitInfo(); TestBoardWords();
    std::printf(g_fail ? "%d FAILED\n" : "all hud text tests passed\n", g_fail);
    return g_fail ? 1 : 0;
}
