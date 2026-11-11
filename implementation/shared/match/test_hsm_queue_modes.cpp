// Tournament and Pit Bridge Duels in the match: the pair that fights, the bench, the gap between pairings, forfeits, late
// joiners, draws, the limits and the rematch. The engines themselves are tested in test_hsm_bracket / test_hsm_kingqueue.
#include "match/hsm_match.h"
#include <cstdio>
#include <set>

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++g_fail; } } while (0)
using namespace hsm;

constexpr uint32_t kGapMs = 3000;

static MatchConfig Cfg(Format f, uint16_t limit, uint16_t timeS = 0) {
    MatchConfig c;
    c.format = f; c.scoreLimit = limit; c.timeLimitS = timeS; c.respawnDelayS = 3;
    return c;
}
struct Rig {
    Match m;
    uint32_t now = 1000;
    explicit Rig(const MatchConfig& c, int players, uint32_t seed = 5) {
        std::vector<FighterInfo> f;
        for (int e = 1; e <= players; ++e) f.push_back({uint8_t(e), kNoTeam, true, uint64_t(100 + e)});
        m.SetBracketSeed(seed);
        m.Start(c, f, now);
        Live();
    }
    Match::Events Tick(uint32_t add = 0) { now += add; return m.Tick(now); }
    void Live() { Tick(Match::kCountdownMs); }
    // The fighter dies: the other of the pair takes the round.
    void Fall(uint8_t e) { m.OnDeath(e, 0, now); }
    // Past the gap: the round reset (or the rematch) and its events.
    Match::Events Gap() { return Tick(kGapMs); }
    uint8_t Other(uint8_t e) const { return e == m.PairA() ? m.PairB() : m.PairA(); }
    // The pair fights a round: the first of the pair wins it.
    void Win(uint8_t winner) { Fall(Other(winner)); }
};
static std::set<uint8_t> Set(const std::vector<uint8_t>& v) { return {v.begin(), v.end()}; }

static void TestTournamentStart() {
    Rig r(Cfg(Format::Tournament, 2), 4);
    CHECK(r.m.phase() == MatchPhase::Live);
    const uint8_t a = r.m.PairA(), b = r.m.PairB();
    CHECK(a != 0 && b != 0 && a != b);
    int fighting = 0, benched = 0;
    for (uint8_t e = 1; e <= 4; ++e) {
        if (e == a || e == b) { ++fighting; CHECK(r.m.Alive(e) && !r.m.Benched(e) && !r.m.AwaitsRespawn(e)); }
        else { ++benched; CHECK(!r.m.Alive(e) && r.m.Benched(e) && r.m.AwaitsRespawn(e)); }   // no body, spectating
    }
    CHECK(fighting == 2 && benched == 2);
    CHECK(r.m.Benched(1));    // #636: the host's own fighter (entity 1) sits out a pairing too (seed 5: 2 vs 4): its body must go as well
    CHECK(r.m.bracket().valid() && r.m.bracket().seeds().size() == 4);
    CHECK(r.m.config().timeLimitS == 0 && Sanitize(Cfg(Format::Tournament, 9, 600)).timeLimitS == 0 && Sanitize(Cfg(Format::Tournament, 9)).scoreLimit == 1);   // one round a fight (0.8.0-pre26)
    CHECK(r.m.round() == 1);
}

static void TestTournamentOneRoundThenNextPairing() {
    Rig r(Cfg(Format::Tournament, 2), 4);                // an old best-of-3 setting: Sanitize makes it one round a fight
    CHECK(r.m.config().scoreLimit == 1);
    const uint8_t a = r.m.PairA(), b = r.m.PairB();
    r.Win(a);                                            // one round won: a takes the fight
    CHECK(r.m.roundWinner() == a && r.m.Score(a) == 1 && r.m.Score(b) == 0 && r.m.phase() == MatchPhase::Live);
    CHECK(r.m.PairingWins(a) == 0);                      // the fight is over: nothing shown for the next
    Match::Events ev = r.Gap();
    // the next pairing: the other two; a and b go to the bench (a is alive, so its body is taken away)
    CHECK(ev.roundReset && Set(ev.bench) == std::set<uint8_t>({a, b}));
    CHECK(r.m.PairA() != a && r.m.PairA() != b && r.m.PairB() != a && r.m.PairB() != b);
    CHECK(Set(ev.respawn) == std::set<uint8_t>({r.m.PairA(), r.m.PairB()}));
    CHECK(r.m.Benched(a) && r.m.Benched(b) && !r.m.Alive(a) && !r.m.Benched(r.m.PairA()) && r.m.Alive(r.m.PairA()));
    CHECK(r.m.PairingWins(r.m.PairA()) == 0);
}

static void TestTournamentFinalWinsTheMatch() {
    Rig r(Cfg(Format::Tournament, 1), 4);                // one round a fight
    std::set<uint8_t> champions;
    int fights = 0;
    while (r.m.phase() == MatchPhase::Live && fights < 10) {
        const uint8_t w = r.m.PairA();
        r.Win(w);
        ++fights;
        if (r.m.phase() == MatchPhase::Live) r.Gap();
    }
    CHECK(fights == 3);                                  // 4 players: two semi-finals and the final
    CHECK(r.m.phase() == MatchPhase::Results && r.m.winner() != Match::kNoWinner);
    CHECK(r.m.bracket().Done() && r.m.winner() == int(r.m.bracket().Champion()));
    CHECK(r.m.Score(r.m.winner()) == 2);                 // the champion won two fights
    Match::Events ev = r.Tick();
    CHECK(ev.ended);
}

static void TestTournamentThreeHaveAByeForTheTopSeed() {
    Rig r(Cfg(Format::Tournament, 1), 3);
    const uint8_t seed1 = r.m.bracket().seeds()[0];
    CHECK(r.m.PairA() != seed1 && r.m.PairB() != seed1 && r.m.Benched(seed1));   // seed 1 waits for the winner of the others
    r.Win(r.m.PairA());
    Match::Events ev = r.Gap();
    CHECK(r.m.PairA() == seed1 || r.m.PairB() == seed1);
    CHECK(ev.bench.size() == 1);                         // the loser sits out; the winner stays in the pairing
    CHECK(r.m.Alive(seed1) && !r.m.Benched(seed1));
}

static void TestTournamentDrawKeepsThePairing() {
    Rig r(Cfg(Format::Tournament, 2), 4);
    const uint8_t a = r.m.PairA(), b = r.m.PairB();
    CHECK(r.m.RoundInPlay() && r.m.Draw(r.now) && !r.m.RoundInPlay());   // the host's check: both down, nobody can finish it
    CHECK(r.m.roundWinner() == 0 && r.m.PairingWins(a) == 0 && r.m.PairingWins(b) == 0 && r.m.Score(a) == 0 && r.m.Score(b) == 0);
    Match::Events ev = r.Gap();
    CHECK(r.m.PairA() == a && r.m.PairB() == b && Set(ev.respawn) == std::set<uint8_t>({a, b}) && ev.bench.empty());
    r.Win(a);                                            // a drawn round gave nobody a point; the next win takes the fight
    CHECK(r.m.Score(a) == 1 && r.m.Score(b) == 0 && r.m.roundWinner() == a);
}

static void TestTournamentForfeitDuringAPairing() {
    Rig r(Cfg(Format::Tournament, 2), 4);
    const uint8_t a = r.m.PairA(), b = r.m.PairB();
    CHECK(r.m.Leave(a, r.now));                          // a leaves mid-round: b takes the fight, not just the round
    CHECK(r.m.Score(b) == 1 && !r.m.bracket().InRunning(a) && r.m.bracket().InRunning(b));
    CHECK(r.m.roundWinner() == b && r.m.RoundGapLeftMs(r.now) > 0);
    Match::Events ev = r.Gap();
    CHECK(Set(ev.bench) == std::set<uint8_t>({a, b}));   // b waits for the other semi-final; a's body goes with him
    CHECK(r.m.PairA() != a && r.m.PairB() != a && r.m.PairA() != b && r.m.PairB() != b);
    r.m.RemoveFighter(a);                                // the reservation ran out
    CHECK(!r.m.Has(a) && r.m.Score(b) == 1);
}

static void TestTournamentForfeitAfterTheRoundWasWon() {
    Rig r(Cfg(Format::Tournament, 2), 4);
    const uint8_t a = r.m.PairA(), b = r.m.PairB();
    r.Win(b);                                            // b is up a round, a is down
    CHECK(r.m.Leave(a, r.now) == false);                 // already down
    CHECK(r.m.Score(b) == 1);                            // the fight goes to b at once
    Match::Events ev = r.Gap();
    CHECK(r.m.PairA() != a && r.m.PairB() != a);
    CHECK(ev.roundReset);
}

// One round a fight: a pairing still open after a drawn round goes to the one who stays when the other leaves.
static void TestTournamentForfeitAfterADraw() {
    Rig r(Cfg(Format::Tournament, 1), 4);
    const uint8_t a = r.m.PairA(), b = r.m.PairB();
    CHECK(r.m.Draw(r.now) && r.m.roundWinner() == 0);   // both down: nobody took the fight
    r.m.Leave(a, r.now);                                 // a leaves in the gap: a forfeit
    CHECK(r.m.Score(b) == 1 && r.m.Score(a) == 0 && !r.m.bracket().InRunning(a) && r.m.bracket().InRunning(b));
    r.Gap();
    CHECK(r.m.PairA() != a && r.m.PairB() != a && r.m.PairA() != b && r.m.PairB() != b);
}

static void TestTournamentWaitingPlayerLeavesIsABye() {
    Rig r(Cfg(Format::Tournament, 1), 4);
    const uint8_t a = r.m.PairA(), b = r.m.PairB();
    uint8_t waiting1 = 0, waiting2 = 0;
    for (uint8_t e = 1; e <= 4; ++e) if (e != a && e != b) (waiting1 ? waiting2 : waiting1) = e;
    r.Fall(waiting1);                                    // a benched fighter's "death" is ignored
    CHECK(r.m.phase() == MatchPhase::Live && r.m.roundWinner() == 0 && r.m.RoundInPlay());
    CHECK(!r.m.Leave(waiting1, r.now));                  // not alive: nothing falls
    CHECK(r.m.Vacant(waiting1) && r.m.bracket().InRunning(waiting1));   // #1429: his place is open until his fight is due
    r.Win(a);
    CHECK(!r.m.Vacant(waiting1) && !r.m.bracket().InRunning(waiting1)); // due now, nobody took it: a bye
    r.Gap();
    // the other semi-final's place is a bye now: a meets waiting2 in the final
    CHECK((r.m.PairA() == a && r.m.PairB() == waiting2) || (r.m.PairA() == waiting2 && r.m.PairB() == a));
    CHECK(b != 0);
}

// #1429: the place of a waiting player who left goes to a newcomer who joins before that fight is due.
static bool InCurrent(const Match& m, uint8_t e) { Bracket::Game g; return m.bracket().Current(g) && (g.a == e || g.b == e); }
static void Waiting(const Rig& r, uint8_t& w1, uint8_t& w2) {
    w1 = w2 = 0;
    for (uint8_t e = 1; e <= 4; ++e) if (e != r.m.PairA() && e != r.m.PairB()) (w1 ? w2 : w1) = e;
}
static void TestTournamentNewcomerTakesAnOpenPlace() {
    Rig r(Cfg(Format::Tournament, 1), 4);
    const uint8_t a = r.m.PairA();
    uint8_t w1, w2; Waiting(r, w1, w2);
    r.m.Leave(w1, r.now);
    r.m.RemoveFighter(w1);                               // the reservation ran out: still open
    CHECK(r.m.Vacant(w1) && r.m.bracket().InRunning(w1));
    r.m.AddFighter({7, kNoTeam, true, 107});
    CHECK(!r.m.Vacant(w1) && r.m.bracket().InRunning(7) && !r.m.bracket().Contains(w1) && r.m.bracket().seeds().size() == 4);
    CHECK(r.m.Benched(7) && !r.m.Alive(7));              // he waits for his fight
    r.Win(a);
    r.Gap();
    CHECK((r.m.PairA() == 7 && r.m.PairB() == w2) || (r.m.PairA() == w2 && r.m.PairB() == 7));
    CHECK(r.m.Alive(7) && !r.m.Benched(7));
    r.Win(7);
    r.Gap();
    CHECK((r.m.PairA() == a && r.m.PairB() == 7) || (r.m.PairA() == 7 && r.m.PairB() == a));
}
static void TestTournamentOpenPlacesFirstLeftFirst() {
    Rig r(Cfg(Format::Tournament, 1), 4);
    uint8_t w1, w2; Waiting(r, w1, w2);
    r.m.Leave(w2, r.now); r.m.RemoveFighter(w2);
    r.m.Leave(w1, r.now); r.m.RemoveFighter(w1);
    CHECK(r.m.Vacant(w1) && r.m.Vacant(w2));
    r.m.AddFighter({7, kNoTeam, true, 107});
    CHECK(!r.m.Vacant(w2) && r.m.Vacant(w1) && r.m.bracket().Contains(7));
    r.m.AddFighter({8, kNoTeam, true, 108});
    CHECK(!r.m.Vacant(w1) && r.m.bracket().Contains(8));
    // a newcomer on a reused slot takes that slot's own place, wherever it is in the line
    Rig q(Cfg(Format::Tournament, 1), 4);
    Waiting(q, w1, w2);
    q.m.Leave(w1, q.now); q.m.RemoveFighter(w1);
    q.m.Leave(w2, q.now); q.m.RemoveFighter(w2);
    q.m.AddFighter({w2, kNoTeam, true, 300});
    CHECK(!q.m.Vacant(w2) && q.m.Vacant(w1) && q.m.bracket().InRunning(w2) && q.m.Benched(w2));
}
static void TestTournamentLeaverBackWaitsForTheNext() {
    // back on another slot (known by his id): no place for him, and his old one stays open for somebody else
    Rig r(Cfg(Format::Tournament, 1), 4);
    uint8_t w1, w2; Waiting(r, w1, w2);
    r.m.Leave(w1, r.now); r.m.RemoveFighter(w1);
    r.m.AddFighter({7, kNoTeam, true, uint64_t(100 + w1)});
    CHECK(r.m.Vacant(w1) && !r.m.bracket().Contains(7) && r.m.Benched(7));
    // back on his own slot within the reservation: benched, his place still open, a bye when due
    Rig q(Cfg(Format::Tournament, 1), 4);
    const uint8_t qa = q.m.PairA();
    Waiting(q, w1, w2);
    q.m.Leave(w1, q.now);
    q.m.Return(w1);
    CHECK(q.m.Benched(w1) && q.m.Vacant(w1));
    q.Win(qa);
    q.Gap();
    CHECK(!q.m.bracket().InRunning(w1) && q.m.PairA() != w1 && q.m.PairB() != w1 && q.m.Alive(w2));
    // left in the countdown (no Leave, the slot just went): his id counts too
    std::vector<FighterInfo> f;
    for (int e = 1; e <= 4; ++e) f.push_back({uint8_t(e), kNoTeam, true, uint64_t(100 + e)});
    Match m;
    m.SetBracketSeed(5);
    m.Start(Cfg(Format::Tournament, 1), f, 0);
    uint8_t c = 0;
    for (uint8_t e = 1; e <= 4 && !c; ++e) if (!InCurrent(m, e)) c = e;
    m.RemoveFighter(c);
    CHECK(m.Vacant(c) && m.bracket().InRunning(c));
    m.AddFighter({9, kNoTeam, true, uint64_t(100 + c)});
    CHECK(m.Vacant(c) && !m.bracket().Contains(9));
    m.AddFighter({10, kNoTeam, true, 0});                // no id: a newcomer
    CHECK(!m.Vacant(c) && m.bracket().Contains(10));
    // players with no id: one who leaves does not make every newcomer with no id "back"
    std::vector<FighterInfo> z;
    for (int e = 1; e <= 4; ++e) z.push_back({uint8_t(e), kNoTeam, true, 0});
    Match n;
    n.SetBracketSeed(5);
    n.Start(Cfg(Format::Tournament, 1), z, 0);
    c = 0;
    for (uint8_t e = 1; e <= 4 && !c; ++e) if (!InCurrent(n, e)) c = e;
    n.RemoveFighter(c);
    n.AddFighter({9, kNoTeam, true, 0});
    CHECK(!n.Vacant(c) && n.bracket().Contains(9));
}
static void TestTournamentNoOpenPlaceOnceDueOrWon() {
    // a winner who leaves before his next fight: a bye at once (his seat has a fight behind it); nobody takes it
    Rig r(Cfg(Format::Tournament, 1), 4);
    const uint8_t a = r.m.PairA();
    r.Win(a);
    r.Gap();
    CHECK(r.m.PairA() != a && r.m.PairB() != a);
    r.m.Leave(a, r.now);
    CHECK(!r.m.Vacant(a) && !r.m.bracket().InRunning(a));
    r.m.AddFighter({7, kNoTeam, true, 107});
    CHECK(!r.m.bracket().Contains(7));
    // one of the next fight who leaves in the gap before it, either side of it: a bye at once
    uint8_t w1, w2;
    for (int side = 0; side < 2; ++side) {
        Rig q(Cfg(Format::Tournament, 1), 4);
        const uint8_t qa = q.m.PairA();
        Waiting(q, w1, w2);
        q.Win(qa);                                       // the gap: the other semi-final is the fight to come
        Bracket::Game g;
        CHECK(q.m.bracket().Current(g));
        const uint8_t gone = side ? g.b : g.a, stays = side ? g.a : g.b;
        q.m.Leave(gone, q.now);
        CHECK(!q.m.Vacant(gone) && !q.m.bracket().InRunning(gone) && q.m.bracket().InRunning(stays));
        q.Gap();
        CHECK((q.m.PairA() == qa && q.m.PairB() == stays) || (q.m.PairA() == stays && q.m.PairB() == qa));
    }
    // one of the fight who leaves while down between its rounds: a forfeit, his place a bye at once
    Rig d(Cfg(Format::Tournament, 2), 4);
    const uint8_t da = d.m.PairA(), db = d.m.PairB();
    d.Win(da);                                           // 1-0, the gap: db is down
    CHECK(!d.m.Leave(db, d.now));
    CHECK(!d.m.Vacant(db) && !d.m.bracket().InRunning(db) && d.m.bracket().InRunning(da));
    // both of a game gone and nobody came: the last one takes the tournament when the fight before ends
    Rig e(Cfg(Format::Tournament, 1), 4);
    const uint8_t ea = e.m.PairA();
    Waiting(e, w1, w2);
    e.m.Leave(w1, e.now); e.m.Leave(w2, e.now);
    CHECK(e.m.Vacant(w1) && e.m.Vacant(w2) && e.m.phase() == MatchPhase::Live);
    e.Win(ea);
    CHECK(e.m.phase() == MatchPhase::Results && e.m.winner() == int(ea));
    // the rematch starts with no open place
    Match::Events ev;
    for (int i = 0; i < 3 && !ev.rematch; ++i) ev = e.Tick(Match::kResultsMs);
    CHECK(ev.rematch && !e.m.Vacant(w1) && !e.m.Vacant(w2));
}

static void TestTournamentOpenPlaceDueAfterAForfeit() {
    // a forfeit in the fight makes the open place's fight the next one: a bye at once, the final is up
    Rig r(Cfg(Format::Tournament, 1), 4);
    const uint8_t a = r.m.PairA(), b = r.m.PairB();
    uint8_t w1, w2; Waiting(r, w1, w2);
    r.m.Leave(w1, r.now);
    CHECK(r.m.Vacant(w1));
    r.m.Leave(a, r.now);                                 // a forfeits: b takes the semi-final
    CHECK(!r.m.Vacant(w1) && !r.m.bracket().InRunning(w1) && r.m.bracket().InRunning(w2));
    Bracket::Game g;
    CHECK(r.m.bracket().Current(g) && g.round == 1 && ((g.a == b && g.b == w2) || (g.a == w2 && g.b == b)));
    // the same when the slot of one of the fight goes in the countdown
    std::vector<FighterInfo> f;
    for (int e = 1; e <= 4; ++e) f.push_back({uint8_t(e), kNoTeam, true, uint64_t(100 + e)});
    Match m;
    m.SetBracketSeed(5);
    m.Start(Cfg(Format::Tournament, 1), f, 0);
    CHECK(m.bracket().Current(g));
    const uint8_t first = g.a;
    uint8_t c = 0;
    for (uint8_t e = 1; e <= 4 && !c; ++e) if (!InCurrent(m, e)) c = e;
    m.RemoveFighter(c);
    CHECK(m.Vacant(c));
    m.RemoveFighter(first);
    CHECK(!m.Vacant(c) && !m.bracket().InRunning(c) && m.bracket().Current(g) && g.round == 1);
}
static void TestTournamentRestartClearsOpenPlaces() {
    // the host starts again: no open place is left over, and one who left the last one is a newcomer again
    std::vector<FighterInfo> f;
    for (int e = 1; e <= 4; ++e) f.push_back({uint8_t(e), kNoTeam, true, uint64_t(100 + e)});
    Match m;
    m.SetBracketSeed(5);
    m.Start(Cfg(Format::Tournament, 1), f, 0);
    uint8_t c = 0;
    for (uint8_t e = 1; e <= 4 && !c; ++e) if (!InCurrent(m, e)) c = e;
    m.RemoveFighter(c);
    CHECK(m.Vacant(c));
    std::vector<FighterInfo> three;
    for (const FighterInfo& x : f) if (x.entity != c) three.push_back(x);
    m.Start(Cfg(Format::Tournament, 1), three, 0);
    CHECK(!m.Vacant(c) && !m.bracket().Contains(c) && m.bracket().seeds().size() == 3);
    const uint8_t top = m.bracket().seeds()[0];          // three players: the top seed's bye, not in the first fight
    CHECK(!InCurrent(m, top));
    m.RemoveFighter(top);
    CHECK(m.Vacant(top));
    m.AddFighter({c, kNoTeam, true, uint64_t(100 + c)});
    CHECK(!m.Vacant(top) && m.bracket().InRunning(c));
}

static void TestTournamentEveryoneLeavesButOne() {
    Rig r(Cfg(Format::Tournament, 2), 3);
    const uint8_t a = r.m.PairA(), b = r.m.PairB();
    uint8_t w = 0;
    for (uint8_t e = 1; e <= 3; ++e) if (e != a && e != b) w = e;
    r.m.Leave(a, r.now);
    r.m.Leave(b, r.now);
    CHECK(r.m.phase() == MatchPhase::Results && r.m.winner() == int(w));   // the one left takes it
}

static void TestTournamentLateJoinerWaitsForTheNext() {
    Rig r(Cfg(Format::Tournament, 1), 4);
    r.m.AddFighter({7, kNoTeam, true, 107});
    CHECK(r.m.Has(7) && r.m.Benched(7) && !r.m.Alive(7) && r.m.AwaitsRespawn(7));
    CHECK(!r.m.bracket().Contains(7) && !r.m.bracket().InRunning(7));
    // played to the end: never in a pairing
    while (r.m.phase() == MatchPhase::Live) {
        CHECK(r.m.PairA() != 7 && r.m.PairB() != 7);
        r.Win(r.m.PairA());
        if (r.m.phase() == MatchPhase::Live) r.Gap();
    }
    // the rematch seeds everybody, the late joiner too
    Match::Events ev;
    for (int i = 0; i < 3 && !ev.rematch; ++i) ev = r.Tick(Match::kResultsMs);
    CHECK(ev.rematch && r.m.bracket().Contains(7) && r.m.bracket().seeds().size() == 5);
    CHECK(r.m.round() == 1 && r.m.Score(r.m.PairA()) == 0);
    int benched = 0;
    for (uint8_t e : {1, 2, 3, 4, 7}) benched += r.m.Benched(e) ? 1 : 0;
    CHECK(benched == 3);
}

static void TestTournamentCountdownJoinerIsBenchedToo() {
    std::vector<FighterInfo> f;
    for (int e = 1; e <= 3; ++e) f.push_back({uint8_t(e), kNoTeam, true, 0});
    Match m;
    m.Start(Cfg(Format::Tournament, 2), f, 0);
    m.AddFighter({5, kNoTeam, true, 0});
    CHECK(m.Benched(5) && !m.Alive(5) && !m.bracket().Contains(5));
    // a benched fighter is not brought back by the countdown's fall rule
    Match::Events ev = m.Tick(Match::kCountdownMs);
    CHECK(ev.live && ev.respawn.empty() && m.Benched(5) && !m.Alive(5));
}

static void TestTournamentRematchReseeds() {
    Rig r(Cfg(Format::Tournament, 1), 4, 11);
    const std::vector<uint8_t> first = r.m.bracket().seeds();
    while (r.m.phase() == MatchPhase::Live) { r.Win(r.m.PairA()); if (r.m.phase() == MatchPhase::Live) r.Gap(); }
    Match::Events ev;
    for (int i = 0; i < 3 && !ev.rematch; ++i) ev = r.Tick(Match::kResultsMs);
    CHECK(ev.rematch && r.m.phase() == MatchPhase::Countdown && r.m.bracket().valid() && !r.m.bracket().Done());
    CHECK(r.m.PairingWins(1) == 0 && r.m.Score(1) == 0 && r.m.PairA() != 0);
    // every rematch draws another order, at least once in a few
    bool differs = false;
    for (int k = 0; k < 6 && !differs; ++k) {
        r.Tick(Match::kCountdownMs);
        while (r.m.phase() == MatchPhase::Live) { r.Win(r.m.PairA()); if (r.m.phase() == MatchPhase::Live) r.Gap(); }
        for (int i = 0; i < 3 && !ev.rematch; ++i) ev = r.Tick(Match::kResultsMs);
        ev = Match::Events{};
        differs = r.m.bracket().seeds() != first;
    }
    CHECK(differs);
}

static void TestTournamentDeterministic() {
    Rig a(Cfg(Format::Tournament, 2), 6, 77), b(Cfg(Format::Tournament, 2), 6, 77);
    CHECK(a.m.bracket().seeds() == b.m.bracket().seeds() && a.m.PairA() == b.m.PairA() && a.m.PairB() == b.m.PairB());
}

// ---------------------------------------------------------------- Pit Bridge Duels

static void TestPitStartAndWinnerStays() {
    Rig r(Cfg(Format::PitBridgeDuel, 5), 4);
    CHECK(r.m.queue().order().size() == 4);
    const uint8_t a = r.m.PairA(), b = r.m.PairB();
    CHECK(a == r.m.queue().order()[0] && b == r.m.queue().order()[1]);
    for (uint8_t e = 1; e <= 4; ++e) CHECK(r.m.Benched(e) == (e != a && e != b));
    const uint8_t c = r.m.queue().order()[2], d = r.m.queue().order()[3];
    r.Win(a);                                            // one round a fight: a takes it and stays
    CHECK(r.m.Score(a) == 1 && r.m.queue().streak() == 1 && r.m.queue().King() == a);
    CHECK((r.m.queue().order() == std::vector<uint8_t>{a, c, d, b}));
    Match::Events ev = r.Gap();
    CHECK(ev.roundReset && r.m.PairA() == a && r.m.PairB() == c);
    CHECK(Set(ev.respawn) == std::set<uint8_t>({a, c}) && Set(ev.bench) == std::set<uint8_t>({b}));   // b's body goes
    CHECK(r.m.Benched(b) && r.m.Benched(d) && !r.m.Benched(c) && r.m.Alive(c));
    r.Win(c);                                            // the challenger wins: he stays, the old king goes to the back
    CHECK(r.m.queue().King() == c && r.m.queue().streak() == 1 && r.m.Score(c) == 1 && r.m.Score(a) == 1);
    ev = r.Gap();
    CHECK(r.m.PairA() == c && r.m.PairB() == d && Set(ev.bench) == std::set<uint8_t>({a}));
}

static void TestPitStreakAndLimit() {
    Rig r(Cfg(Format::PitBridgeDuel, 3), 3);
    const uint8_t king = r.m.PairA();
    for (int i = 1; i <= 2; ++i) {
        r.Win(king);
        CHECK(r.m.queue().streak() == i && r.m.Score(king) == i && r.m.phase() == MatchPhase::Live);
        r.Gap();
        CHECK(r.m.PairA() == king);
    }
    r.Win(king);                                         // the third win takes the match
    CHECK(r.m.Score(king) == 3 && r.m.phase() == MatchPhase::Results && r.m.winner() == int(king));
}

static void TestPitDraw() {
    Rig r(Cfg(Format::PitBridgeDuel, 5), 4);
    const std::vector<uint8_t> o = r.m.queue().order();
    CHECK(r.m.Draw(r.now));                              // both down
    CHECK(r.m.roundWinner() == 0 && r.m.Score(o[0]) == 0 && r.m.Score(o[1]) == 0);
    CHECK((r.m.queue().order() == std::vector<uint8_t>{o[2], o[3], o[0], o[1]}) && r.m.queue().streak() == 0);
    Match::Events ev = r.Gap();
    CHECK(r.m.PairA() == o[2] && r.m.PairB() == o[3]);
    CHECK(Set(ev.bench) == std::set<uint8_t>({o[0], o[1]}) && Set(ev.respawn) == std::set<uint8_t>({o[2], o[3]}));
    CHECK(r.m.Draw(r.now));                              // drawn again
    CHECK((r.m.queue().order() == std::vector<uint8_t>{o[0], o[1], o[2], o[3]}));
}

static void TestPitTwoPlayersRepeat() {
    Rig r(Cfg(Format::PitBridgeDuel, 5), 2);
    const uint8_t a = r.m.PairA(), b = r.m.PairB();
    CHECK(r.m.Benched(a) == false && r.m.Benched(b) == false);
    r.Win(b);
    Match::Events ev = r.Gap();
    CHECK(r.m.PairA() == b && r.m.PairB() == a && ev.bench.empty() && ev.respawn.size() == 2);   // a duel again
    CHECK(r.m.Draw(r.now));
    r.Gap();
    CHECK(r.m.PairA() != 0 && r.m.PairB() != 0);         // a draw: the same two again
}

static void TestPitLeaversAndJoiners() {
    Rig r(Cfg(Format::PitBridgeDuel, 9), 4);
    const std::vector<uint8_t> o = r.m.queue().order();
    // a waiting player leaves
    CHECK(!r.m.Leave(o[3], r.now));
    CHECK((r.m.queue().order() == std::vector<uint8_t>{o[0], o[1], o[2]}));
    // a fighter leaves mid-round: the other wins it, the leaver is out of the line
    CHECK(r.m.Leave(o[0], r.now));
    CHECK(r.m.roundWinner() == o[1] && r.m.Score(o[1]) == 1);
    CHECK((r.m.queue().order() == std::vector<uint8_t>{o[1], o[2]}));
    r.Gap();
    CHECK(r.m.PairA() == o[1] && r.m.PairB() == o[2]);
    // a late joiner goes to the back and sits out
    r.m.AddFighter({8, kNoTeam, true, 108});
    CHECK(r.m.Benched(8) && !r.m.Alive(8) && r.m.AwaitsRespawn(8));
    CHECK((r.m.queue().order() == std::vector<uint8_t>{o[1], o[2], 8}));
    r.Win(o[2]);
    CHECK((r.m.queue().order() == std::vector<uint8_t>{o[2], 8, o[1]}));
    Match::Events ev = r.Gap();
    CHECK(r.m.PairA() == o[2] && r.m.PairB() == 8 && Set(ev.respawn) == std::set<uint8_t>({o[2], 8}) && !r.m.Benched(8) && r.m.Alive(8));
}

static void TestPitLeaverComesBack() {
    Rig r(Cfg(Format::PitBridgeDuel, 9), 3);
    const std::vector<uint8_t> o = r.m.queue().order();
    CHECK(!r.m.Leave(o[2], r.now));
    r.m.Return(o[2]);                                    // back within its reservation: at the back, benched
    CHECK((r.m.queue().order() == std::vector<uint8_t>{o[0], o[1], o[2]}));
    CHECK(r.m.Benched(o[2]) && !r.m.Alive(o[2]) && r.m.AwaitsRespawn(o[2]));
}

static void TestPitEndsWhenOnlyOneIsLeft() {
    Rig r(Cfg(Format::PitBridgeDuel, 9), 2);
    const uint8_t a = r.m.PairA(), b = r.m.PairB();
    r.Win(a);                                            // a leads 1-0
    r.Gap();
    r.m.Leave(b, r.now);
    CHECK(r.m.phase() == MatchPhase::Live);
    r.Tick();
    CHECK(r.m.phase() == MatchPhase::Results && r.m.winner() == int(a));
}

static void TestPitTimeLimitAndSuddenDeath() {
    Rig r(Cfg(Format::PitBridgeDuel, 9, 60), 3);
    const uint8_t a = r.m.PairA();
    r.Win(a);
    r.Gap();
    r.Tick(60000);                                       // the clock runs out: the leader wins
    CHECK(r.m.phase() == MatchPhase::Results && r.m.winner() == int(a));
    Rig tie(Cfg(Format::PitBridgeDuel, 9, 60), 3);
    tie.Tick(60000);                                     // nobody has won: sudden death, the next fight decides
    CHECK(tie.m.phase() == MatchPhase::SuddenDeath);
    const uint8_t w = tie.m.PairB();
    tie.Win(w);
    CHECK(tie.m.phase() == MatchPhase::Results && tie.m.winner() == int(w));
}

static void TestPitRematchStartsOver() {
    Rig r(Cfg(Format::PitBridgeDuel, 1), 3);
    r.Win(r.m.PairA());
    CHECK(r.m.phase() == MatchPhase::Results);
    Match::Events ev;
    for (int i = 0; i < 3 && !ev.rematch; ++i) ev = r.Tick(Match::kResultsMs);
    CHECK(ev.rematch && r.m.phase() == MatchPhase::Countdown && r.m.queue().order().size() == 3 && r.m.queue().streak() == 0);
    int fighting = 0;
    for (uint8_t e = 1; e <= 3; ++e) fighting += r.m.Alive(e) ? 1 : 0;
    CHECK(fighting == 2 && r.m.Score(1) == 0);
}

static void TestLoadoutsAheadForTwo() {
    Rig r(Cfg(Format::Tournament, 2), 6);
    MatchConfig c = Cfg(Format::Tournament, 2); c.gear = Gear::RandomTier;
    CHECK(StartLoadoutsAhead(c, 6) == 2 && StartLoadoutsAhead(Cfg(Format::Duel, 2), 6) == 0);
    c.format = Format::LastManStanding;
    CHECK(StartLoadoutsAhead(c, 6) == 6);
    c.format = Format::PitBridgeDuel;
    CHECK(StartLoadoutsAhead(c, 6) == 2 && StartLoadoutsAhead(c, 1) == 1);
}

static void TestLoadoutsAheadInTheGap() {
    MatchConfig c = Cfg(Format::Tournament, 2); c.gear = Gear::RandomTier;
    Rig r(c, 6);
    r.Win(r.m.PairA());                                  // the gap: only the two who fight next need a loadout
    CHECK(r.m.LoadoutsAhead() == 2);
}

static void TestFormatHelpers() {
    CHECK(uint8_t(Format::Tournament) == 14 && uint8_t(Format::PitBridgeDuel) == 15 && kFormatCount == 20);
    CHECK(IsQueueFormat(Format::Tournament) && IsQueueFormat(Format::PitBridgeDuel) && !IsQueueFormat(Format::Duel));
    CHECK(IsRoundFormat(Format::Tournament) && IsRoundFormat(Format::PitBridgeDuel) && ScoresPerFighter(Format::Tournament) && ScoresPerFighter(Format::PitBridgeDuel));
    CHECK(!HasTeams(Format::Tournament) && !IsTeamRound(Format::PitBridgeDuel) && !IsBotFormat(Format::Tournament));
    CHECK(DefaultScoreLimit(Format::Tournament) == 1 && DefaultScoreLimit(Format::PitBridgeDuel) == 5);
    CHECK(DropClearFor(Format::Tournament) == DropClear::EachRound && DropClearFor(Format::PitBridgeDuel) == DropClear::EachRound);
    // the lobby keys
    CHECK(std::string(LimitKeyFor(Format::Tournament)) == "hsm_trnr" && std::string(LimitKeyFor(Format::PitBridgeDuel)) == "hsm_pbwin" && std::string(LimitKeyFor(Format::Duel)) == "hsm_lim");
    for (int rounds : {1, 3, 5}) CHECK(LimitToKey(Format::Tournament, LimitFromKey(Format::Tournament, rounds)) == rounds);
    CHECK(LimitToKey(Format::Tournament, 2) == 3 && LimitFromKey(Format::Tournament, 3) == 2 && LimitFromKey(Format::Tournament, 0) == 1 && LimitFromKey(Format::Tournament, 99) == 3);
    CHECK(LimitToKey(Format::PitBridgeDuel, 5) == 5 && LimitFromKey(Format::PitBridgeDuel, 7) == 7 && LimitToKey(Format::Duel, 3) == 3);
}

int main() {
    TestTournamentStart();
    TestTournamentOneRoundThenNextPairing();
    TestTournamentFinalWinsTheMatch();
    TestTournamentThreeHaveAByeForTheTopSeed();
    TestTournamentDrawKeepsThePairing();
    TestTournamentForfeitDuringAPairing();
    TestTournamentForfeitAfterTheRoundWasWon();
    TestTournamentForfeitAfterADraw();
    TestTournamentWaitingPlayerLeavesIsABye();
    TestTournamentNewcomerTakesAnOpenPlace();
    TestTournamentOpenPlacesFirstLeftFirst();
    TestTournamentLeaverBackWaitsForTheNext();
    TestTournamentNoOpenPlaceOnceDueOrWon();
    TestTournamentOpenPlaceDueAfterAForfeit();
    TestTournamentRestartClearsOpenPlaces();
    TestTournamentEveryoneLeavesButOne();
    TestTournamentLateJoinerWaitsForTheNext();
    TestTournamentCountdownJoinerIsBenchedToo();
    TestTournamentRematchReseeds();
    TestTournamentDeterministic();
    TestPitStartAndWinnerStays();
    TestPitStreakAndLimit();
    TestPitDraw();
    TestPitTwoPlayersRepeat();
    TestPitLeaversAndJoiners();
    TestPitLeaverComesBack();
    TestPitEndsWhenOnlyOneIsLeft();
    TestPitTimeLimitAndSuddenDeath();
    TestPitRematchStartsOver();
    TestLoadoutsAheadForTwo();
    TestLoadoutsAheadInTheGap();
    TestFormatHelpers();
    if (g_fail) { std::printf("%d failure(s)\n", g_fail); return 1; }
    std::puts("ok");
    return 0;
}
