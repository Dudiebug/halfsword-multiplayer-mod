// Unit tests for hsm_bench_pawn.h (a benched client's own pawn goes down; public #4257). Run via
// implementation/shared/run_tests.sh bench_pawn.
#include "match/hsm_bench_pawn.h"
#include "match/hsm_bracket.h"
#include <cstdio>
#include <string>
#include <vector>

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++g_fail; } } while (0)
using namespace hsm;

static MatchInfo Info(Format f, MatchPhase ph) { MatchInfo m; m.format = uint8_t(f); m.phase = uint8_t(ph); return m; }

// #4257: a third player joins a fought Duel (Map_Arena_Pit). The host benches it (down, no body); its client stood in the level's
// stock pawn with live input, because the bench check only knew Tournament and Pit Bridge Duels.
static void TestDuelBench() {
    const MatchInfo live = Info(Format::Duel, MatchPhase::Live);
    CHECK(BenchedPawnGoesDown(live, 3, true, true));                                  // the report: down, stock pawn
    CHECK(BenchedPawnGoesDown(Info(Format::Duel, MatchPhase::SuddenDeath), 3, true, true));
    CHECK(!BenchedPawnGoesDown(live, 3, true, false));    // a fighter the host respawned, its MatchInfo still has it down: keep it
    CHECK(!BenchedPawnGoesDown(live, 3, false, true));    // a fighter whose body is on its way (host counts it alive)
    CHECK(!BenchedPawnGoesDown(live, 0, true, true));     // no entity yet
    for (MatchPhase ph : {MatchPhase::Idle, MatchPhase::Countdown, MatchPhase::Results})   // the bench is only made while it is fought
        CHECK(!BenchedPawnGoesDown(Info(Format::Duel, ph), 3, true, true));
}

// Other round formats have no bench: a down fighter with the stock pawn is left alone, as before.
static void TestNoBench() {
    for (Format f : {Format::LastManStanding, Format::FreeForAll, Format::TwoTeams, Format::Chaos, Format::Infection, Format::TeamDuel})
        CHECK(!BenchedPawnGoesDown(Info(f, MatchPhase::Live), 3, true, true));
}

// Tournament and Pit Bridge Duels, unchanged: from the countdown on, everybody down and not in the pairing; the body does not matter.
static void TestQueueBench() {
    MatchInfo pit = Info(Format::PitBridgeDuel, MatchPhase::Live);
    pit.queueOrder = {5, 6, 7};
    CHECK(BenchedPawnGoesDown(pit, 7, true, false) && BenchedPawnGoesDown(pit, 7, true, true));
    CHECK(BenchedPawnGoesDown(pit, 1, true, false));      // a joiner the queue has not listed yet waits too
    CHECK(!BenchedPawnGoesDown(pit, 5, true, true));      // in the pairing, down between two rounds
    CHECK(!BenchedPawnGoesDown(pit, 7, false, true));     // the host counts it alive
    pit.phase = uint8_t(MatchPhase::Countdown);
    CHECK(BenchedPawnGoesDown(pit, 7, true, false));
    pit.phase = uint8_t(MatchPhase::Results);
    CHECK(!BenchedPawnGoesDown(pit, 7, true, false));
    pit.phase = uint8_t(MatchPhase::Idle);
    CHECK(!BenchedPawnGoesDown(pit, 7, true, false));

    MatchInfo t = Info(Format::Tournament, MatchPhase::Live);
    Bracket b; b.Start(std::vector<uint8_t>{1, 2, 3, 4}, 4);
    t.queueOrder = b.seeds(); t.bracketWinners = b.GameWinners();
    Bracket::Game g; CHECK(b.Current(g));
    uint8_t waiting = 0;
    for (uint8_t e = 1; e <= 4; ++e) if (e != g.a && e != g.b) waiting = e;
    CHECK(waiting != 0);
    CHECK(!BenchedPawnGoesDown(t, g.a, true, false) && !BenchedPawnGoesDown(t, g.b, true, false));
    CHECK(BenchedPawnGoesDown(t, waiting, true, false));
    CHECK(BenchedPawnGoesDown(t, 9, true, true));         // a late joiner, not in this bracket
}

static void TestNames() {
    CHECK(std::string(BenchName(Format::Tournament)) == "tournament");
    CHECK(std::string(BenchName(Format::PitBridgeDuel)) == "pit bridge duels");
    CHECK(std::string(BenchName(Format::Duel)) == "duel: joined a fought Duel");
}

int main() {
    TestDuelBench(); TestNoBench(); TestQueueBench(); TestNames();
    if (g_fail) { std::printf("test_hsm_bench_pawn: %d failure(s)\n", g_fail); return 1; }
    std::printf("test_hsm_bench_pawn: ok\n");
    return 0;
}
