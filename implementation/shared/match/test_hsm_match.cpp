// AC-1 / AC-2: match rules, team resolution, kill attribution, waves, spawn sides.
#include "match/hsm_match.h"
#include <cstdio>
#include <set>

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++g_fail; } } while (0)
using namespace hsm;
static int T(const std::map<uint8_t, int8_t>& m, uint8_t slot) { auto it = m.find(slot); return it == m.end() ? -99 : it->second; }

static MatchConfig Cfg(Format f, uint16_t limit, uint16_t timeS = 0, uint16_t respawnS = 5) {
    MatchConfig c; c.format = f; c.scoreLimit = limit; c.timeLimitS = timeS; c.respawnDelayS = respawnS; return c;
}

static void TestSanitize() {
    MatchConfig c; c.format = Format(9); c.scoreLimit = 0; c.timeLimitS = 9000; c.respawnDelayS = 500;
    c.gear = Gear(7); c.tier = 9;
    MatchConfig s = Sanitize(c);
    CHECK(s.format == Format::FreeForAll && s.scoreLimit == 1 && s.timeLimitS == 3600 && s.respawnDelayS == 60);
    CHECK(s.gear == Gear::Own && s.tier == 7);
    c.format = Format::Zombies; CHECK(Sanitize(c).format == Format::Zombies && TeamCount(Format::Zombies) == 2 && TeamCount(Format::Duel) == 0);
    CHECK(IsBotFormat(Format::Zombies) && IsBotFormat(Format::PlayersVsBots) && !IsBotFormat(Format::Duel));
    CHECK(DefaultScoreLimit(Format::Duel) == 3);
    c = Cfg(Format::ThreeTeams, 250); CHECK(Sanitize(c).scoreLimit == 100 && Sanitize(c).format == Format::ThreeTeams);
    CHECK(TeamCount(Format::FreeForAll) == 0 && TeamCount(Format::TwoTeams) == 2 && TeamCount(Format::ThreeTeams) == 3 && TeamCount(Format::PlayersVsBots) == 2);
    CHECK(DefaultScoreLimit(Format::FreeForAll) == 5 && DefaultScoreLimit(Format::TwoTeams) == 10);
}

static void TestResolveTeams() {
    std::vector<TeamRequest> m = {{0, -1, -1}, {1, 1, -1}, {2, -1, -1}, {3, 1, 0}, {4, 1, -1}, {5, -1, -1}};
    auto two = ResolveTeams(m, Format::TwoTeams);
    // slot 3 override 0; slots 1,4 pick 1; unassigned 0,2,5 fill the smaller team in slot order.
    CHECK(T(two, 3) == 0 && T(two, 1) == 1 && T(two, 4) == 1);
    CHECK(T(two, 0) == 0 && T(two, 2) == 0 && T(two, 5) == 1);       // 0 -> team 0 (1 vs 2), 2 -> tie -> 0, 5 -> 1
    auto three = ResolveTeams({{0, -1, -1}, {1, -1, -1}, {2, -1, -1}, {3, 2, -1}}, Format::ThreeTeams);
    CHECK(T(three, 3) == 2 && T(three, 0) == 0 && T(three, 1) == 1 && T(three, 2) == 0);   // smallest team, ties lowest index
    auto bad = ResolveTeams({{0, 5, 7}, {1, -3, -1}}, Format::TwoTeams);              // out-of-range picks ignored
    CHECK(T(bad, 0) == 0 && T(bad, 1) == 1);
    auto uneven = ResolveTeams({{0, 0, -1}, {1, 0, -1}, {2, 0, -1}}, Format::TwoTeams);
    CHECK(T(uneven, 0) == 0 && T(uneven, 1) == 0 && T(uneven, 2) == 0);            // picks are honoured, uneven allowed
    auto ffa = ResolveTeams(m, Format::FreeForAll);
    for (auto& [s, t] : ffa) CHECK(t == kNoTeam);
    auto pvb = ResolveTeams(m, Format::PlayersVsBots);
    for (auto& [s, t] : pvb) CHECK(t == kHumanTeam);
    for (auto& [s, t] : ResolveTeams(m, Format::Zombies)) CHECK(t == kHumanTeam);
    for (auto& [s, t] : ResolveTeams(m, Format::Duel)) CHECK(t == kNoTeam);
    CHECK(ffa.size() == 6 && pvb.size() == 6);
}

static void TestColors() {
    CHECK(TeamColor(kNoTeam).a == 0.f);
    std::set<std::tuple<float, float, float>> seen;
    for (int t = 0; t < 4; ++t) { Rgba c = TeamColor(t); CHECK(c.a == 1.f); seen.insert({c.r, c.g, c.b}); }
    CHECK(seen.size() == 4);
    CHECK(TeamColor(0).r > TeamColor(0).b && TeamColor(1).b > TeamColor(1).r);   // red, blue first
}

static void TestKillLog() {
    KillLog k;
    CHECK(k.KillerOf(2, 0) == 0);
    k.OnHit(2, 3, 1000); k.OnHit(2, 4, 2000);
    CHECK(k.KillerOf(2, 5000) == 4);                                   // last contact wins
    CHECK(k.KillerOf(2, 2000 + KillLog::kWindowMs) == 4);
    CHECK(k.KillerOf(2, 2000 + KillLog::kWindowMs + 1) == 0);          // stale
    k.OnHit(5, 5, 100); CHECK(k.KillerOf(5, 200) == 0);                // self contact ignored
    k.OnHit(6, 0, 100); CHECK(k.KillerOf(6, 200) == 0);
    k.Forget(2); CHECK(k.KillerOf(2, 2500) == 0);
}

static std::vector<FighterInfo> Teams2() { return {{1, 0, true}, {2, 1, true}, {3, 0, true}, {4, 1, true}}; }

static void TestTeamScoringAndLimit() {
    Match m; m.Start(Cfg(Format::TwoTeams, 3), Teams2(), 0);
    CHECK(m.phase() == MatchPhase::Countdown && m.TimeLeftMs(1000) == Match::kCountdownMs - 1000);
    m.OnDeath(2, 1, 1000); CHECK(m.Score(0) == 0 && m.Deaths(2) == 0 && !m.Alive(2));   // countdown: dies, not scored
    m.MarkRespawned(2);
    auto ev = m.Tick(Match::kCountdownMs);
    CHECK(ev.live && m.phase() == MatchPhase::Live);
    uint32_t t = Match::kCountdownMs;
    m.OnDeath(2, 1, t + 10); CHECK(m.Score(0) == 1 && m.Kills(1) == 1 && m.Deaths(2) == 1 && !m.Alive(2));
    m.OnDeath(3, 1, t + 20); CHECK(m.Score(0) == 0 && m.Kills(1) == 1);      // teamkill: -1, no kill credit
    m.OnDeath(1, 1, t + 30); CHECK(m.Score(0) == -1);                         // suicide: -1
    m.OnDeath(4, 0, t + 40); CHECK(m.Score(1) == 0 && m.Deaths(4) == 1);      // environment: no score change
    for (uint8_t e : {1, 2, 3, 4}) m.MarkRespawned(e);
    m.OnDeath(4, 1, t + 50); m.MarkRespawned(4);
    m.OnDeath(2, 3, t + 60); m.OnDeath(2, 3, t + 65);                        // second report of the same death ignored
    m.MarkRespawned(2); m.OnDeath(4, 3, t + 70);
    CHECK(m.Score(0) == 2 && m.Kills(3) == 2 && m.Deaths(2) == 2 && m.phase() == MatchPhase::Live);
    m.MarkRespawned(4); m.OnDeath(4, 1, t + 80);
    CHECK(m.Score(0) == 3 && m.phase() == MatchPhase::Results && m.winner() == 0);
    ev = m.Tick(t + 81); CHECK(ev.ended);
    m.OnDeath(1, 2, t + 90); CHECK(m.Score(1) == 0);                          // results: not scored
    ev = m.Tick(t + 80 + Match::kResultsMs);
    CHECK(ev.rematch && m.phase() == MatchPhase::Countdown && m.Score(0) == 0 && m.Kills(1) == 0 && m.Alive(4) && m.winner() == Match::kNoWinner);
}

static void TestRespawnSchedule() {
    Match m; m.Start(Cfg(Format::TwoTeams, 10, 0, 5), Teams2(), 0);
    m.Tick(Match::kCountdownMs);
    uint32_t t = 10000;
    m.OnDeath(2, 1, t);
    CHECK(m.Tick(t + 4999).respawn.empty());
    auto ev = m.Tick(t + 5000);
    CHECK(ev.respawn.size() == 1 && ev.respawn[0] == 2);
    CHECK(m.Tick(t + 6000).respawn == std::vector<uint8_t>({2}));            // repeats until marked
    m.MarkRespawned(2); CHECK(m.Tick(t + 7000).respawn.empty() && m.Alive(2));
    m.OnDeath(2, 1, t + 8000); m.OnDeath(2, 1, t + 8100);                     // duplicate death report ignored
    CHECK(m.Deaths(2) == 2 && m.Score(0) == 2);
}

static void TestTimeLimitAndSuddenDeath() {
    Match m; m.Start(Cfg(Format::TwoTeams, 10, 60), Teams2(), 0);
    m.Tick(Match::kCountdownMs);
    uint32_t live = Match::kCountdownMs;
    CHECK(m.TimeLeftMs(live + 1000) == 59000);
    m.OnDeath(2, 1, live + 100); m.OnDeath(1, 2, live + 200);                 // 1 : 1
    auto ev = m.Tick(live + 60000);
    CHECK(m.phase() == MatchPhase::SuddenDeath && !ev.ended);
    m.MarkRespawned(1); m.OnDeath(1, 1, live + 61000);                        // suicide breaks the tie for team 1
    CHECK(m.phase() == MatchPhase::Results && m.winner() == 1);
    Match w; w.Start(Cfg(Format::TwoTeams, 10, 30), Teams2(), 0); w.Tick(Match::kCountdownMs);
    w.OnDeath(2, 1, Match::kCountdownMs + 5);
    w.Tick(Match::kCountdownMs + 30000);
    CHECK(w.phase() == MatchPhase::Results && w.winner() == 0);               // leader at time-out wins
    Match u; u.Start(Cfg(Format::TwoTeams, 10, 0), Teams2(), 0); u.Tick(Match::kCountdownMs);
    CHECK(u.TimeLeftMs(Match::kCountdownMs + 999999) == 0 && u.Tick(Match::kCountdownMs + 999999).ended == false);
}

static void TestFreeForAll() {
    Match m; m.Start(Cfg(Format::FreeForAll, 2), {{1, kNoTeam, true}, {2, kNoTeam, true}, {3, kNoTeam, true}}, 0);
    m.Tick(Match::kCountdownMs);
    uint32_t t = Match::kCountdownMs;
    CHECK(m.ScoreKeyOf(2) == 2);
    m.OnDeath(2, 3, t + 1); m.OnDeath(1, 1, t + 2);
    CHECK(m.Score(3) == 1 && m.Score(1) == -1 && m.phase() == MatchPhase::Live);
    m.MarkRespawned(2); m.OnDeath(2, 3, t + 3);
    CHECK(m.phase() == MatchPhase::Results && m.winner() == 3);
}

// Spec revision (V2-P3B, user answers): players-vs-bots is now Survival - endless waves scored by waves cleared,
// no timed respawns (downed players return when their team clears a wave), the run ends when every player is down.
static std::vector<FighterInfo> Two() { return {{1, kHumanTeam, true}, {2, kHumanTeam, true}}; }
static Match LiveSurvival(Format f, uint32_t& t) {
    MatchConfig c = Cfg(f, 10); c.tier = 2;
    Match m; m.Start(c, Two(), 0);
    auto ev = m.Tick(Match::kCountdownMs);
    CHECK(ev.live && ev.waveStarted && m.wave() == 0);
    t = Match::kCountdownMs;
    return m;
}

static void TestSurvivalWaves() {
    uint32_t t = 0;
    Match m = LiveSurvival(Format::PlayersVsBots, t);
    Wave w0 = WaveFor(Format::PlayersVsBots, 0, 2, 2);
    CHECK(m.botsLeft() == w0.count);
    auto ev = m.Tick(t + 1);
    CHECK(ev.spawnBots == std::min(w0.count, Match::kMaxBotsAlive) && ev.botRank == w0.rank);
    for (int i = 0; i < w0.count; ++i) m.AddFighter({uint8_t(7 + i), kBotTeam, false});
    CHECK(m.Tick(t + 2).spawnBots == 0 && m.botsLeft() == w0.count);              // all spawned, all alive
    m.OnDeath(1, 7, t + 3);                                                     // a player goes down: stays down
    CHECK(m.Tick(t + 3 + 60000).respawn.empty());
    for (int i = 0; i < w0.count - 1; ++i) m.OnDeath(uint8_t(7 + i), 2, t + 10);
    CHECK(m.Kills(2) == w0.count - 1 && m.Score(kHumanTeam) == 0);              // kills credited, not scored
    m.OnDeath(uint8_t(7 + w0.count - 1), 2, t + 20);                            // last bot: wave cleared
    ev = m.Tick(t + 21);
    CHECK(ev.waveCleared && ev.respawn == std::vector<uint8_t>({1}) && m.Score(kHumanTeam) == 1 && m.wave() == 1);
    m.MarkRespawned(1);
    CHECK(m.Tick(t + 22).spawnBots == 0);                                       // break: nothing spawns
    ev = m.Tick(t + 21 + Match::kWaveBreakMs);
    CHECK(ev.waveStarted && m.botsLeft() == WaveFor(Format::PlayersVsBots, 1, 2, 2).count);
    auto stale = m.StaleBotCorpses(t + 20 + Match::kCorpseMs, Match::kCorpseMs);
    CHECK(stale.size() == size_t(w0.count));
    CHECK(m.StaleBotCorpses(t + 19 + Match::kCorpseMs, Match::kCorpseMs).size() == size_t(w0.count - 1));
    m.OnDeath(1, 0, t + 30000); m.OnDeath(2, 0, t + 30001);                     // everyone down: run over
    CHECK(m.phase() == MatchPhase::Results && m.winner() == kBotTeam);
}

static void TestSurvivalAliveCap() {
    uint32_t t = 0;
    Match m = LiveSurvival(Format::Zombies, t);
    Wave w = WaveFor(Format::Zombies, 0, 2, 2);
    CHECK(w.count > Match::kMaxBotsAlive);                                      // hordes exceed the alive cap
    auto ev = m.Tick(t + 1); CHECK(ev.spawnBots == Match::kMaxBotsAlive);
    for (int i = 0; i < Match::kMaxBotsAlive; ++i) m.AddFighter({uint8_t(7 + i), kBotTeam, false});
    CHECK(m.Tick(t + 2).spawnBots == 0);
    m.OnDeath(7, 1, t + 3); m.RemoveFighter(7);                                 // one dies and is cleaned up
    CHECK(m.Tick(t + 4).spawnBots == 1);                                        // refill from the wave's remainder
    CHECK(m.Score(kHumanTeam) == 0 && m.botsLeft() == w.count - 1);
    m.OnDeath(1, 8, t + 5);
    CHECK(m.Tick(t + 6).respawn.empty());                                       // no timed respawns
}

static void TestWaves() {
    Wave s0 = WaveFor(Format::PlayersVsBots, 0, 2, 2), s3 = WaveFor(Format::PlayersVsBots, 3, 2, 2);
    CHECK(s0.count == 2 && s0.rank == 2 && s3.count == 5 && s3.rank == 3);      // humans + wave; tier + wave/2
    Wave z0 = WaveFor(Format::Zombies, 0, 2, 0), z6 = WaveFor(Format::Zombies, 6, 2, 0);
    CHECK(z0.count == 12 && z6.count == 36 && z0.rank == 0 && z6.rank == 2);   // 6 per human + 4 per wave; tier + wave/3
    CHECK(WaveFor(Format::PlayersVsBots, 100, 6, 7).rank == 7 && WaveFor(Format::PlayersVsBots, 100, 6, 7).count == 30);
    CHECK(WaveFor(Format::Zombies, 100, 6, 0).count == 60 && WaveFor(Format::PlayersVsBots, 0, 0, 0).count == 1);
}

static void TestDuelRounds() {
    MatchConfig c = Cfg(Format::Duel, 2);
    Match m; m.Start(c, {{1, kNoTeam, true}, {2, kNoTeam, true}}, 0);
    m.Tick(Match::kCountdownMs);
    uint32_t t = Match::kCountdownMs;
    m.OnDeath(2, 0, t + 1);                                                     // fell off / bled out: round still goes to 1
    CHECK(m.Score(1) == 1 && m.phase() == MatchPhase::Live);
    m.OnDeath(1, 0, t + 2);                                                     // round already decided: no point
    CHECK(m.Score(2) == 0);
    CHECK(m.Tick(t + 1 + Match::kRoundGapMs - 1).respawn.empty());
    auto ev = m.Tick(t + 1 + Match::kRoundGapMs);
    CHECK(ev.roundReset && ev.respawn == std::vector<uint8_t>({1, 2}) && m.Alive(1) && m.Alive(2));
    m.OnDeath(1, 2, t + 10000);                                                 // second round to player 2
    CHECK(m.Score(2) == 1 && m.Kills(2) == 1);
    CHECK(m.Tick(t + 10000 + 60000).respawn.empty() == false);                  // only the round reset revives
    m.OnDeath(2, 1, t + 80000);
    CHECK(m.phase() == MatchPhase::Results && m.winner() == 1);
    Match three; three.Start(Cfg(Format::Duel, 3), {{1, kNoTeam, true}, {2, kNoTeam, true}, {3, kNoTeam, true}}, 0);
    three.Tick(Match::kCountdownMs);
    three.OnDeath(1, 2, t + 1); CHECK(three.Score(2) == 0 && three.Score(3) == 0);  // two still standing
    three.OnDeath(3, 2, t + 2); CHECK(three.Score(2) == 1);                     // last one standing
}

// DUEL-5: a fighter already dead when Start observes it, or dying at the Countdown boundary,
// must recover without points, and must not enter Live until a fresh full countdown has elapsed.
static void TestDuelCountdownRecovery() {
    for (uint32_t deathAt : {0u, 1000u, Match::kCountdownMs}) {
        Match m; m.Start(Cfg(Format::Duel, 2), {{1, kNoTeam, true}, {2, kNoTeam, true}}, 0);
        m.OnDeath(1, 0, deathAt);
        auto ev = m.Tick(deathAt);
        CHECK(ev.countdownRestart && ev.respawn == std::vector<uint8_t>({1}) && !ev.live);
        CHECK(!m.Alive(1) && m.phase() == MatchPhase::Countdown && m.Score(2) == 0 && m.Deaths(1) == 0);
        CHECK(m.Tick(deathAt + 999).respawn.empty());
        ev = m.Tick(deathAt + 1000); // failed/unacknowledged spawn: retry, never silently mark alive
        CHECK(ev.respawn == std::vector<uint8_t>({1}) && !ev.live);
        m.MarkRespawned(1);
        CHECK(!m.Tick(deathAt + 1000 + Match::kCountdownMs - 1).live);
        CHECK(m.Tick(deathAt + 1000 + Match::kCountdownMs).live);
        m.OnDeath(1, 0, deathAt + 7000); // Live environmental death awards the survivor, not a kill
        CHECK(m.Score(2) == 1 && m.Kills(2) == 0);
        ev = m.Tick(deathAt + 7000 + Match::kRoundGapMs);
        CHECK(ev.roundReset && ev.respawn == std::vector<uint8_t>({1, 2}));
        m.RespawnFailed(1, deathAt + 11000);
        CHECK(!m.Alive(1) && m.phase() == MatchPhase::Countdown && m.Score(2) == 1);
        CHECK(m.Tick(deathAt + 12000).respawn == std::vector<uint8_t>({1}));
        m.MarkRespawned(1); m.Tick(deathAt + 17000);
        m.OnDeath(1, 0, deathAt + 17001);
        CHECK(m.phase() == MatchPhase::Results && m.winner() == 2 && m.Score(2) == 2);
        ev = m.Tick(deathAt + 17001 + Match::kResultsMs);
        CHECK(ev.rematch && m.Alive(1) && m.Alive(2) && m.phase() == MatchPhase::Countdown && m.Score(2) == 0);
    }
    Match both; both.Start(Cfg(Format::Duel, 2), {{1, kNoTeam, true}, {2, kNoTeam, true}}, 0);
    both.OnDeath(1, 2, 10); both.OnDeath(2, 1, 10);
    CHECK(both.Tick(6000).respawn == std::vector<uint8_t>({1, 2})); // late frame cannot skip recovery
    CHECK(both.phase() == MatchPhase::Countdown && both.scores().empty());
    both.MarkRespawned(1);
    CHECK(both.Tick(7000).respawn == std::vector<uint8_t>({2}));
    for (auto phase : {MatchPhase::Idle, MatchPhase::Countdown, MatchPhase::Results, MatchPhase(255)})
        CHECK(!CombatPhase(phase));
    CHECK(CombatPhase(MatchPhase::Live) && CombatPhase(MatchPhase::SuddenDeath));
    KillLog k;
    CHECK(k.AgeText(1, 20000) == "none");
    k.OnDamage(1, 2, 0xfffffff0u, 10, true);
    CHECK(k.AgeText(1, 0x10u) == "32ms"); // real wrap is a valid elapsed duration
    CHECK(k.AgeText(1, 0xfffffff0u) == "0ms");
    k.Forget(1); CHECK(k.AgeText(1, 0) == "none");
}

static void TestHasAndFinish() {
    Match m; m.Start(Cfg(Format::FreeForAll, 5), {{1, kNoTeam, true}}, 0);
    CHECK(m.Has(1) && !m.Has(2));
    m.Tick(Match::kCountdownMs);
    m.Finish(kHumanTeam, Match::kCountdownMs + 10);                           // e.g. Progression: every opponent is down
    CHECK(m.phase() == MatchPhase::Results && m.winner() == kHumanTeam && m.Tick(Match::kCountdownMs + 11).ended);
    m.Finish(7, Match::kCountdownMs + 20); CHECK(m.winner() == kHumanTeam);   // already over: ignored
}

static void TestSides() {
    // 8 points on a circle; 2 teams -> two contiguous half circles; 4 points per side.
    std::vector<Point3> ring;
    for (int i = 0; i < 8; ++i) { double a = i * 3.14159265358979 / 4; ring.push_back({std::cos(a) * 1000, std::sin(a) * 1000, 0}); }
    auto s2 = PartitionSides(ring, 2);
    int c0 = 0; for (int8_t s : s2) c0 += s == 0;
    CHECK(c0 == 4);
    int changes = 0; for (int i = 0; i < 8; ++i) changes += s2[i] != s2[(i + 1) % 8];
    CHECK(changes == 2);                                                      // contiguous arcs
    auto s3 = PartitionSides(ring, 3);
    std::set<int> sides(s3.begin(), s3.end()); CHECK(sides.size() == 3);
    changes = 0; for (int i = 0; i < 8; ++i) changes += s3[i] != s3[(i + 1) % 8];
    CHECK(changes == 3);
    for (int8_t s : PartitionSides(ring, 0)) CHECK(s == 0);
    auto few = PartitionSides({{0, 0, 0}}, 3); CHECK(few.size() == 1 && few[0] == 0);
    // Respawn: team 0 side, farthest from the enemy.
    size_t pick = PickRespawn(ring, s2, s2[0], {ring[0]});
    CHECK(s2[pick] == s2[0] && pick != 0);
    double best = 0; size_t bestI = 0;
    for (size_t i = 0; i < ring.size(); ++i) if (s2[i] == s2[0]) { double d = Dist2(ring[i], ring[0]); if (d > best) { best = d; bestI = i; } }
    CHECK(pick == bestI);
    CHECK(PickRespawn(ring, s2, 5, {}) == 0);                                 // no side for team 5, no enemies -> first point
    size_t ffa = PickRespawn(ring, s2, kNoTeam, {ring[2]});
    CHECK(ffa == 6);                                                          // opposite point on the whole ring
}

static void TestZeroScoreBeatsNegative() {
    Match f; f.Start(Cfg(Format::FreeForAll, 5, 60), {{1, kNoTeam, true}, {2, kNoTeam, true}}, 0);
    f.Tick(Match::kCountdownMs);
    f.OnDeath(1, 1, Match::kCountdownMs + 10);                                // player 1 suicides: -1, player 2 never scored
    f.Tick(Match::kCountdownMs + 60000);
    CHECK(f.phase() == MatchPhase::Results && f.winner() == 2);
    Match t; t.Start(Cfg(Format::TwoTeams, 5, 60), Teams2(), 0);
    t.Tick(Match::kCountdownMs);
    t.OnDeath(3, 1, Match::kCountdownMs + 10);                                // team 0 teamkill: -1, team 1 at 0
    t.Tick(Match::kCountdownMs + 60000);
    CHECK(t.phase() == MatchPhase::Results && t.winner() == 1);
}

static void TestEndedSurvivesLateTick() {
    Match m; m.Start(Cfg(Format::TwoTeams, 1), Teams2(), 0);
    m.Tick(Match::kCountdownMs);
    m.OnDeath(2, 1, Match::kCountdownMs + 10);                                // limit reached: match ends
    auto ev = m.Tick(Match::kCountdownMs + 10 + Match::kResultsMs);           // host stalled through the whole results phase
    CHECK(ev.ended && ev.rematch);
    CHECK(!m.Tick(Match::kCountdownMs + 20 + Match::kResultsMs).ended);       // not reported again in the next match
}

static void TestHitAllowed() {
    CHECK(HitAllowed(true, kNoTeam, kNoTeam, false));                         // my body, no teams: every hit counts
    CHECK(!HitAllowed(false, kNoTeam, kNoTeam, true));                        // not my body: its owner processes it
    CHECK(!HitAllowed(true, 0, 0, false) && HitAllowed(true, 0, 0, true));    // teammates: only with friendly fire
    CHECK(HitAllowed(true, 0, 1, false));                                     // enemies always
}

int main() {
    TestHitAllowed();
    TestZeroScoreBeatsNegative(); TestEndedSurvivesLateTick();
    TestSanitize(); TestResolveTeams(); TestColors(); TestKillLog(); TestTeamScoringAndLimit(); TestRespawnSchedule();
    TestTimeLimitAndSuddenDeath(); TestFreeForAll(); TestSurvivalWaves(); TestSurvivalAliveCap(); TestDuelRounds(); TestDuelCountdownRecovery(); TestHasAndFinish(); TestWaves(); TestSides();
    std::printf(g_fail ? "%d FAILED\n" : "all match tests passed\n", g_fail);
    return g_fail ? 1 : 0;
}
