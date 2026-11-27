// Unit tests for hsm_bench.h (a guest's BACK TO LOBBY, protocol rev 49). Run via implementation/shared/run_tests.sh test_hsm_bench.
#include "session/hsm_bench.h"
#include "session/hsm_budget.h"
#include "match/hsm_match.h"
#include <cstdio>

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++g_fail; } } while (0)

using namespace hsm;

static void TestWire() {
    BackToLobbyMsg m; m.seq = 0x01020304;
    std::vector<uint8_t> b = Encode(m);
    MsgType t; CHECK(PeekType(b.data(), b.size(), t) && t == MsgType::BackToLobby && uint8_t(t) == 72);
    CHECK(b.size() == 4 + 4 && b[2] == kProtocolRevision && b[3] == 72);
    BackToLobbyMsg y; CHECK(Decode(b.data(), b.size(), y) && y.seq == 0x01020304);
    for (size_t cut = 0; cut < b.size(); ++cut) CHECK(!Decode(b.data(), cut, y));
    { auto x = b; x.push_back(0); CHECK(!Decode(x.data(), x.size(), y)); }                 // nothing after it
    { BackToLobbyMsg z; z.seq = 0; auto e = Encode(z); CHECK(!Decode(e.data(), e.size(), y)); }   // 0: no travel yet, nothing to leave
    { BackToLobbyMsg z; z.seq = 0xFFFFFFFFu; auto e = Encode(z); CHECK(Decode(e.data(), e.size(), y) && y.seq == 0xFFFFFFFFu); }
    { auto x = b; x[3] = uint8_t(MsgType::Gather); CHECK(!Decode(x.data(), x.size(), y)); }       // another type
    { BackToLobbyMsg keep; keep.seq = 7; auto x = b; x[3] = 0; CHECK(!Decode(x.data(), x.size(), keep) && keep.seq == 7); }   // a refusal leaves it as it was
    CHECK(BudgetClassOf(MsgType::BackToLobby) == BudgetClass::Control);
    CHECK(kProtocolRevision == 53 && kLastDecodedMsgType == 78 && kLastReservedMsgType == 78);
}

static void TestVerdict() {
    // host, match level, slot 3, no gather, no Progression, msg seq 5 = host seq 5, not benched yet
    auto v = [](bool host, bool match, uint8_t slot, bool gather, bool prog, uint32_t msgSeq, uint32_t hostSeq, bool already) {
        return BenchVerdictOf(host, match, slot, gather, prog, msgSeq, hostSeq, already);
    };
    CHECK(v(true, true, 3, false, false, 5, 5, false) == BenchVerdict::Bench);
    CHECK(v(false, true, 3, false, false, 5, 5, false) == BenchVerdict::NotHost);       // a client gets it from nobody
    CHECK(v(true, true, 0, false, false, 5, 5, false) == BenchVerdict::HostSlot);       // the host's own is a Travel
    CHECK(v(true, false, 3, false, false, 5, 5, false) == BenchVerdict::NoMatch);       // the host is in the menu: nothing to bench from
    CHECK(v(true, true, 3, true, false, 5, 5, false) == BenchVerdict::Gather);
    CHECK(v(true, true, 3, false, true, 5, 5, false) == BenchVerdict::Progression);
    CHECK(v(true, true, 3, false, false, 4, 5, false) == BenchVerdict::Stale);          // the host travelled since the click
    CHECK(v(true, true, 3, false, false, 6, 5, false) == BenchVerdict::Stale);
    CHECK(v(true, true, 3, false, false, 5, 5, true) == BenchVerdict::Duplicate);
    // The order: who may ask, then the host's own slot, the level, the run, the seq, the repeat.
    CHECK(v(false, false, 0, true, true, 1, 2, true) == BenchVerdict::NotHost);
    CHECK(v(true, false, 0, true, true, 1, 2, true) == BenchVerdict::HostSlot);
    CHECK(v(true, false, 3, true, true, 1, 2, true) == BenchVerdict::NoMatch);
    CHECK(v(true, true, 3, true, true, 1, 2, true) == BenchVerdict::Gather);
    CHECK(v(true, true, 3, false, true, 1, 2, true) == BenchVerdict::Progression);
    CHECK(v(true, true, 3, false, false, 1, 2, true) == BenchVerdict::Stale);
    for (int i = 0; i <= int(BenchVerdict::Stale); ++i) CHECK(BenchVerdictText(BenchVerdict(i))[0] != '\0');
}

static void TestBench() {
    SlotBench b;
    CHECK(!b.Any() && !b.Benched(3, 5) && b.Active(5).empty());
    CHECK(b.Bench(3, 5));                                        // benched in travel 5
    CHECK(b.Any() && b.Benched(3, 5));
    CHECK(!b.Bench(3, 5));                                       // again: nothing to do twice
    CHECK(!b.Benched(4, 5) && !b.Benched(3, 4));                 // another slot, another travel: not benched
    CHECK(b.Bench(4, 5));
    std::vector<uint8_t> act = b.Active(5);
    CHECK(act.size() == 2 && act[0] == 3 && act[1] == 4);
    CHECK(b.Active(6).empty());
    // The host travels (seq 6): the bench is over without anyone clearing it.
    CHECK(!b.Benched(3, 6) && !b.Benched(4, 6));
    std::vector<uint8_t> lifted = b.Lifted(6);
    CHECK(lifted.size() == 2 && lifted[0] == 3 && lifted[1] == 4);
    CHECK(!b.Any() && b.Lifted(6).empty());                      // once
    // Lifting keeps this travel's own.
    CHECK(b.Bench(3, 6) && b.Bench(4, 5));
    lifted = b.Lifted(6);
    CHECK(lifted.size() == 1 && lifted[0] == 4 && b.Benched(3, 6) && !b.Benched(4, 5));
    // Benched again in a later travel after an earlier bench: the seq moves, the entry stays one.
    CHECK(b.Bench(3, 7) && b.Benched(3, 7) && !b.Benched(3, 6) && b.Active(7).size() == 1);
    b.Clear();
    CHECK(!b.Any() && !b.Benched(3, 7));
}

static void TestLeaveFreesOnce() {
    SlotBench b;
    CHECK(b.Bench(3, 5));
    CHECK(b.Leave(3, 5));                                        // leaves while benched: the fall, body and leases were done at the bench
    CHECK(!b.Leave(3, 5));                                       // a second leave (a repeated Steam event) finds nothing to skip
    CHECK(!b.Benched(3, 5) && !b.Any());
    // A rejoin takes the slot again and spawns normally: not benched.
    CHECK(!b.Benched(3, 5));
    // Left after the host travelled: the guest was pulled into the next match, so the leave is an ordinary one.
    CHECK(b.Bench(3, 5));
    CHECK(!b.Leave(3, 6));
    CHECK(!b.Any());
    // A leave of a slot never benched.
    CHECK(!b.Leave(9, 5));
}

static void TestBodyAllowed() {
    SlotBench b;
    CHECK(BodyAllowed(b, 3, 5));                                 // nobody benched: bodies as ever
    b.Bench(3, 5);
    CHECK(!BodyAllowed(b, 3, 5));                                // benched in this travel: no body (SpawnDue, RespawnEntity, the start's wait)
    CHECK(BodyAllowed(b, 4, 5));                                 // another slot is not the bench's business
    CHECK(BodyAllowed(b, 3, 6));                                 // the host travelled: it spawns normally again
    b.Leave(3, 5);
    CHECK(BodyAllowed(b, 3, 5));
}

static MatchConfig Cfg(Format f, uint16_t limit, uint16_t respawnS = 3) {
    MatchConfig c; c.format = f; c.scoreLimit = limit; c.timeLimitS = 0; c.respawnDelayS = respawnS; return c;
}
// The host's sweep after every Match::Tick (gameplay/session.cpp HostMatchTick): the benched are left, once.
static void Sweep(Match& m, const SlotBench& b, uint32_t seq, uint32_t now) { for (uint8_t e : BenchedToLeave(b, seq, m)) m.Leave(e, now); }

static void TestRoundResolvesWithoutTheBenched() {
    // A Duel round of two: the guest goes back mid-round, the host takes the round (the match wants 2 rounds; then the match is its).
    { Match m; SlotBench b; m.Start(Cfg(Format::Duel, 2), {{1, kNoTeam, true}, {2, kNoTeam, true}}, 0);
      m.Tick(5000);
      b.Bench(2, 7); Sweep(m, b, 7, 6000);
      CHECK(m.Gone(2) && !m.Alive(2) && m.Alive(1) && m.Score(1) == 1 && !m.RoundInPlay());
      Sweep(m, b, 7, 6100); CHECK(m.Deaths(2) == 1 && m.Score(1) == 1);   // once: the second sweep finds it gone already
      CHECK(BenchedToLeave(b, 7, m).empty());
      m.Tick(9000);
      m.Tick(9100);
      CHECK(m.phase() == MatchPhase::Results && m.winner() == 1);
    }
    // Last man standing, three: one benched, the round is the other two's; the second benched too leaves the host the round.
    { Match m; SlotBench b; m.Start(Cfg(Format::LastManStanding, 3), {{1, kNoTeam, true}, {2, kNoTeam, true}, {3, kNoTeam, true}}, 0);
      m.Tick(5000);
      b.Bench(3, 7); Sweep(m, b, 7, 6000);
      CHECK(m.RoundInPlay() && m.Alive(1) && m.Alive(2) && !m.Alive(3));    // two still fight it out
      b.Bench(2, 7); Sweep(m, b, 7, 6500);
      CHECK(!m.RoundInPlay() && m.Score(1) == 1);                           // the round is the host's
    }
    // Every human benched or gone ends a survival match, as when they leave.
    { Match m; SlotBench b; m.Start(Cfg(Format::PlayersVsBots, 100), {{1, 0, true}, {2, 0, true}}, 0);
      m.Tick(5000);
      b.Bench(1, 7); Sweep(m, b, 7, 6000);
      CHECK(m.phase() == MatchPhase::Live);
      m.Leave(2, 6500);                                                     // the other one leaves for good
      CHECK(m.phase() == MatchPhase::Results && m.winner() == kBotTeam);
    }
    { Match m; SlotBench b; m.Start(Cfg(Format::PlayersVsBots, 100), {{1, 0, true}, {2, 0, true}}, 0);
      m.Tick(5000);
      b.Bench(1, 7); b.Bench(2, 7); Sweep(m, b, 7, 6000);
      CHECK(m.phase() == MatchPhase::Results && m.winner() == kBotTeam);    // both benched: nobody is left to fight
    }
    // A rematch clears the match's own note of its leavers; the bench is the host's, so the next sweep leaves the guest again.
    { Match m; SlotBench b; m.Start(Cfg(Format::FreeForAll, 5), {{1, kNoTeam, true}, {2, kNoTeam, true}, {3, kNoTeam, true}}, 0);
      m.Tick(5000);
      b.Bench(3, 7); Sweep(m, b, 7, 6000);
      CHECK(m.Gone(3));
      m.Start(Cfg(Format::FreeForAll, 5), {{1, kNoTeam, true}, {2, kNoTeam, true}, {3, kNoTeam, true}}, 20000);   // the rematch
      CHECK(!m.Gone(3) && BenchedToLeave(b, 7, m).size() == 1);             // still benched: to be left once the fight is live
      m.Tick(20100); Sweep(m, b, 7, 20100);
      CHECK(!m.Gone(3));                                                    // the countdown: Leave records nothing yet
      m.Tick(25100); Sweep(m, b, 7, 25100);
      CHECK(m.Gone(3) && !m.Alive(3));
      CHECK(BenchedToLeave(b, 8, m).empty());                               // the host travelled: nobody is benched any more
    }
}

static void TestBlock() {
    //                       host   lobby  crypts prog
    CHECK(BackToLobbyBlock(true, true, false, false) == BackBlock::None);
    CHECK(BackToLobbyBlock(false, true, false, false) == BackBlock::None);      // a guest in a plain match: it works
    CHECK(BackToLobbyBlock(true, true, true, false) == BackBlock::Crypts);
    CHECK(BackToLobbyBlock(false, true, true, false) == BackBlock::Crypts);     // the same rule for a guest
    CHECK(BackToLobbyBlock(true, true, false, true) == BackBlock::None);        // the host's Travel works in Progression co-op
    CHECK(BackToLobbyBlock(false, true, false, true) == BackBlock::Progression);
    CHECK(BackToLobbyBlock(true, false, false, false) == BackBlock::NoLobby);
    CHECK(BackToLobbyBlock(false, false, false, false) == BackBlock::NoLobby);
    CHECK(BackToLobbyBlock(false, false, true, true) == BackBlock::NoLobby);    // no lobby comes first, then the Crypts, then Progression
    CHECK(BackToLobbyBlock(false, true, true, true) == BackBlock::Crypts);
}

static void TestMigration() {
    const std::string fight = "/Game/Maps/Arena", menu = "/Game/Maps/Menu";
    CHECK(MigrationReopenLevel(true, false, fight, "") == fight);               // in the fight: that level
    CHECK(MigrationReopenLevel(true, false, fight, "/Game/Maps/Old") == fight); // an old bench never wins over the open level
    CHECK(MigrationReopenLevel(true, true, "/Game/Crypts", "/Game/Maps/Old").empty());   // never a Crypts run
    CHECK(MigrationReopenLevel(false, false, menu, "/Game/Maps/Old") == "/Game/Maps/Old");   // back in the lobby: the level it left
    CHECK(MigrationReopenLevel(false, false, menu, "").empty());                // an ordinary menu: nothing to reopen
}

int main() {
    TestWire();
    TestVerdict();
    TestBench();
    TestLeaveFreesOnce();
    TestBodyAllowed();
    TestRoundResolvesWithoutTheBenched();
    TestBlock();
    TestMigration();
    if (g_fail) { std::printf("test_hsm_bench: %d failure(s)\n", g_fail); return 1; }
    std::printf("test_hsm_bench: ok\n");
    return 0;
}
