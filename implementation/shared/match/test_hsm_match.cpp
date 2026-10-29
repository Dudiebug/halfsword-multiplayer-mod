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
    k.OnHit(7, 3, 1000); CHECK(k.KillerOf(7, 1000 + 45000) == 3);
    for (int t = 0; t < 7; ++t) CHECK(MuscleForTier(t) < MuscleForTier(t + 1));   // more muscle every rank up
    CHECK(MuscleForTier(-3) == MuscleForTier(0) && MuscleForTier(12) == MuscleForTier(7) && MuscleForTier(7) == 1.0);
    CHECK(MuscleForTier(0) > 0.0 && MuscleFor(0.1, 0) == MuscleForTier(0));
    CHECK(MuscleFor(0.95, 2) == 0.95 && MuscleFor(0.3, 7) == 1.0);                // never lowers a heavier build    // a bleed-out 45 s after the blow is still a kill
    CHECK(!CombatPhase(MatchPhase::Idle) && !CombatPhase(MatchPhase::Countdown) && !CombatPhase(MatchPhase::Results));
    CHECK(CombatPhase(MatchPhase::Live) && CombatPhase(MatchPhase::SuddenDeath));
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
    CHECK(ev.spawnBots == 1 && ev.botRank == w0.rank);                          // one bot per tick (TestWavePacing)
    for (int i = 0; i < w0.count; ++i) {
        if (i) CHECK(m.Tick(t + 1).spawnBots == 1);
        m.AddFighter({uint8_t(7 + i), kBotTeam, false});
    }
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
    for (int i = 0; i < Match::kMaxBotsAlive; ++i) {
        CHECK(m.Tick(t + 1).spawnBots == 1);                                    // one bot per tick (TestWavePacing)
        m.AddFighter({uint8_t(7 + i), kBotTeam, false});
    }
    CHECK(m.Tick(t + 2).spawnBots == 0);
    m.OnDeath(7, 1, t + 3); m.RemoveFighter(7);                                 // one dies and is cleaned up
    CHECK(m.Tick(t + 4).spawnBots == 1);                                        // refill from the wave's remainder
    CHECK(m.Score(kHumanTeam) == 0 && m.botsLeft() == w.count - 1);
    m.OnDeath(1, 8, t + 5);
    CHECK(m.Tick(t + 6).respawn.empty());                                       // no timed respawns
}

// A wave spawns one bot per Tick (the host ticks once a frame): test 5 spawned a whole Abyss horde in one frame and the
// frame took seconds. The alive cap and the wave count still hold; a bot that died makes room for the next one.
static void TestWavePacing() {
    uint32_t t = 0;
    Match m = LiveSurvival(Format::Zombies, t);
    Wave w = WaveFor(Format::Zombies, 0, 2, 2);
    CHECK(w.count >= Match::kMaxBotsAlive + 2);
    for (int i = 0; i < Match::kMaxBotsAlive; ++i) {
        auto ev = m.Tick(t + 1 + uint32_t(i));
        CHECK(ev.spawnBots == 1 && ev.botRank == w.rank);
        if (i < Match::kMaxBotsAlive - 1) m.AddFighter({uint8_t(7 + i), kBotTeam, false});
    }
    CHECK(m.Tick(t + 19).spawnBots == 0);                                       // a requested bot not added yet counts
    m.AddFighter({uint8_t(7 + Match::kMaxBotsAlive - 1), kBotTeam, false});     // against the alive cap
    CHECK(m.Tick(t + 20).spawnBots == 0 && m.botsLeft() == w.count);            // at the alive cap
    m.OnDeath(7, 1, t + 21); m.OnDeath(8, 1, t + 21);                           // two die at once: refilled one at a time
    CHECK(m.Tick(t + 22).spawnBots == 1);
    m.AddFighter({17, kBotTeam, false});
    CHECK(m.Tick(t + 23).spawnBots == 1);
    m.AddFighter({18, kBotTeam, false});
    CHECK(m.Tick(t + 24).spawnBots == 0 && m.botsLeft() == w.count - 2);
    Match f = LiveSurvival(Format::PlayersVsBots, t);
    Wave w0 = WaveFor(Format::PlayersVsBots, 0, 2, 2);
    CHECK(w0.count == 2);
    CHECK(f.Tick(t + 1).spawnBots == 1);                                        // not yet added: the next tick asks for
    CHECK(f.Tick(t + 2).spawnBots == 1);                                        // the next bot of the wave
    CHECK(f.Tick(t + 3).spawnBots == 0 && f.botsLeft() == w0.count);            // the wave is all requested
    f.SpawnFailed(1);                                                           // one could not be spawned: back to the wave
    CHECK(f.Tick(t + 4).spawnBots == 1);
}

static void TestWaves() {
    Wave s0 = WaveFor(Format::PlayersVsBots, 0, 2, 2), s3 = WaveFor(Format::PlayersVsBots, 3, 2, 2);
    CHECK(s0.count == 2 && s0.rank == 2 && s3.count == 5 && s3.rank == 3);      // humans + wave; tier + wave/2
    Wave z0 = WaveFor(Format::Zombies, 0, 2, 0), z6 = WaveFor(Format::Zombies, 6, 2, 0);
    CHECK(z0.count == 12 && z6.count == 36 && z0.rank == 0 && z6.rank == 2);   // 6 per human + 4 per wave; tier + wave/3
    CHECK(WaveFor(Format::PlayersVsBots, 100, 6, 7).rank == 7 && WaveFor(Format::PlayersVsBots, 100, 6, 7).count == 30);
    CHECK(WaveFor(Format::Zombies, 100, 6, 0).count == 60 && WaveFor(Format::PlayersVsBots, 0, 0, 0).count == 1);
}

// Spec revision (rounds plan 1.1): the gap between Duel rounds is the lobby's respawn delay (default 5 s), not a fixed
// 4 s, so the host controls it and the HUD counts it down (RoundGapLeftMs). round() numbers the rounds.
static void TestDuelRounds() {
    MatchConfig c = Cfg(Format::Duel, 2);                                       // respawn delay 5 s: the gap
    Match m; m.Start(c, {{1, kNoTeam, true}, {2, kNoTeam, true}}, 0);
    CHECK(m.round() == 1 && m.RoundGapLeftMs(0) == 0 && m.roundWinner() == 0);
    m.Tick(Match::kCountdownMs);
    uint32_t t = Match::kCountdownMs;
    CHECK(m.round() == 1 && m.RoundGapLeftMs(t + 1) == 0);                      // no gap while the round is fought
    m.OnDeath(2, 0, t + 1);                                                     // fell off / bled out: round still goes to 1
    CHECK(m.Score(1) == 1 && m.phase() == MatchPhase::Live && m.roundWinner() == 1);
    CHECK(m.RoundGapLeftMs(t + 1) == 5000 && m.RoundGapLeftMs(t + 2001) == 3000 && m.RoundGapLeftMs(t + 5000) == 1);
    CHECK(m.RoundGapLeftMs(t + 5001) == 0);
    m.OnDeath(1, 0, t + 2);                                                     // round already decided: no point
    CHECK(m.Score(2) == 0 && m.roundWinner() == 1);                             // still player 1's round, though down
    CHECK(m.Tick(t + 1 + 5000 - 1).respawn.empty() && m.round() == 1);
    auto ev = m.Tick(t + 1 + 5000);
    CHECK(ev.roundReset && ev.respawn == std::vector<uint8_t>({1, 2}) && m.Alive(1) && m.Alive(2));
    CHECK(m.round() == 2 && m.RoundGapLeftMs(t + 5001) == 0 && m.roundWinner() == 0);
    ev = m.Tick(t + 5002);
    CHECK(!ev.roundReset && ev.respawn.empty() && m.round() == 2);              // one reset, one round
    m.OnDeath(1, 2, t + 10000);                                                 // second round to player 2
    CHECK(m.Score(2) == 1 && m.Kills(2) == 1 && m.roundWinner() == 2 && m.round() == 2);
    CHECK(m.Tick(t + 10000 + 60000).respawn.empty() == false);                  // only the round reset revives
    CHECK(m.round() == 3);
    m.OnDeath(2, 1, t + 80000);
    CHECK(m.phase() == MatchPhase::Results && m.winner() == 1);
    CHECK(m.RoundGapLeftMs(t + 80000) == 0 && m.round() == 3);                  // the deciding round has no gap
    CHECK(!m.Tick(t + 80000 + 5000).roundReset && m.round() == 3);
    ev = m.Tick(t + 80000 + Match::kResultsMs);
    CHECK(ev.rematch && m.round() == 1 && m.roundWinner() == 0);                // the rematch starts at round 1
    Match slow; slow.Start(Cfg(Format::Duel, 3, 0, 7), {{1, kNoTeam, true}, {2, kNoTeam, true}}, 0);
    slow.Tick(Match::kCountdownMs);
    slow.OnDeath(2, 1, t + 100);
    CHECK(slow.RoundGapLeftMs(t + 100) == 7000);                                // a 7 s respawn delay is a 7 s gap
    CHECK(slow.Tick(t + 100 + 6999).respawn.empty() && slow.Tick(t + 100 + 7000).roundReset);
    Match three; three.Start(Cfg(Format::Duel, 3), {{1, kNoTeam, true}, {2, kNoTeam, true}, {3, kNoTeam, true}}, 0);
    three.Tick(Match::kCountdownMs);
    three.OnDeath(1, 2, t + 1); CHECK(three.Score(2) == 0 && three.Score(3) == 0);  // two still standing
    CHECK(three.RoundGapLeftMs(t + 1) == 0 && three.roundWinner() == 0);
    three.OnDeath(3, 2, t + 2); CHECK(three.Score(2) == 1 && three.roundWinner() == 2);   // last one standing
    Match solo; solo.Start(Cfg(Format::Duel, 3), {{1, kNoTeam, true}}, 0);
    solo.Tick(Match::kCountdownMs);
    solo.OnDeath(1, 0, t + 1);                                                  // nobody standing: a drawn round
    CHECK(solo.roundWinner() == 0 && solo.RoundGapLeftMs(t + 1) == 5000 && solo.Score(1) == 0);
    Match ffa; ffa.Start(Cfg(Format::FreeForAll, 5), {{1, kNoTeam, true}, {2, kNoTeam, true}}, 0);
    CHECK(ffa.round() == 0);                                                    // rounds are Duel's
    ffa.Tick(Match::kCountdownMs); ffa.OnDeath(2, 1, t + 1);
    CHECK(ffa.round() == 0 && ffa.RoundGapLeftMs(t + 2) == 0 && ffa.roundWinner() == 0);
}

// Hold G to give up (2026-10-25 request): a Duel round can be conceded; it scores as the forfeiter falling, without a
// kill for the opponent, and only while the round is being fought.
static void TestDuelForfeit() {
    Match m; m.Start(Cfg(Format::Duel, 2), {{1, kNoTeam, true}, {2, kNoTeam, true}}, 0);
    CHECK(!m.Forfeit(1, 100) && m.Alive(1));                                    // countdown: nothing to give up
    m.Tick(Match::kCountdownMs);
    uint32_t t = Match::kCountdownMs;
    CHECK(!m.Forfeit(9, t + 1));                                                // not in the match
    CHECK(m.Forfeit(1, t + 1));
    CHECK(!m.Alive(1) && m.Score(2) == 1 && m.Kills(2) == 0 && m.roundWinner() == 2 && m.RoundGapLeftMs(t + 1) == 5000);
    CHECK(m.Deaths(1) == 1 && m.phase() == MatchPhase::Live);
    CHECK(!m.Forfeit(2, t + 2) && m.Alive(2) && m.Score(1) == 0);               // round gap: already decided
    auto ev = m.Tick(t + 1 + 5000);
    CHECK(ev.roundReset && ev.respawn == std::vector<uint8_t>({1, 2}) && m.Alive(1));   // the forfeiter comes back
    CHECK(m.Forfeit(1, t + 6000) && m.phase() == MatchPhase::Results && m.winner() == 2);   // and can concede the match
    CHECK(!m.Forfeit(2, t + 6001));                                             // results: no round to give up
    Match three; three.Start(Cfg(Format::Duel, 3), {{1, kNoTeam, true}, {2, kNoTeam, true}, {3, kNoTeam, true}}, 0);
    three.Tick(Match::kCountdownMs);
    CHECK(three.Forfeit(3, t + 1) && three.roundWinner() == 0);                 // two still standing: the round goes on
    CHECK(!three.Forfeit(3, t + 2));                                            // already out of this round
    CHECK(three.Forfeit(2, t + 3) && three.roundWinner() == 1 && three.Score(1) == 1);
    Match ffa; ffa.Start(Cfg(Format::FreeForAll, 5), {{1, kNoTeam, true}, {2, kNoTeam, true}}, 0);
    ffa.Tick(Match::kCountdownMs);
    CHECK(!ffa.Forfeit(1, t + 1) && ffa.Alive(1) && ffa.Score(1) == 0 && ffa.Score(2) == 0);   // Duel only
}

static void TestGiveUpHold() {
    GiveUpHold h;
    CHECK(!h.Update(false, 0) && h.progress() == 0.0f);
    CHECK(!h.Update(true, 1000) && h.progress() == 0.0f);                       // the hold starts
    CHECK(!h.Update(true, 1750) && h.progress() > 0.49f && h.progress() < 0.51f);
    CHECK(!h.Update(true, 1000 + GiveUpHold::kHoldMs - 1) && h.progress() < 1.0f);
    CHECK(h.Update(true, 1000 + GiveUpHold::kHoldMs));                          // fires once at the full hold
    CHECK(h.progress() == 0.0f);
    CHECK(!h.Update(true, 5000) && !h.Update(true, 9000) && h.progress() == 0.0f);   // keeps holding: no repeat
    CHECK(!h.Update(false, 9001));
    CHECK(!h.Update(true, 9002) && !h.Update(true, 9002 + GiveUpHold::kHoldMs - 1));
    CHECK(!h.Update(false, 9002 + GiveUpHold::kHoldMs) && h.progress() == 0.0f);   // let go just short: cancelled
    CHECK(!h.Update(true, 20000) && h.Update(true, 20000 + GiveUpHold::kHoldMs + 500));   // a late frame still fires
    GiveUpHold w;
    CHECK(!w.Update(true, 0xFFFFFF00u) && w.Update(true, 0xFFFFFF00u + GiveUpHold::kHoldMs));   // clock wrap
}

static void TestLoadoutsAhead() {
    MatchConfig c = Cfg(Format::Duel, 2); c.gear = Gear::RandomTier;
    Match m; m.Start(c, {{1, kNoTeam, true}, {2, kNoTeam, true}}, 0);
    CHECK(m.LoadoutsAhead() == 0);                                              // countdown
    m.Tick(Match::kCountdownMs);
    uint32_t t = Match::kCountdownMs;
    CHECK(m.LoadoutsAhead() == 0);                                              // fighting: never
    m.OnDeath(1, 2, t + 1);
    CHECK(m.LoadoutsAhead() == 2);                                              // the round gap: both come back
    m.Tick(t + 1 + 5000);
    CHECK(m.LoadoutsAhead() == 0);
    m.OnDeath(1, 2, t + 6000);
    CHECK(m.phase() == MatchPhase::Results && m.LoadoutsAhead() == 2);          // the rematch regears everyone
    MatchConfig mc = c; mc.gear = Gear::Mirror;
    Match mirror; mirror.Start(mc, {{1, kNoTeam, true}, {2, kNoTeam, true}}, 0);
    mirror.Tick(Match::kCountdownMs);
    mirror.OnDeath(2, 1, t + 1);
    CHECK(mirror.LoadoutsAhead() == 1);                                         // one loadout shared by all
    MatchConfig oc = c; oc.gear = Gear::Own;
    Match own; own.Start(oc, {{1, kNoTeam, true}, {2, kNoTeam, true}}, 0);
    own.Tick(Match::kCountdownMs);
    own.OnDeath(2, 1, t + 1);
    CHECK(own.LoadoutsAhead() == 0);                                            // own gear: nothing generated
    MatchConfig fc = c; fc.format = Format::FreeForAll;
    Match ffa; ffa.Start(fc, {{1, kNoTeam, true}, {2, kNoTeam, true}}, 0);
    ffa.Tick(Match::kCountdownMs);
    ffa.OnDeath(2, 1, t + 1);
    CHECK(ffa.LoadoutsAhead() == 1);                                            // the fallen one's next loadout, while it waits
    ffa.MarkRespawned(2);
    CHECK(ffa.LoadoutsAhead() == 0);                                            // everyone fights: nothing ahead
    MatchConfig fo = fc; fo.gear = Gear::Own;
    Match ffaOwn; ffaOwn.Start(fo, {{1, kNoTeam, true}, {2, kNoTeam, true}}, 0);
    ffaOwn.Tick(Match::kCountdownMs);
    ffaOwn.OnDeath(2, 1, t + 1);
    CHECK(ffaOwn.LoadoutsAhead() == 0);                                         // own gear: nothing generated
}

static void TestStartLoadoutsAhead() {
    MatchConfig c = Cfg(Format::Duel, 2); c.gear = Gear::RandomTier;
    CHECK(StartLoadoutsAhead(c, 2) == 2);                                       // one each, made while they come in
    CHECK(StartLoadoutsAhead(c, 0) == 0);
    MatchConfig mc = c; mc.gear = Gear::Mirror; mc.format = Format::FreeForAll;
    CHECK(StartLoadoutsAhead(mc, 3) == 1);                                      // one shared by all
    CHECK(StartLoadoutsAhead(mc, 0) == 0);                                      // nobody to wear it
    MatchConfig oc = c; oc.gear = Gear::Own;
    CHECK(StartLoadoutsAhead(oc, 2) == 0);                                      // own gear: nothing generated
    MatchConfig bc = c; bc.format = Format::PlayersVsBots;
    CHECK(StartLoadoutsAhead(bc, 2) == 0);                                      // gear is a PvP option
    MatchConfig tc = c; tc.format = Format::TwoTeams;
    CHECK(StartLoadoutsAhead(tc, 4) == 4);
}

static void TestGearPool() {
    GearPool p;
    CHECK(p.Wants(4, 2) && p.size() == 0 && p.Take(4).empty());
    p.Put(4, {1});
    p.Put(4, {});                                                               // a failed generation
    CHECK(p.size() == 1 && p.Wants(4, 2));
    p.Put(4, {2});
    CHECK(!p.Wants(4, 2) && p.Wants(4, 3) && !p.Wants(4, 0));
    CHECK(p.Take(4) == std::vector<uint8_t>({1}) && p.Take(4) == std::vector<uint8_t>({2}) && p.Take(4).empty());
    p.Put(4, {3});
    CHECK(p.Take(5).empty() && p.size() == 0);                                  // another tier empties it
    p.Put(5, {5});
    CHECK(p.Wants(6, 1) && p.size() == 0);
    p.Put(6, {6});
    p.Put(7, {7});
    CHECK(p.size() == 1 && p.Take(7) == std::vector<uint8_t>({7}));
}

// Spec revision (user, 2026-10-25): what clears the dropped weapons and loose armour differs per format. Duel: each new
// round (as before). Free-for-all and teams: each rematch. The bot formats, fought in waves with loot from the fallen
// bots: a minute after each piece was dropped (session/hsm_lease.h IdleDespawnDue, LooseTimer).
static void TestDropClearFor() {
    CHECK(DropClearFor(Format::Duel) == DropClear::EachRound);
    CHECK(DropClearFor(Format::FreeForAll) == DropClear::EachMatch);
    CHECK(DropClearFor(Format::TwoTeams) == DropClear::EachMatch && DropClearFor(Format::ThreeTeams) == DropClear::EachMatch);
    CHECK(DropClearFor(Format::PlayersVsBots) == DropClear::AfterIdle && DropClearFor(Format::Zombies) == DropClear::AfterIdle);
}

// The moment each machine clears: Duel's round moving on from one already seen (the next round, or the rematch's round
// 1); free-for-all and teams' rematch (the results giving way to the next countdown). Never in the bot formats. Another
// format or round 0 forgets what was seen.
static void TestDropClearWatch() {
    const uint8_t live = uint8_t(MatchPhase::Live), results = uint8_t(MatchPhase::Results), countdown = uint8_t(MatchPhase::Countdown);
    DropClearWatch w;
    CHECK(!w.Observe(Format::Duel, live, 1));                                   // the first round seen: nothing before it
    CHECK(!w.Observe(Format::Duel, live, 1));
    CHECK(w.Observe(Format::Duel, live, 2) && !w.Observe(Format::Duel, live, 2));   // once per new round
    CHECK(w.Observe(Format::Duel, live, 3));
    CHECK(w.Observe(Format::Duel, countdown, 1));                               // the rematch
    CHECK(!w.Observe(Format::FreeForAll, live, 0) && !w.Observe(Format::FreeForAll, live, 2));
    CHECK(!w.Observe(Format::Duel, live, 2));                                   // another format forgot the round
    CHECK(!w.Observe(Format::Duel, live, 0) && !w.Observe(Format::Duel, live, 3));   // so does round 0 (no match)
    CHECK(w.Observe(Format::Duel, live, 4));
    DropClearWatch f;
    CHECK(!f.Observe(Format::FreeForAll, countdown, 0) && !f.Observe(Format::FreeForAll, live, 0));   // the first match
    CHECK(!f.Observe(Format::FreeForAll, results, 0) && !f.Observe(Format::FreeForAll, results, 0));
    CHECK(f.Observe(Format::FreeForAll, countdown, 0));                         // the rematch: once
    CHECK(!f.Observe(Format::FreeForAll, countdown, 0) && !f.Observe(Format::FreeForAll, live, 0));
    CHECK(!f.Observe(Format::FreeForAll, results, 0) && f.Observe(Format::FreeForAll, live, 0));   // a rematch seen late
    CHECK(!f.Observe(Format::TwoTeams, live, 0) && !f.Observe(Format::TwoTeams, countdown, 0));     // a new format: first seen
    CHECK(!f.Observe(Format::TwoTeams, results, 0) && !f.Observe(Format::ThreeTeams, countdown, 0));   // and again
    CHECK(!f.Observe(Format::ThreeTeams, results, 0) && f.Observe(Format::ThreeTeams, countdown, 0));
    DropClearWatch b;
    for (Format g : {Format::PlayersVsBots, Format::Zombies}) {
        CHECK(!b.Observe(g, live, 0) && !b.Observe(g, results, 0) && !b.Observe(g, countdown, 0) && !b.Observe(g, live, 0));
    }
}

// The bot formats' break between waves. Survival's is a minute of looting the fallen bots (Zombies keep 10 s). Every
// player may vote to skip it (held G); when all of them have, the next wave starts on the next Tick.
// Progression co-op, test 2026-10-25: both players lay downed but alive with the stock give-up screen suppressed, so
// the fight could never end. Holding G gives the fight up; when every human has, the bots win.
static void TestGiveUpFight() {
    uint32_t t = 0;
    Match m = LiveSurvival(Format::PlayersVsBots, t);
    m.AddFighter({7, kBotTeam, false});
    CHECK(!m.GiveUpFight(7, t + 1) && m.Alive(7));                              // a bot cannot
    CHECK(!m.GiveUpFight(9, t + 1));                                            // not in the match
    CHECK(m.GiveUpFight(1, t + 1) && !m.Alive(1) && m.phase() == MatchPhase::Live);
    CHECK(!m.GiveUpFight(1, t + 2));                                            // already down
    CHECK(m.GiveUpFight(2, t + 3) && m.phase() == MatchPhase::Results && m.winner() == kBotTeam);
    CHECK(!m.GiveUpFight(2, t + 4));                                            // results: nothing to give up
    Match duel; duel.Start(Cfg(Format::Duel, 2), {{1, kNoTeam, true}, {2, kNoTeam, true}}, 0);
    duel.Tick(Match::kCountdownMs);
    CHECK(!duel.GiveUpFight(1, Match::kCountdownMs + 1) && duel.Alive(1));      // PvP: Forfeit's rules instead
}

static void TestWaveBreakSkip() {
    CHECK(Match::kWaveBreakMs == 60000 && Match::kZombieBreakMs == 10000);
    uint32_t t = 0;
    Match m = LiveSurvival(Format::PlayersVsBots, t);
    const int n = m.botsLeft();
    CHECK(m.SkipVotesNeeded() == 2 && m.SkipVotes() == 0 && m.BreakLeftMs(t) == 0);
    CHECK(!m.VoteSkip(1, t));                                                  // not in a break: no vote
    for (int i = 0; i < n; ++i) { m.Tick(t + 1); m.AddFighter({uint8_t(7 + i), kBotTeam, false}); }
    for (int i = 0; i < n; ++i) m.OnDeath(uint8_t(7 + i), 1, t + 10);
    CHECK(m.Tick(t + 11).waveCleared && m.BreakLeftMs(t + 11) == Match::kWaveBreakMs);
    CHECK(m.BreakLeftMs(t + 11 + 15000) == Match::kWaveBreakMs - 15000);
    CHECK(m.VoteSkip(1, t + 12) && m.SkipVotes() == 1);
    CHECK(!m.VoteSkip(1, t + 13) && m.SkipVotes() == 1);                         // once per player
    CHECK(!m.VoteSkip(7, t + 13) && !m.VoteSkip(99, t + 13));                    // bots and strangers do not vote
    CHECK(!m.Tick(t + 14).waveStarted);                                          // one of two: the break goes on
    CHECK(m.VoteSkip(2, t + 15) && m.SkipVotes() == 2);
    auto ev = m.Tick(t + 16);
    CHECK(ev.waveStarted && m.SkipVotes() == 0 && m.BreakLeftMs(t + 16) == 0);  // everyone: the next wave now
    CHECK(!m.VoteSkip(1, t + 17));                                              // fighting again
    // A player who left does not hold the break up.
    for (int i = 0; i < m.botsLeft(); ++i) { m.Tick(t + 20); m.AddFighter({uint8_t(30 + i), kBotTeam, false}); }
    for (int i = 0; i < 30; ++i) if (m.Has(uint8_t(30 + i))) m.OnDeath(uint8_t(30 + i), 2, t + 30);
    CHECK(m.Tick(t + 31).waveCleared);
    CHECK(m.VoteSkip(1, t + 32) && !m.Tick(t + 33).waveStarted);
    m.RemoveFighter(2);
    CHECK(m.SkipVotesNeeded() == 1 && m.Tick(t + 34).waveStarted);
    // Zombies' short break; no votes outside the bot formats.
    uint32_t z = 0;
    Match zm = LiveSurvival(Format::Zombies, z);
    CHECK(zm.BreakLeftMs(z) == 0);
    Match d; d.Start(Cfg(Format::Duel, 3), {{1, kNoTeam, true}, {2, kNoTeam, true}}, 0); d.Tick(Match::kCountdownMs);
    CHECK(!d.VoteSkip(1, Match::kCountdownMs + 1) && d.BreakLeftMs(Match::kCountdownMs + 1) == 0);
    Match f; f.Start(Cfg(Format::FreeForAll, 3), {{1, kNoTeam, true}, {2, kNoTeam, true}}, 0); f.Tick(Match::kCountdownMs);
    CHECK(!f.VoteSkip(1, Match::kCountdownMs + 1));
}

// Lobby settings: the wave break (bot formats) and the ground-clear timer (free-for-all, teams, Brawl).
static void TestWaveBreakAndClearSettings() {
    MatchConfig c = Cfg(Format::PlayersVsBots, 10);
    CHECK(Sanitize(c).waveBreakS == 0);                                          // default: the format's own
    c.waveBreakS = 30; CHECK(Sanitize(c).waveBreakS == 30);
    c.waveBreakS = 45; CHECK(Sanitize(c).waveBreakS == 0);                       // not an option
    c = Cfg(Format::TwoTeams, 10); c.waveBreakS = 30; c.clearGroundMin = 3;
    CHECK(Sanitize(c).waveBreakS == 0 && Sanitize(c).clearGroundMin == 3);       // wave break: bot formats only
    c.clearGroundMin = 4; CHECK(Sanitize(c).clearGroundMin == 0);
    c = Cfg(Format::Duel, 3); c.clearGroundMin = 5; CHECK(Sanitize(c).clearGroundMin == 0);
    c = Cfg(Format::Zombies, 10); c.clearGroundMin = 5; CHECK(Sanitize(c).clearGroundMin == 0);
    c = Cfg(Format::FreeForAll, 5); c.gear = Gear::Fists; c.clearGroundMin = 2; CHECK(Sanitize(c).clearGroundMin == 2);   // Brawl
    // The break the match runs: 60 s Survival, 10 s Zombies, or the pick.
    for (Format f : {Format::PlayersVsBots, Format::Zombies}) {
        for (uint16_t s : {0, 10, 30, 60, 120}) {
            MatchConfig b = Cfg(f, 10); b.tier = 2; b.waveBreakS = s;
            Match m; m.Start(Sanitize(b), Two(), 0);
            m.Tick(Match::kCountdownMs);
            const uint32_t t = Match::kCountdownMs;
            for (uint8_t e = 7; m.botsLeft() > 0 && e < 200; ++e) { m.Tick(t + 1); m.AddFighter({e, kBotTeam, false}); m.OnDeath(e, 1, t + 10); }
            CHECK(m.Tick(t + 11).waveCleared);
            const uint32_t want = s ? s * 1000u : f == Format::Zombies ? Match::kZombieBreakMs : Match::kWaveBreakMs;
            CHECK(m.BreakLeftMs(t + 11) == want);
        }
    }
}

// A fallen Survival bot sheds a couple of armour pieces (LootPicks: which of its worn pieces), besides its weapons.
static void TestLootPicks() {
    CHECK(kLootArmorPieces == 2 && Match::kLootDelayMs == 2000);
    CHECK(LootPicks(0, 2, 5).empty());
    CHECK(LootPicks(1, 2, 5) == std::vector<size_t>({0}));
    for (uint32_t seed = 0; seed < 64; ++seed) {
        std::vector<size_t> p = LootPicks(7, 2, seed);
        CHECK(p.size() == 2 && p[0] < 7 && p[1] < 7 && p[0] != p[1]);
        CHECK(LootPicks(7, 2, seed) == p);                                      // the same seed, the same picks
    }
    std::set<size_t> firsts;
    for (uint32_t seed = 0; seed < 64; ++seed) firsts.insert(LootPicks(7, 2, seed)[0]);
    CHECK(firsts.size() >= 5);                                                  // not always the same piece
    CHECK(LootPicks(3, 5, 1).size() == 3);
}

// Sudden death ends on the next deciding event, as the HUD says (NEXT ROUND WINS, NEXT POINT WINS): in Duel the next
// round that leaves one leader, in the bot formats the next wave cleared. Neither waits for the score limit.
static void TestSuddenDeathEnds() {
    Match d; d.Start(Cfg(Format::Duel, 5, 60), {{1, kNoTeam, true}, {2, kNoTeam, true}, {3, kNoTeam, true}}, 0);
    d.Tick(Match::kCountdownMs);
    uint32_t t = Match::kCountdownMs;
    d.OnDeath(2, 1, t + 100); d.OnDeath(3, 1, t + 200);                     // round 1 to fighter 1
    CHECK(d.Tick(t + 200 + 5000).roundReset);
    d.OnDeath(1, 2, t + 6000); d.OnDeath(3, 2, t + 6100);                   // round 2 to fighter 2: 1 : 1 : 0
    CHECK(d.Tick(t + 6100 + 5000).roundReset && d.round() == 3);
    auto ev = d.Tick(t + 60000);
    CHECK(d.phase() == MatchPhase::SuddenDeath && !ev.ended);                 // tied at the time limit
    d.OnDeath(1, 3, t + 61000); d.OnDeath(2, 3, t + 61100);                 // round 3 to fighter 3: 1 : 1 : 1, still tied
    CHECK(d.phase() == MatchPhase::SuddenDeath && d.Score(3) == 1 && d.RoundGapLeftMs(t + 61100) == 5000);
    CHECK(d.Tick(t + 61100 + 5000).roundReset && d.round() == 4);
    d.OnDeath(2, 1, t + 70000);
    CHECK(d.phase() == MatchPhase::SuddenDeath);                              // two still standing: no round yet
    d.OnDeath(3, 1, t + 70100);                                              // round 4 to fighter 1: 2 : 1 : 1
    CHECK(d.phase() == MatchPhase::Results && d.winner() == 1 && d.Score(1) == 2);   // far from the limit of 5
    CHECK(d.RoundGapLeftMs(t + 70100) == 0 && !d.Tick(t + 70100 + 5000).roundReset);
    Match two; two.Start(Cfg(Format::Duel, 5, 30), {{1, kNoTeam, true}, {2, kNoTeam, true}}, 0);
    two.Tick(t);
    two.OnDeath(2, 1, t + 100);
    CHECK(two.phase() == MatchPhase::Live && two.Score(1) == 1);             // before the limit a round is just a round
    CHECK(two.Tick(t + 100 + 5000).roundReset);
    two.OnDeath(1, 2, t + 6000); two.Tick(t + 6000 + 5000);
    two.Tick(t + 30000);
    CHECK(two.phase() == MatchPhase::SuddenDeath);
    two.OnDeath(1, 2, t + 31000);
    CHECK(two.phase() == MatchPhase::Results && two.winner() == 2 && two.Score(2) == 2);

    MatchConfig c = Cfg(Format::PlayersVsBots, 10, 60); c.tier = 2;
    Match b; b.Start(c, Two(), 0);
    b.Tick(t);
    Wave w0 = WaveFor(Format::PlayersVsBots, 0, 2, 2);
    for (int i = 0; i < w0.count; ++i) { b.Tick(t + 1); b.AddFighter({uint8_t(7 + i), kBotTeam, false}); }
    ev = b.Tick(t + 60000);
    CHECK(b.phase() == MatchPhase::SuddenDeath && !ev.ended);                 // no wave cleared by the time limit
    b.OnDeath(1, 7, t + 60001);
    CHECK(b.phase() == MatchPhase::SuddenDeath);                              // one player down, one still up
    for (int i = 0; i < w0.count - 1; ++i) b.OnDeath(uint8_t(7 + i), 2, t + 60010);
    CHECK(!b.Tick(t + 60011).ended && b.phase() == MatchPhase::SuddenDeath);  // a bot still up
    b.OnDeath(uint8_t(7 + w0.count - 1), 2, t + 60020);
    ev = b.Tick(t + 60021);
    CHECK(ev.waveCleared && ev.ended && b.phase() == MatchPhase::Results && b.winner() == kHumanTeam);
    CHECK(ev.respawn.empty() && ev.regear == Regear::None && b.Score(kHumanTeam) == 1);   // over: nobody returns
    CHECK(!b.Tick(t + 60022).ended);                                          // reported once
    Match held; held.Start(c, Two(), 0);
    held.Tick(t);
    for (int i = 0; i < w0.count; ++i) { held.Tick(t + 1); held.AddFighter({uint8_t(7 + i), kBotTeam, false}); }
    for (int i = 0; i < w0.count; ++i) held.OnDeath(uint8_t(7 + i), 1, t + 10);
    ev = held.Tick(t + 11);
    CHECK(ev.waveCleared && !ev.ended && held.phase() == MatchPhase::Live);   // before the limit a wave is just a wave
    held.Tick(t + 60000);
    CHECK(held.phase() == MatchPhase::Results && held.winner() == kHumanTeam); // a wave cleared by the limit: players win
    Match lost; lost.Start(c, Two(), 0);
    lost.Tick(t); lost.Tick(t + 60000);
    CHECK(lost.phase() == MatchPhase::SuddenDeath);
    lost.OnDeath(1, 0, t + 60100); lost.OnDeath(2, 0, t + 60200);             // everyone down in sudden death: bots win
    CHECK(lost.phase() == MatchPhase::Results && lost.winner() == kBotTeam);
}

// Rounds plan 1.1: which respawns bring a new loadout. Own gear and the bot formats never do.
static void TestRegear() {
    using C = RespawnCause;
    auto R = [](Format f, Gear g, C cause) { MatchConfig c = Cfg(f, 5); c.gear = g; return RegearFor(c, cause); };
    int cells = 0;
    for (Format f : {Format::FreeForAll, Format::TwoTeams, Format::ThreeTeams, Format::Duel, Format::PlayersVsBots, Format::Zombies})
        for (C cause : {C::Individual, C::RoundReset, C::WaveCleared, C::Rematch}) {
            ++cells;
            CHECK(R(f, Gear::Own, cause) == Regear::None);
            if (IsBotFormat(f)) { CHECK(R(f, Gear::RandomTier, cause) == Regear::None && R(f, Gear::Mirror, cause) == Regear::None); continue; }
            CHECK(R(f, Gear::RandomTier, cause) == Regear::Each);                // every respawning fighter, every time
            bool together = cause == C::RoundReset || cause == C::Rematch;      // everybody reset at once
            CHECK(R(f, Gear::Mirror, cause) == (together ? Regear::Shared : Regear::None));
        }
    CHECK(cells == 24);
    CHECK(R(Format::Duel, Gear::Mirror, C::RoundReset) == Regear::Shared && R(Format::Duel, Gear::RandomTier, C::RoundReset) == Regear::Each);
    CHECK(R(Format::FreeForAll, Gear::RandomTier, C::Individual) == Regear::Each && R(Format::FreeForAll, Gear::Mirror, C::Individual) == Regear::None);
    CHECK(R(Format::TwoTeams, Gear::Mirror, C::Rematch) == Regear::Shared);
    CHECK(R(Format::Duel, Gear(7), C::RoundReset) == Regear::None);             // an unknown gear setting is Own
}

// Tick says when the respawns it orders bring a new loadout: once per reset or death, never for Own or bot formats.
static void TestRegearEvents() {
    std::vector<FighterInfo> two = {{1, kNoTeam, true}, {2, kNoTeam, true}};
    uint32_t t = Match::kCountdownMs;
    for (Gear g : {Gear::Own, Gear::RandomTier, Gear::Mirror}) {
        MatchConfig c = Cfg(Format::Duel, 5); c.gear = g;
        Match m; m.Start(c, two, 0);
        auto ev = m.Tick(t);
        CHECK(ev.live && ev.regear == Regear::None);                            // the match start gears everyone itself
        m.OnDeath(2, 1, t + 100);
        CHECK(m.Tick(t + 200).regear == Regear::None);
        ev = m.Tick(t + 100 + 5000);
        Regear want = g == Gear::Own ? Regear::None : g == Gear::Mirror ? Regear::Shared : Regear::Each;
        CHECK(ev.roundReset && ev.respawn.size() == 2 && ev.regear == want);
        ev = m.Tick(t + 100 + 5001);
        CHECK(ev.respawn.empty() && ev.regear == Regear::None);                 // exactly once per reset
    }
    MatchConfig f = Cfg(Format::FreeForAll, 2, 0, 5); f.gear = Gear::RandomTier;
    Match a; a.Start(f, two, 0); a.Tick(t);
    a.OnDeath(2, 1, t + 10);
    CHECK(a.Tick(t + 10 + 4999).regear == Regear::None);
    auto ev = a.Tick(t + 10 + 5000);
    CHECK(ev.respawn == std::vector<uint8_t>({2}) && ev.regear == Regear::Each);   // the victim alone, with new gear
    a.MarkRespawned(2);
    ev = a.Tick(t + 10 + 5001);
    CHECK(ev.respawn.empty() && ev.regear == Regear::None);
    f.gear = Gear::Mirror;
    Match b; b.Start(f, two, 0); b.Tick(t);
    b.OnDeath(2, 1, t + 10);
    ev = b.Tick(t + 10 + 5000);
    CHECK(ev.respawn.size() == 1 && ev.regear == Regear::None);                // keeps the shared loadout
    b.MarkRespawned(2);
    b.OnDeath(2, 1, t + 6000);
    CHECK(b.phase() == MatchPhase::Results);
    ev = b.Tick(t + 6001);
    CHECK(ev.ended && ev.regear == Regear::None);
    ev = b.Tick(t + 6000 + Match::kResultsMs);
    CHECK(ev.rematch && ev.respawn.empty() && ev.regear == Regear::Shared);     // a new shared loadout at the rematch
    CHECK(b.Tick(t + 6001 + Match::kResultsMs).regear == Regear::None);
    f.gear = Gear::Own;
    Match o; o.Start(f, two, 0); o.Tick(t);
    o.OnDeath(2, 1, t + 10); o.OnDeath(2, 1, t + 11);
    CHECK(o.Tick(t + 10 + 5000).regear == Regear::None);
    // Survival with a gear setting left over from a PvP lobby: the downed come back at wave clear in their own gear.
    MatchConfig s = Cfg(Format::PlayersVsBots, 10); s.gear = Gear::RandomTier;
    Match v; v.Start(s, {{1, kHumanTeam, true}, {2, kHumanTeam, true}}, 0);
    CHECK(v.Tick(t).waveStarted);
    CHECK(v.Tick(t + 1).spawnBots == 1); v.AddFighter({7, kBotTeam, false});
    CHECK(v.Tick(t + 2).spawnBots == 1); v.AddFighter({8, kBotTeam, false});
    v.OnDeath(1, 7, t + 3);
    v.OnDeath(7, 2, t + 4); v.OnDeath(8, 2, t + 5);
    ev = v.Tick(t + 6);
    CHECK(ev.waveCleared && ev.respawn == std::vector<uint8_t>({1}) && ev.regear == Regear::None);
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

// Two-player test 2026-10-25: rounds with both fighters down and unable to act are drawn (hsm_draw.h): the round ends
// with no point and no death, and after the gap both respawn as for any round.
static void TestDuelDraw() {
    Match m; m.Start(Cfg(Format::Duel, 2), {{1, kNoTeam, true}, {2, kNoTeam, true}}, 0);
    CHECK(!m.RoundInPlay() && !m.Draw(100));                                    // countdown
    m.Tick(Match::kCountdownMs);
    uint32_t t = Match::kCountdownMs;
    CHECK(m.RoundInPlay() && m.round() == 1);
    CHECK(m.Draw(t + 10));
    CHECK(!m.RoundInPlay() && m.roundWinner() == 0 && m.Score(1) == 0 && m.Score(2) == 0 && m.Alive(1) && m.Alive(2));
    CHECK(m.Deaths(1) == 0 && m.Deaths(2) == 0 && m.RoundGapLeftMs(t + 10) == 5000 && m.phase() == MatchPhase::Live);
    CHECK(!m.Draw(t + 20) && !m.Forfeit(1, t + 20));                            // the gap: decided
    auto ev = m.Tick(t + 10 + 5000);
    CHECK(ev.roundReset && ev.respawn == std::vector<uint8_t>({1, 2}) && m.round() == 2 && m.RoundInPlay());
    m.OnDeath(2, 1, t + 6000);
    CHECK(!m.RoundInPlay() && !m.Draw(t + 6001) && m.Score(1) == 1);            // a won round is not drawn after
    Match ffa; ffa.Start(Cfg(Format::FreeForAll, 5), {{1, kNoTeam, true}, {2, kNoTeam, true}}, 0);
    ffa.Tick(Match::kCountdownMs);
    CHECK(!ffa.RoundInPlay() && !ffa.Draw(t + 1));                              // Duel only
}

// Brawl: free-for-all with gear = Fists. Only free-for-all can be one, it is tier 0, it never regears (the bodies are
// stripped at the spawn), and it is scored, respawned and ended like free-for-all.
static void TestBrawl() {
    MatchConfig b = Cfg(Format::FreeForAll, 3); b.gear = Gear::Fists; b.tier = 5;
    CHECK(IsBrawl(b) && IsBrawl(Format::FreeForAll, Gear::Fists) && !IsBrawl(Format::FreeForAll, Gear::Own));
    MatchConfig s = Sanitize(b);
    CHECK(s.gear == Gear::Fists && s.tier == 0 && s.format == Format::FreeForAll);   // tier 0: the lowest build and budget
    for (Format f : {Format::Duel, Format::TwoTeams, Format::ThreeTeams, Format::PlayersVsBots, Format::Zombies}) {
        MatchConfig o = b; o.format = f; o.tier = 4;
        CHECK(Sanitize(o).gear == Gear::Own && Sanitize(o).tier == 4 && !IsBrawl(Sanitize(o)));   // only free-for-all is Brawl
    }
    MatchConfig own = Cfg(Format::FreeForAll, 3); own.tier = 5;
    CHECK(Sanitize(own).tier == 5 && Sanitize(own).gear == Gear::Own);                // the tier is only forced for Brawl
    for (RespawnCause cause : {RespawnCause::Individual, RespawnCause::RoundReset, RespawnCause::WaveCleared, RespawnCause::Rematch})
        CHECK(RegearFor(s, cause) == Regear::None);                                   // no generated loadout: not own gear's blob either
    CHECK(DropClearFor(s.format) == DropClear::EachMatch);                            // the free-for-all rule: the drops go at the rematch
    CHECK(MuscleFor(0.1, s.tier) == MuscleForTier(0) && MuscleFor(0.9, s.tier) == 0.9);   // tier 0 raises nobody's build, lowers none

    Match m; m.Start(s, {{1, kNoTeam, true}, {2, kNoTeam, true}}, 0);
    m.Tick(Match::kCountdownMs);
    CHECK(m.config().gear == Gear::Fists && m.phase() == MatchPhase::Live);
    m.OnDeath(2, 1, 6000);
    CHECK(m.Score(1) == 1 && !m.Alive(2));
    auto ev = m.Tick(6000 + 5000 + 1);                                                // the respawn delay passes: back, and a fresh life
    CHECK(ev.respawn.size() == 1 && ev.respawn[0] == 2 && ev.regear == Regear::None);
    for (uint32_t t = 12000, k = 0; k < 2; ++k, t += 6000) { m.MarkRespawned(2); m.OnDeath(2, 1, t); m.Tick(t + 5001); }
    CHECK(m.phase() == MatchPhase::Results && m.winner() == 1);                       // first to the limit wins
}

int main() {
    TestBrawl();
    TestWaveBreakAndClearSettings();
    TestHitAllowed();
    TestZeroScoreBeatsNegative(); TestEndedSurvivesLateTick();
    TestSanitize(); TestResolveTeams(); TestColors(); TestKillLog(); TestTeamScoringAndLimit(); TestRespawnSchedule();
    TestTimeLimitAndSuddenDeath(); TestFreeForAll(); TestSurvivalWaves(); TestSurvivalAliveCap(); TestDuelRounds(); TestHasAndFinish(); TestWavePacing(); TestWaves(); TestSides();
    TestRegear(); TestRegearEvents(); TestSuddenDeathEnds(); TestDuelForfeit(); TestGiveUpFight(); TestDuelDraw(); TestGiveUpHold(); TestDropClearFor(); TestDropClearWatch(); TestWaveBreakSkip(); TestLootPicks(); TestLoadoutsAhead(); TestStartLoadoutsAhead(); TestGearPool();
    std::printf(g_fail ? "%d FAILED\n" : "all match tests passed\n", g_fail);
    return g_fail ? 1 : 0;
}
