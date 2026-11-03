// Capture the flag: the rule engine (hsm_ctf.h), the score/limit/sudden death through Match::AddCapture, the bases from the
// spawn arcs, and the text.
#include "match/hsm_ctf.h"
#include "session/hsm_migration.h"
#include <cstdio>
#include <string>

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++g_fail; } } while (0)
using namespace hsm;

namespace {
const Point3 kBase0{-2000, 0, 100}, kBase1{2000, 0, 100};
constexpr uint32_t kReturn = 20000;

CtfFighter F(uint8_t e, int8_t team, double x, double y = 0, bool alive = true, bool down = false) {
    CtfFighter f; f.entity = e; f.team = team; f.at = {x, y, 100}; f.alive = alive; f.down = down; return f;
}
Ctf Fresh() { Ctf c; c.Start(kReturn, kBase0, kBase1); return c; }
bool Has(const std::vector<CtfEvent>& v, CtfEventKind k, uint8_t flag, uint8_t entity) {
    for (const CtfEvent& e : v) if (e.kind == k && e.flag == flag && e.entity == entity) return true;
    return false;
}
// Team 0 (entity 1) takes team 1's flag at its base.
void Take1(Ctf& c, uint32_t t) { c.Tick(t, {F(1, 0, 2000), F(2, 1, 0, 3000)}); }

void TestTake() {
    Ctf c = Fresh();
    CHECK(c.state(0) == ObjectiveState::Home && c.state(1) == ObjectiveState::Home && c.CarriedBy(1) == -1);
    CHECK(c.Tick(0, {F(1, 0, -1000), F(2, 1, 1000)}).empty());                // nobody near a flag
    auto ev = c.Tick(10, {F(1, 0, 2000 - 100), F(2, 1, 0, 3000)});             // enemy of flag 1 at its base
    CHECK(ev.size() == 1 && Has(ev, CtfEventKind::Taken, 1, 1));
    CHECK(c.state(1) == ObjectiveState::Carried && c.carrier(1) == 1 && c.CarriedBy(1) == 1 && c.state(0) == ObjectiveState::Home);
    // A fighter at its own flag takes nothing.
    Ctf d = Fresh();
    CHECK(d.Tick(0, {F(1, 0, -2000), F(2, 1, 2000)}).empty());                  // each at its own flag
    CHECK(d.state(0) == ObjectiveState::Home);
    // Out of reach: just past the touch radius, and too high above it.
    Ctf e = Fresh();
    CHECK(e.Tick(0, {F(1, 0, 2000 - kFlagTouchCm - 5)}).empty());
    CtfFighter high = F(1, 0, 2000); high.at.z = 100 + kFlagTouchZCm + 5;
    CHECK(e.Tick(0, {high}).empty());
    // The dead, the downed and teamless fighters take nothing.
    CHECK(e.Tick(0, {F(1, 0, 2000, 0, false)}).empty());
    CHECK(e.Tick(0, {F(1, 0, 2000, 0, true, true)}).empty());
    CHECK(e.Tick(0, {F(1, kNoTeam, 2000)}).empty());
    CHECK(e.state(1) == ObjectiveState::Home);
}

void TestDropOnDeathAndDown() {
    Ctf c = Fresh();
    Take1(c, 0);
    CHECK(c.CarriedBy(1) == 1);
    auto ev = c.Tick(100, {F(1, 0, 500, 40), F(2, 1, 0, 3000)});               // carrying along
    CHECK(ev.empty() && c.state(1) == ObjectiveState::Carried);
    ev = c.Tick(200, {F(1, 0, 600, 50, false), F(2, 1, 0, 3000)});             // dies
    CHECK(Has(ev, CtfEventKind::Dropped, 1, 1) && c.state(1) == ObjectiveState::Dropped);
    const auto o = c.Objectives(200);
    CHECK(o[1].state == ObjectiveState::Dropped && o[1].x == 600 && o[1].y == 50 && o[1].timerMs == kReturn && o[1].carrier == 0);
    // Downed: dropped after kDownDropMs, not at once; getting up in time keeps it.
    Ctf d = Fresh();
    Take1(d, 0);
    CHECK(d.Tick(100, {F(1, 0, 900, 0, true, true), F(2, 1, 0, 3000)}).empty());
    CHECK(d.Tick(100 + kDownDropMs - 1, {F(1, 0, 900, 0, true, true), F(2, 1, 0, 3000)}).empty());
    CHECK(d.Tick(100 + kDownDropMs - 1 + 10, {F(1, 0, 900), F(2, 1, 0, 3000)}).empty() && d.state(1) == ObjectiveState::Carried);   // up again
    CHECK(d.Tick(2000, {F(1, 0, 900, 0, true, true), F(2, 1, 0, 3000)}).empty());       // down again: the clock starts afresh
    auto ev2 = d.Tick(2000 + kDownDropMs, {F(1, 0, 910, 5, true, true), F(2, 1, 0, 3000)});
    CHECK(Has(ev2, CtfEventKind::Dropped, 1, 1) && d.Objectives(2500)[1].x == 910);
    // The dropper cannot take it straight back (guard), even once up again; after the guard it can.
    CHECK(d.Tick(2600, {F(1, 0, 910, 5), F(2, 1, 0, 3000)}).empty());
    CHECK(!d.Tick(2000 + kDownDropMs + kRetakeGuardMs, {F(1, 0, 910, 5), F(2, 1, 0, 3000)}).empty());
    CHECK(d.CarriedBy(1) == 1);
}

void TestEnemyTakesDropped() {
    Ctf c = Fresh();
    Take1(c, 0);
    c.Tick(100, {F(1, 0, 0, 0, false), F(2, 1, 0, 3000)});                     // dropped at the middle
    CHECK(c.state(1) == ObjectiveState::Dropped);
    auto ev = c.Tick(200, {F(2, 1, 0, 3000)});                                // far away: still there
    CHECK(ev.empty());
    ev = c.Tick(300, {F(3, 0, 50)});                                          // the other team's... (team 0 is an enemy of flag 1)
    CHECK(Has(ev, CtfEventKind::Taken, 1, 3) && c.carrier(1) == 3);
}

void TestOwnTeamReturns() {
    Ctf c = Fresh();
    Take1(c, 0);
    c.Tick(100, {F(1, 0, 0, 0, false), F(2, 1, 0, 3000)});                     // flag 1 (team 1's) lies at the middle
    auto ev = c.Tick(200, {F(2, 1, 40)});                                     // a teammate of team 1 touches it
    CHECK(Has(ev, CtfEventKind::Returned, 1, 2) && c.state(1) == ObjectiveState::Home);
    CHECK(c.Objectives(200)[1].x == kBase1.x && c.Objectives(200)[1].timerMs == 0);
    // Both an owner and an enemy on it in the same tick: the return comes first.
    Ctf d = Fresh();
    Take1(d, 0);
    d.Tick(100, {F(1, 0, 0, 0, false), F(2, 1, 0, 3000)});
    ev = d.Tick(5000, {F(3, 0, 20), F(2, 1, -20)});
    CHECK(Has(ev, CtfEventKind::Returned, 1, 2) && !Has(ev, CtfEventKind::Taken, 1, 3) && d.state(1) == ObjectiveState::Home);
    // The enemy standing on the base's flag at once takes it now it is home.
    ev = d.Tick(5010, {F(3, 0, 2000)});
    CHECK(Has(ev, CtfEventKind::Taken, 1, 3));
}

void TestAutoReturn() {
    Ctf c = Fresh();
    Take1(c, 1000);
    c.Tick(1100, {F(1, 0, 0, 0, false), F(2, 1, 0, 3000)});
    CHECK(c.LeftMs(1, 1100) == kReturn && c.LeftMs(1, 1100 + 14000) == kReturn - 14000 && c.LeftMs(0, 1100) == 0);
    CHECK(c.Tick(1100 + kReturn - 1, {F(2, 1, 0, 3000)}).empty() && c.state(1) == ObjectiveState::Dropped);
    auto ev = c.Tick(1100 + kReturn, {F(2, 1, 0, 3000)});
    CHECK(ev.size() == 1 && Has(ev, CtfEventKind::Expired, 1, 0) && c.state(1) == ObjectiveState::Home);
    CHECK(c.LeftMs(1, 1100 + kReturn) == 0);
    Ctf s; s.Start(10000, kBase0, kBase1);                                     // the lobby's shorter return time
    s.Tick(0, {F(1, 0, 2000)}); s.Tick(10, {F(1, 0, 0, 0, false)});
    CHECK(s.Tick(10 + 9999, {}).empty() && !s.Tick(10 + 10000, {}).empty());
}

void TestCapture() {
    Ctf c = Fresh();
    Take1(c, 0);
    CHECK(c.Tick(100, {F(1, 0, -1500), F(2, 1, 0, 3000)}).empty());            // on the way
    auto ev = c.Tick(200, {F(1, 0, -2000 + 100), F(2, 1, 0, 3000)});          // at its own base, own flag home
    CHECK(ev.size() == 1 && Has(ev, CtfEventKind::Captured, 1, 1));
    CHECK(c.state(1) == ObjectiveState::Home && c.CarriedBy(1) == -1);
    // The captured flag is back at its base: the carrier at its own base is not touching it (flag 1 is at base 1).
    CHECK(c.Tick(300, {F(1, 0, -2000)}).empty());
    // Blocked while the own flag is away; goes through as soon as it is home.
    Ctf d = Fresh();
    d.Tick(0, {F(1, 0, 2000), F(2, 1, -2000)});                                // both flags taken
    CHECK(d.CarriedBy(1) == 1 && d.CarriedBy(2) == 0 && d.CarriedBy(3) == -1);
    CHECK(d.carrier(0) == 2 && d.carrier(1) == 1);
    ev = d.Tick(100, {F(1, 0, -2000), F(2, 1, 2000)});                         // both at their own base, both flags away
    CHECK(ev.empty() && d.carrier(0) == 2 && d.carrier(1) == 1);
    ev = d.Tick(200, {F(1, 0, -2000), F(2, 1, 2000, 0, false)});              // 2 falls: flag 0 drops at base 1... far from base 0
    CHECK(Has(ev, CtfEventKind::Dropped, 0, 2) && d.state(0) == ObjectiveState::Dropped);
    CHECK(d.Tick(300, {F(1, 0, -2000)}).empty() && d.carrier(1) == 1);        // own flag still away: no capture
    ev = d.Tick(300 + kReturn, {F(1, 0, -2000)});                              // it goes home by itself: the capture follows in the same tick
    CHECK(Has(ev, CtfEventKind::Expired, 0, 0) && Has(ev, CtfEventKind::Captured, 1, 1));
    // A teammate bringing it home also frees the capture.
    Ctf e = Fresh();
    e.Tick(0, {F(1, 0, 2000), F(2, 1, -2000)});
    ev = e.Tick(10, {F(1, 0, -2000), F(2, 1, 2000, 0, false), F(3, 0, 2000)});   // 2 falls with flag 0 at base 1; teammate 3 of team 0 is there
    CHECK(Has(ev, CtfEventKind::Dropped, 0, 2) && Has(ev, CtfEventKind::Returned, 0, 3) && Has(ev, CtfEventKind::Captured, 1, 1) && ev.size() == 3);
    // A dead or enemy carrier near the base captures nothing; capture needs the base's team.
    Ctf f = Fresh();
    Take1(f, 0);
    CHECK(f.Tick(100, {F(1, 0, -2000, 0, false)}).size() == 1);               // dies at its own base: only a drop
    CHECK(f.state(1) == ObjectiveState::Dropped);
}

void TestTwoCarriers() {
    Ctf c = Fresh();
    c.Tick(0, {F(1, 0, 2000), F(2, 1, -2000), F(3, 0, -1900), F(4, 1, 1900)});
    CHECK(c.carrier(1) == 1 && c.carrier(0) == 2);
    CHECK(c.CarriedBy(1) == 1 && c.CarriedBy(2) == 0);
    auto o = c.Objectives(0);
    CHECK(o.size() == 2 && o[0].state == ObjectiveState::Carried && o[0].carrier == 2 && o[1].carrier == 1 && o[0].owner == 0 && o[1].owner == 1);
    // One carrier cannot carry two flags: a team-0 fighter holding flag 1 touching... only one enemy flag exists, so a fighter never holds two.
    CHECK(c.Tick(10, {F(1, 0, 2000), F(2, 1, -2000)}).empty());
}

void TestLeave() {
    Ctf c = Fresh();
    Take1(c, 0);
    c.Tick(100, {F(1, 0, 300, 70), F(2, 1, 0, 3000)});
    auto ev = c.Remove(1, 150);                                               // the carrier leaves the match
    CHECK(ev.size() == 1 && Has(ev, CtfEventKind::Dropped, 1, 1) && c.state(1) == ObjectiveState::Dropped);
    CHECK(c.Objectives(150)[1].x == 300 && c.Objectives(150)[1].y == 70);
    CHECK(c.Remove(1, 160).empty() && c.Remove(9, 160).empty());               // nothing carried any more / a stranger
    // Gone from the list without Remove (the slot was freed): the next tick drops it where it was last seen.
    Ctf d = Fresh();
    Take1(d, 0);
    d.Tick(100, {F(1, 0, 400, 20), F(2, 1, 0, 3000)});
    ev = d.Tick(200, {F(2, 1, 0, 3000)});
    CHECK(Has(ev, CtfEventKind::Dropped, 1, 1) && d.Objectives(200)[1].x == 400);
}

void TestResetAndMove() {
    Ctf c = Fresh();
    Take1(c, 0);
    c.Tick(100, {F(1, 0, 10, 0, false), F(2, 1, 0, 3000)});
    c.MoveDropped(1, {10, 0, 5});
    CHECK(c.Objectives(100)[1].z == 5);
    c.MoveDropped(0, {1, 2, 3});                                              // not dropped: ignored
    CHECK(c.Objectives(100)[0].x == kBase0.x);
    c.Reset();
    CHECK(c.state(0) == ObjectiveState::Home && c.state(1) == ObjectiveState::Home && c.Objectives(100)[1].x == kBase1.x);
    CHECK(c.Objectives(100)[1].timerMs == 0 && c.base(0).x == kBase0.x && c.returnMs() == kReturn);
}

void TestMatchScoring() {
    auto start = [](Match& m, uint16_t captures, uint16_t timeS) {
        MatchConfig cfg; cfg.format = Format::CaptureTheFlag; cfg.ctfCaptures = uint8_t(captures); cfg.timeLimitS = timeS;
        m.Start(cfg, {{1, 0, true, 0}, {2, 1, true, 0}, {3, 0, true, 0}}, 0);
        m.Tick(Match::kCountdownMs);
    };
    Match m; start(m, 3, 0);
    CHECK(m.config().scoreLimit == 3 && m.config().teams == 2 && m.phase() == MatchPhase::Live);
    CHECK(m.AddCapture(0, 6000) && m.Score(0) == 1 && m.phase() == MatchPhase::Live);
    m.OnDeath(2, 1, 6100);                                                    // a kill scores nothing
    CHECK(m.Score(0) == 1 && m.Score(1) == 0 && m.Kills(1) == 1 && m.Deaths(2) == 1 && !m.Alive(2));
    m.OnDeath(3, 3, 6100);                                                    // nor does a suicide cost anything
    CHECK(m.Score(0) == 1);
    CHECK(m.AddCapture(1, 7000) && m.AddCapture(0, 8000) && m.Score(0) == 2 && m.Score(1) == 1 && m.phase() == MatchPhase::Live);
    CHECK(m.AddCapture(0, 9000) && m.phase() == MatchPhase::Results && m.winner() == 0);   // first to 3
    CHECK(!m.AddCapture(1, 9100));                                            // over
    // The fallen come back after the respawn delay.
    Match r; start(r, 3, 0);
    r.OnDeath(2, 1, 6000);
    CHECK(r.Tick(6000 + 4000).respawn.empty());
    auto ev = r.Tick(6000 + 5000);
    CHECK(ev.respawn.size() == 1 && ev.respawn[0] == 2);
    // The host changes the limit: 1 capture wins at once.
    Match one; start(one, 1, 0);
    CHECK(one.AddCapture(1, 6000) && one.winner() == 1 && one.phase() == MatchPhase::Results);
    // Wrong places for a capture.
    Match ffa; MatchConfig fc; ffa.Start(fc, {{1, kNoTeam, true, 0}}, 0); ffa.Tick(Match::kCountdownMs);
    CHECK(!ffa.AddCapture(0, 6000));
    Match early; MatchConfig ec; ec.format = Format::CaptureTheFlag; early.Start(ec, {{1, 0, true, 0}, {2, 1, true, 0}}, 0);
    CHECK(!early.AddCapture(0, 100));                                         // countdown
    Match t3; start(t3, 3, 0);
    CHECK(!t3.AddCapture(2, 6000) && !t3.AddCapture(-1, 6000));
}

void TestTimeLimitAndSuddenDeath() {
    auto start = [](Match& m, uint16_t timeS) {
        MatchConfig cfg; cfg.format = Format::CaptureTheFlag; cfg.ctfCaptures = 3; cfg.timeLimitS = timeS;
        m.Start(cfg, {{1, 0, true, 0}, {2, 1, true, 0}}, 0);
        m.Tick(Match::kCountdownMs);
    };
    Match lead; start(lead, 60);
    lead.AddCapture(1, 10000);
    lead.Tick(Match::kCountdownMs + 60000);
    CHECK(lead.phase() == MatchPhase::Results && lead.winner() == 1);          // the time ran out: the team ahead wins
    Match tie; start(tie, 60);
    tie.Tick(Match::kCountdownMs + 60000);
    CHECK(tie.phase() == MatchPhase::SuddenDeath && tie.winner() == Match::kNoWinner);   // 0-0 is a tie
    CHECK(tie.AddCapture(0, Match::kCountdownMs + 61000) && tie.phase() == MatchPhase::Results && tie.winner() == 0);   // the next capture wins
    Match tie2; start(tie2, 60);
    tie2.AddCapture(0, 10000); tie2.AddCapture(1, 20000);
    tie2.Tick(Match::kCountdownMs + 60000);
    CHECK(tie2.phase() == MatchPhase::SuddenDeath);
    CHECK(tie2.AddCapture(1, Match::kCountdownMs + 70000) && tie2.winner() == 1 && tie2.phase() == MatchPhase::Results);
}

void TestCarrierLeavesMatch() {
    // The match side of a leaver: Leave counts it down with no kill, and the rule engine drops its flag.
    MatchConfig cfg; cfg.format = Format::CaptureTheFlag;
    Match m; m.Start(cfg, {{1, 0, true, 0}, {2, 1, true, 0}, {3, 1, true, 0}}, 0);
    m.Tick(Match::kCountdownMs);
    Ctf c = Fresh();
    Take1(c, 0);
    CHECK(m.Leave(1, 6000));
    auto ev = c.Remove(1, 6000);
    CHECK(ev.size() == 1 && !m.Alive(1) && m.Gone(1));
    CHECK(m.Score(0) == 0 && m.Score(1) == 0);
}

// Host migration: the captures are team scores, so the next host's Match::Start keeps them (ScoreCarryFrom); the flags
// start at home on the new host, and a carried flag is not carried over (nobody holds it on the new map).
void TestMigration() {
    MatchInfo mi; mi.format = uint8_t(Format::CaptureTheFlag); mi.phase = uint8_t(MatchPhase::Live); mi.scoreLimit = 3;
    mi.scores = {{0, 2}, {1, 1}};
    mi.fighters.push_back({1, 0, 0, 3, "Ann", 0, true, false, 111, 0});
    mi.fighters.push_back({2, 1, 5, 1, "Bo", 0, true, false, 222, 0});
    mi.objectives = Fresh().Objectives(0);
    const ScoreCarry carry = ScoreCarryFrom(mi);
    CHECK(carry.format == Format::CaptureTheFlag && carry.teams.at(0) == 2 && carry.teams.at(1) == 1 && carry.players.size() == 2);
    MatchConfig cfg; cfg.format = Format::CaptureTheFlag; cfg.ctfCaptures = 3;
    Match m; m.Start(cfg, {{1, 0, true, 111}, {2, 1, true, 222}}, 0, carry);
    CHECK(m.Score(0) == 2 && m.Score(1) == 1 && m.Kills(2) == 5 && m.Deaths(1) == 3);
    m.Tick(Match::kCountdownMs);
    CHECK(m.AddCapture(0, 6000) && m.phase() == MatchPhase::Results && m.winner() == 0);   // one more takes the match, as before
    Ctf c; c.Start(kReturn, kBase0, kBase1);                                  // the new host
    CHECK(c.state(0) == ObjectiveState::Home && c.state(1) == ObjectiveState::Home);
    // A carry of another format does not apply.
    MatchConfig teams; teams.format = Format::TwoTeams;
    Match o; o.Start(teams, {{1, 0, true, 111}, {2, 1, true, 222}}, 0, carry);
    CHECK(o.Score(0) == 0);
}

void TestBases() {
    // Two arcs of spawn points left and right of the middle.
    std::vector<Point3> pts = {{-2100, -100, 90}, {-2100, 100, 90}, {-1900, 0, 110}, {1900, 0, 110}, {2100, 100, 90}, {2100, -100, 90}};
    std::vector<int8_t> sides = PartitionSides(pts, 2);
    CtfBases b = DeriveBases(pts, sides);
    CHECK(b.ok && std::fabs(b.apartCm - 4066.67) < 1.0);
    const int left = b.at[0].x < 0 ? 0 : 1;
    CHECK(std::fabs(b.at[left].x - -2033.3) < 1.0 && std::fabs(b.at[left].y) < 1e-9 && std::fabs(b.at[left].z - 96.6667) < 0.01);
    CHECK(std::fabs(b.at[1 - left].x - 2033.3) < 1.0);
    // Hand-made sides: the centroid per side.
    CtfBases h = DeriveBases({{0, 0, 0}, {100, 0, 0}, {1000, 0, 0}, {1200, 200, 60}}, {0, 0, 1, 1});
    CHECK(h.ok && h.at[0].x == 50 && h.at[1].x == 1100 && h.at[1].y == 100 && h.at[1].z == 30);
    // Too close: not ok, but the places are still given.
    CtfBases close = DeriveBases({{0, 0, 0}, {100, 0, 0}, {200, 0, 0}, {300, 0, 0}}, {0, 0, 1, 1});
    CHECK(!close.ok && close.apartCm == 200 && close.at[0].x == 50 && close.at[1].x == 250);
    // One team without a point: mirrored through the centroid of the points.
    CtfBases one = DeriveBases({{0, 0, 5}, {200, 0, 5}}, {0, 0});
    CHECK(!one.ok && one.at[0].x == 100 && one.at[1].x == 100);               // the mirror of the centroid through itself
    CtfBases lone = DeriveBases({{0, 0, 5}, {100, 0, 5}, {1000, 0, 5}}, {0, 0, 2});
    CHECK(!lone.ok && lone.at[0].x == 50 && std::fabs(lone.at[1].x - (2 * (1100.0 / 3) - 50)) < 1e-9);
    // Nothing at all: two bases round the fallback.
    CtfBases none = DeriveBases({}, {}, {500, 500, 10});
    CHECK(!none.ok && none.at[0].x == 500 - kMinBaseGapCm / 2 && none.at[1].x == 500 + kMinBaseGapCm / 2 && none.at[0].y == 500 && none.apartCm == kMinBaseGapCm);
    // Non-finite points are ignored.
    CtfBases nan = DeriveBases({{NAN, 0, 0}, {0, 0, 0}, {1000, 0, 0}}, {0, 0, 1});
    CHECK(nan.ok && nan.at[0].x == 0 && nan.at[1].x == 1000);
}

void TestText() {
    auto name = [](uint8_t e) { return e == 1 ? std::string("Ann") : "#" + std::to_string(e); };
    CHECK(CtfEventLine({CtfEventKind::Captured, 1, 1}, name) == "Ann captured the BLUE flag");
    CHECK(CtfEventLine({CtfEventKind::Taken, 0, 1}, name) == "Ann took the RED flag");
    CHECK(CtfEventLine({CtfEventKind::Dropped, 0, 7}, name) == "#7 dropped the RED flag");
    CHECK(CtfEventLine({CtfEventKind::Returned, 1, 1}, name) == "Ann returned the BLUE flag");
    CHECK(CtfEventLine({CtfEventKind::Expired, 1, 0}, name) == "The BLUE flag went home");
    Objective o; o.owner = 0;
    CHECK(FlagStatusLine(o, "") == "RED FLAG: home");
    o.state = ObjectiveState::Carried; o.carrier = 4;
    CHECK(FlagStatusLine(o, "Ann") == "RED FLAG: taken by Ann");
    o.state = ObjectiveState::Dropped; o.carrier = 0; o.timerMs = 13200;
    CHECK(FlagStatusLine(o, "") == "RED FLAG: dropped 14 s");
    o.timerMs = 14000; CHECK(FlagStatusLine(o, "") == "RED FLAG: dropped 14 s");
    o.owner = 1; o.timerMs = 0; CHECK(FlagStatusLine(o, "") == "BLUE FLAG: dropped 0 s");
}

void TestEventWire() {
    for (uint8_t k = 1; k <= 5; ++k) {
        CtfEventMsg m; m.e = {CtfEventKind(k), 1, k == 4 ? uint8_t(0) : uint8_t(3)};
        auto b = Encode(m); CtfEventMsg o;
        MsgType t; CHECK(PeekType(b.data(), b.size(), t) && t == MsgType::CtfEvent && uint8_t(t) == 62);
        CHECK(Decode(b.data(), b.size(), o) && o.e == m.e);
        for (size_t cut = 0; cut < b.size(); ++cut) CHECK(!Decode(b.data(), cut, o));
        b.push_back(0); CHECK(!Decode(b.data(), b.size(), o));
    }
    CtfEventMsg bad; bad.e = {CtfEventKind(0), 0, 1}; auto b0 = Encode(bad); CtfEventMsg o;
    CHECK(!Decode(b0.data(), b0.size(), o));
    bad.e = {CtfEventKind(6), 0, 1}; b0 = Encode(bad); CHECK(!Decode(b0.data(), b0.size(), o));
    bad.e = {CtfEventKind::Taken, 2, 1}; b0 = Encode(bad); CHECK(!Decode(b0.data(), b0.size(), o));     // only two flags
    bad.e = {CtfEventKind::Taken, 0, 0}; b0 = Encode(bad); CHECK(!Decode(b0.data(), b0.size(), o));     // taken by nobody
    bad.e = {CtfEventKind::Expired, 0, 0}; b0 = Encode(bad); CHECK(Decode(b0.data(), b0.size(), o));    // nobody is right for a timeout
}

}  // namespace

int main() {
    TestTake();
    TestDropOnDeathAndDown();
    TestEnemyTakesDropped();
    TestOwnTeamReturns();
    TestAutoReturn();
    TestCapture();
    TestTwoCarriers();
    TestLeave();
    TestResetAndMove();
    TestMatchScoring();
    TestTimeLimitAndSuddenDeath();
    TestCarrierLeavesMatch();
    TestMigration();
    TestBases();
    TestText();
    TestEventWire();
    std::printf(g_fail ? "%d FAILED\n" : "all CTF tests passed\n", g_fail);
    return g_fail ? 1 : 0;
}
