// Roulette (#836; match/hsm_modes.h RouletteGame): unarmed peasants pass one crossbow round, six chambers and one of them
// live. The format's tables, the points, the cylinder and the turns, what every machine shows of it, who the holder
// faces, the wire kinds, the HUD lines and the lobby.
#include "match/hsm_crossbow.h"
#include "match/hsm_hud_scene.h"
#include "match/hsm_lobby_scene.h"
#include "match/hsm_maps.h"
#include "match/hsm_modes.h"
#include "protocol/hsm_protocol.h"
#include "session/hsm_autoflow.h"
#include <cstdio>
#include <map>
#include <set>
#include <string>
#include <vector>

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++g_fail; } } while (0)
using namespace hsm;
using Pull = RouletteGame::Pull;

static MatchConfig Cfg() { MatchConfig c; c.format = Format::Roulette; return Sanitize(c); }
static bool Has(const std::string& s, const char* part) { return s.find(part) != std::string::npos; }

// The format: a round format with no teams and no roles; bare peasants; no crossbow of the Crossbows row.
static void TestFormat() {
    const Format f = Format::Roulette;
    CHECK(uint8_t(f) == 18 && kFormatCount >= 19 && IsRoulette(f) && !IsRoulette(Format::OneCrossbow) && !IsRoulette(Format::Tag));
    CHECK(IsRoundFormat(f) && !HasTeams(f) && !HasRoles(f) && !IsQueueFormat(f) && !IsTag(f) && !IsOneCrossbow(f) && ScoresPerFighter(f));
    CHECK(DefaultScoreLimit(f) == 5 && DefaultScoreLimit(Format::LastManStanding) == 3 && DefaultScoreLimit(Format::FreeForAll) == 5 && DefaultScoreLimit(Format::TwoTeams) == 10);
    const MatchConfig c = Cfg();
    CHECK(c.gear == Gear::Fists && IsBare(c) && !IsBrawl(c) && c.tier == kOneCrossbowTier && c.crossbows == 0);
    for (uint8_t stale : {uint8_t(1), uint8_t(4)}) {                       // whatever another mode left
        MatchConfig o; o.format = f; o.crossbows = stale; o.gear = Gear::Mirror; o.tier = 7;
        const MatchConfig s = Sanitize(o);
        CHECK(s.crossbows == 0 && s.gear == Gear::Fists && s.tier == kOneCrossbowTier);
    }
    CHECK(GearAllowed(f, Gear::Fists) && !GearAllowed(Format::LastManStanding, Gear::Fists));
    { MatchConfig one; one.format = Format::OneCrossbow; CHECK(Sanitize(one).crossbows == uint8_t(Crossbows::One)); }   // its neighbour keeps its own
    { MatchConfig brawl; brawl.gear = Gear::Fists; CHECK(Sanitize(brawl).tier == kBrawlTier); }
    { MatchConfig lms; lms.format = Format::LastManStanding; lms.crossbows = uint8_t(Crossbows::Half); lms.tier = 6; CHECK(Sanitize(lms).crossbows == uint8_t(Crossbows::Half) && Sanitize(lms).tier == 6); }
}

// The points: a blank on yourself is one, the limit reached ends the match, and the round is one too for the last one
// standing. Nothing outside a fought round, for a fighter that is down, or in another format.
static void TestPoints() {
    MatchConfig c = Cfg(); c.scoreLimit = 3;
    Match m; m.Start(c, {{1, kNoTeam, true}, {2, kNoTeam, true}, {3, kNoTeam, true}}, 0);
    CHECK(!m.AwardPoint(1, 10) && m.Score(1) == 0);                // the countdown is no round
    const uint32_t t = Match::kCountdownMs;
    m.Tick(t);
    CHECK(m.AwardPoint(1, t + 1) && m.Score(1) == 1 && m.Score(2) == 0);
    m.OnDeath(2, 0, t + 2);                                        // the live chamber on themself: out, with no killer
    CHECK(!m.AwardPoint(2, t + 3) && m.Score(2) == 0);             // down: no point
    CHECK(!m.AwardPoint(9, t + 3));
    m.OnDeath(3, 1, t + 4);
    CHECK(m.roundWinner() == 1 && m.Score(1) == 2 && !m.RoundInPlay());   // the round is a point as well
    CHECK(!m.AwardPoint(1, t + 5) && m.Score(1) == 2);             // not in the gap
    const auto ev = m.Tick(t + 4 + 5000);
    CHECK(ev.roundReset && m.round() == 2 && m.RoundInPlay() && m.phase() == MatchPhase::Live);
    CHECK(m.AwardPoint(1, t + 6000) && m.Score(1) == 3 && m.phase() == MatchPhase::Results);   // the limit: the match is over
    CHECK(!m.AwardPoint(3, t + 6001) && m.Score(3) == 0);
    MatchConfig lms; lms.format = Format::LastManStanding;         // another format has no such point
    Match o; o.Start(Sanitize(lms), {{1, kNoTeam, true}, {2, kNoTeam, true}}, 0); o.Tick(t);
    CHECK(o.RoundInPlay() && !o.AwardPoint(1, t + 1) && o.Score(1) == 0);
}

// The pull (1-based) that is live after the first spin of a game with this seed.
static int LiveAt(uint32_t seed) {
    RouletteGame g; g.Reset(seed); g.BeginRound({1, 2, 3, 4}, 1000);
    const uint8_t h = g.holder(); uint32_t now = 1000;
    for (int n = 1; n <= 6; ++n) { if (g.OnPull(h, h, now) == Pull::LiveSelf) return n; now += kRoulettePullGapMs; }
    return 0;
}
static uint32_t SeedWith(int liveAt) { for (uint32_t s = 1; s < 500; ++s) if (LiveAt(s) == liveAt) return s; return 0; }
static RouletteGame Fresh(uint32_t seed) { RouletteGame g; g.Reset(seed); g.BeginRound({1, 2, 3, 4}, 1000); return g; }
static uint8_t Other(uint8_t h) { return h == 4 ? uint8_t(1) : uint8_t(h + 1); }
static uint8_t NextOf(const std::vector<uint8_t>& living, uint8_t h) { for (uint8_t e : living) if (e > h) return e; return living.front(); }

// The cylinder: six chambers, one live. The odds rise with every blank until it fires, then it is spun again.
static void TestCylinder() {
    std::map<int, int> firedAt; int differ = 0;
    for (uint32_t seed = 1; seed <= 600; ++seed) {
        RouletteGame g; g.Reset(seed);
        g.BeginRound({1, 2, 3}, 0);
        const uint8_t h = g.holder();
        CHECK(h >= 1 && h <= 3 && g.active() && !g.loaded() && g.pulls() == 0 && g.target() == 0);
        uint32_t now = 0; int n = 0; Pull p = Pull::Refused;
        do {
            p = g.OnPull(h, h, now); now += kRoulettePullGapMs; ++n;
            if (p == Pull::BlankSelf) CHECK(g.pulls() == n && g.holder() == h);
        } while (p == Pull::BlankSelf && n < 10);
        CHECK(p == Pull::LiveSelf && n >= 1 && n <= 6);
        ++firedAt[n];
        CHECK(g.pulls() == 0 && g.holder() != h && g.holder() != 0);       // spun again; the next fighter holds it
        const uint8_t h2 = g.holder(); int n2 = 0;
        do { p = g.OnPull(h2, h2, now); now += kRoulettePullGapMs; ++n2; } while (p == Pull::BlankSelf && n2 < 10);
        CHECK(p == Pull::LiveSelf && n2 >= 1 && n2 <= 6 && g.holder() == 0);
        if (n2 != n) ++differ;
    }
    CHECK(firedAt.size() == 6 && differ > 400);                            // a new spin is a new live chamber (5 times in 6)
    for (const auto& [n, count] : firedAt) CHECK(n >= 1 && n <= 6 && count > 50 && count < 170);   // each chamber about one time in six
}

// The turns.
static void TestTurns() {
    {   // refused: not the holder, nobody there, before the round, no round
        RouletteGame g = Fresh(7); const uint8_t h = g.holder(), o = Other(h);
        CHECK(g.OnPull(o, o, 5000) == Pull::Refused && g.OnPull(o, h, 5000) == Pull::Refused);
        CHECK(g.OnPull(h, 9, 5000) == Pull::Refused && g.OnPull(h, 0, 5000) == Pull::Refused);
        CHECK(g.OnPull(h, h, 999) == Pull::Refused && g.pulls() == 0 && g.holder() == h);
        RouletteGame idle; CHECK(idle.OnPull(1, 1, 5000) == Pull::Refused && idle.OnPull(0, 0, 5000) == Pull::Refused && !idle.active() && idle.holder() == 0);
        g.EndRound(); CHECK(g.OnPull(h, h, 5000) == Pull::Refused);
    }
    {   // the gap between two pulls
        const uint32_t seed = SeedWith(5); CHECK(seed != 0);
        RouletteGame g = Fresh(seed); const uint8_t h = g.holder();
        CHECK(g.OnPull(h, h, 2000) == Pull::BlankSelf);
        CHECK(g.OnPull(h, h, 2000 + kRoulettePullGapMs - 1) == Pull::Refused && g.pulls() == 1);
        CHECK(g.OnPull(h, h, 2000 + kRoulettePullGapMs) == Pull::BlankSelf && g.pulls() == 2);
    }
    {   // a blank on someone else hands the crossbow over and the count goes on; the live chamber on someone else loads it
        const uint32_t seed = SeedWith(4); CHECK(seed != 0);
        RouletteGame g = Fresh(seed); const uint8_t h = g.holder(), o = Other(h);
        uint32_t now = 1000;
        CHECK(g.OnPull(h, o, now) == Pull::BlankOther && g.holder() == o && g.pulls() == 1 && !g.loaded());
        now += kRoulettePullGapMs;
        CHECK(g.OnPull(h, h, now) == Pull::Refused);                             // no longer theirs
        CHECK(g.OnPull(o, o, now) == Pull::BlankSelf && g.holder() == o && g.pulls() == 2);
        now += kRoulettePullGapMs;
        CHECK(g.OnPull(o, h, now) == Pull::BlankOther && g.holder() == h && g.pulls() == 3);
        now += kRoulettePullGapMs;
        CHECK(!g.OnFired(h, now));                                               // nothing loaded yet
        CHECK(g.OnPull(h, o, now) == Pull::LiveOther && g.loaded() && g.target() == o && g.holder() == h && g.pulls() == 0);
        CHECK(g.OnPull(h, h, now + 5000) == Pull::Refused && g.OnPull(h, o, now + 5000) == Pull::Refused);   // a live round is shot, not pulled again
        CHECK(!g.OnFired(o, now + 100) && g.OnFired(h, now + 100) && !g.OnFired(h, now + 200));
        RouletteGame::Step st = g.Update({1, 2, 3, 4}, now + 100 + kRouletteLandMs - 1);
        CHECK(!st.moved && !st.wasted && g.holder() == h && g.loaded());         // the bolt is still on its way
        st = g.Update({1, 2, 3, 4}, now + 100 + kRouletteLandMs);
        CHECK(st.moved && !st.wasted && g.holder() == o && !g.loaded() && g.target() == 0);   // the one shot at has it now
        // the next live round on someone else can be shot again
        now += 100 + kRouletteLandMs;
        Pull p = Pull::Refused; int n = 0;
        do { const uint8_t a = g.holder(); p = g.OnPull(a, Other(a), now); now += kRoulettePullGapMs; ++n; } while (p == Pull::BlankOther && n < 10);
        CHECK(p == Pull::LiveOther && g.loaded() && g.OnFired(g.holder(), now));
        g.EndRound(); CHECK(!g.loaded() && g.target() == 0 && !g.active());
    }
    {   // the shot killed: the next one after the shooter
        const uint32_t seed = SeedWith(1); CHECK(seed != 0);
        RouletteGame g = Fresh(seed); const uint8_t h = g.holder(), o = Other(h);
        CHECK(g.OnPull(h, o, 1000) == Pull::LiveOther && g.OnFired(h, 1500));
        std::vector<uint8_t> left; for (uint8_t e = 1; e <= 4; ++e) if (e != o) left.push_back(e);
        RouletteGame::Step st = g.Update(left, 1600);
        CHECK(!st.moved && g.holder() == h && g.loaded());
        st = g.Update(left, 1500 + kRouletteLandMs);
        CHECK(st.moved && !st.wasted && g.holder() == NextOf(left, h) && g.holder() != h && !g.loaded());
    }
    {   // a live round nobody shot is wasted after kRouletteShotMs
        RouletteGame g = Fresh(SeedWith(1)); const uint8_t h = g.holder(), o = Other(Other(h));   // not the next one in line
        CHECK(g.OnPull(h, o, 1000) == Pull::LiveOther);
        RouletteGame::Step st = g.Update({1, 2, 3, 4}, 1000 + kRouletteShotMs - 1);
        CHECK(!st.moved && !st.wasted && g.loaded());
        st = g.Update({1, 2, 3, 4}, 1000 + kRouletteShotMs);
        CHECK(st.moved && st.wasted && g.holder() == o && !g.loaded() && !g.OnFired(h, 1000 + kRouletteShotMs + 1));
    }
    {   // the shooter died with the round loaded: the next fighter, nothing loaded
        RouletteGame g = Fresh(SeedWith(1)); const uint8_t h = g.holder(), o = Other(h);
        CHECK(g.OnPull(h, o, 1000) == Pull::LiveOther);
        std::vector<uint8_t> left; for (uint8_t e = 1; e <= 4; ++e) if (e != h) left.push_back(e);
        const RouletteGame::Step st = g.Update(left, 1100);
        CHECK(st.moved && !st.wasted && g.holder() == NextOf(left, h) && !g.loaded());
    }
    {   // the live chamber on yourself with two standing: nobody's turn is left
        RouletteGame g; g.Reset(SeedWith(1)); g.BeginRound({5, 9}, 0);
        const uint8_t h = g.holder(); CHECK(h == 5 || h == 9);
        CHECK(g.OnPull(h, h, 0) == Pull::LiveSelf && g.holder() == 0);
        const RouletteGame::Step st = g.Update({uint8_t(h == 5 ? 9 : 5)}, 10);
        CHECK(!st.moved && g.holder() == 0);
    }
    {   // the shot left one standing: nobody holds it and nothing is loaded; a new round starts unloaded
        RouletteGame g; g.Reset(SeedWith(1)); g.BeginRound({5, 9}, 0);
        const uint8_t h = g.holder(), o = uint8_t(h == 5 ? 9 : 5);
        CHECK(g.OnPull(h, o, 0) == Pull::LiveOther && g.loaded());
        const RouletteGame::Step st = g.Update({h}, 10);
        CHECK(st.moved && g.holder() == 0 && !g.loaded());
        RouletteGame b = Fresh(SeedWith(1)); const uint8_t bh = b.holder();
        CHECK(b.OnPull(bh, Other(bh), 1000) == Pull::LiveOther && b.loaded());
        b.BeginRound({1, 2, 3, 4}, 9000); CHECK(!b.loaded() && b.target() == 0);
    }
    {   // the live chamber on yourself with more standing: the next one in id order
        RouletteGame g = Fresh(SeedWith(1)); const uint8_t h = g.holder();
        std::vector<uint8_t> left; for (uint8_t e = 1; e <= 4; ++e) if (e != h) left.push_back(e);
        CHECK(g.OnPull(h, h, 1000) == Pull::LiveSelf && g.holder() == NextOf(left, h));
    }
    {   // a holder that is gone hands on; one standing: nobody; no round: nothing moves
        std::set<uint8_t> holders;
        for (uint32_t seed = 1; seed <= 40; ++seed) {                            // from every seat the next one in id order, round the ring
            RouletteGame r = Fresh(seed); const uint8_t rh = r.holder(); holders.insert(rh);
            std::vector<uint8_t> rest; for (uint8_t e = 1; e <= 4; ++e) if (e != rh) rest.push_back(e);
            const RouletteGame::Step moved = r.Update(rest, 2000);
            CHECK(moved.moved && r.holder() == NextOf(rest, rh) && r.holder() == Other(rh));
        }
        CHECK(holders == std::set<uint8_t>({1, 2, 3, 4}));
        RouletteGame g = Fresh(3); const uint8_t h = g.holder();
        std::vector<uint8_t> left; for (uint8_t e = 1; e <= 4; ++e) if (e != h) left.push_back(e);
        const uint8_t want = NextOf(left, h);
        RouletteGame::Step st = g.Update(left, 2000);
        CHECK(st.moved && g.holder() == want);
        st = g.Update(left, 2100); CHECK(!st.moved && g.holder() == want);
        st = g.Update({want}, 2200); CHECK(st.moved && g.holder() == 0);
        st = g.Update(left, 2300); CHECK(st.moved && g.holder() == left.front());   // somebody to play with again
        g.EndRound(); CHECK(!g.active() && g.holder() == 0);
        st = g.Update({1, 2, 3}, 2400); CHECK(!st.moved && g.holder() == 0);
    }
    {   // the first holder moves on from round to round; alone, nobody holds it
        RouletteGame g; g.Reset(11); std::set<uint8_t> firsts;
        for (int r = 0; r < 3; ++r) { g.BeginRound({1, 2, 3}, 0); firsts.insert(g.holder()); g.EndRound(); }
        CHECK(firsts == std::set<uint8_t>({1, 2, 3}));
        g.BeginRound({4}, 0); CHECK(g.holder() == 0 && g.active());
        g.Reset(11); g.BeginRound({1, 2, 3}, 0); CHECK(!g.loaded() && g.pulls() == 0 && g.holder() != 0);
    }
}

// What every machine shows of the cylinder.
static void TestView() {
    RouletteView v; CHECK(v.OneIn() == 6 && !v.loaded && v.pulls == 0);
    v.OnInfo(1, 2);
    v.OnClick(); v.OnClick(); CHECK(v.pulls == 2 && v.OneIn() == 4);
    v.OnInfo(1, 3); CHECK(v.pulls == 2 && v.holder == 3);  // another hand: the count goes on
    v.OnBang(false); CHECK(v.pulls == 0 && v.loaded && v.OneIn() == 6);
    v.OnInfo(1, 3); CHECK(v.loaded);                       // the same holder: still loaded
    v.OnInfo(1, 4); CHECK(!v.loaded);                      // shot: the crossbow moved on
    v.OnClick(); v.OnBang(true); CHECK(v.pulls == 0 && !v.loaded);
    for (int i = 0; i < 9; ++i) v.OnClick();
    CHECK(v.pulls == 5 && v.OneIn() == 1);                 // never past the last chamber
    v.OnBang(false); v.OnClick(); v.OnInfo(2, 4);
    CHECK(v.pulls == 0 && !v.loaded && v.round == 2);      // a new round: a fresh spin
}

// Who the holder faces: from the camera through the body, on the ground plane.
static void TestFaced() {
    const std::vector<AimMark> marks = {{2, 500, 0}, {3, 300, 200}, {4, -400, 0}, {5, 5000, 0}};
    CHECK(FacedFighter(-300, 0, 0, 0, marks) == 2);        // the camera behind, looking along +x
    CHECK(FacedFighter(300, 0, 0, 0, marks) == 4);         // looking the other way
    CHECK(FacedFighter(0, -300, 0, 0, marks) == 0);        // along +y: nobody within the cone
    CHECK(FacedFighter(0, 0, 0, 0, marks) == 0 && FacedFighter(0.5, 0, 0, 0, marks) == 0);   // the camera on the body: ahead is unknown
    const std::vector<AimMark> far = {{5, 3000.5, 0}}, reach = {{5, 3000, 0}}, in = {{6, 100, 40}}, out = {{6, 100, 60}};
    CHECK(FacedFighter(-300, 0, 0, 0, far) == 0 && FacedFighter(-300, 0, 0, 0, reach) == 5);
    CHECK(FacedFighter(-300, 0, 0, 0, in) == 6 && FacedFighter(-300, 0, 0, 0, out) == 0);    // the cone: about 25 degrees
    const std::vector<AimMark> two = {{6, 200, 50}, {2, 500, 0}}, owt = {{2, 500, 0}, {6, 200, 50}}, tie = {{7, 400, 0}, {2, 500, 0}};
    CHECK(FacedFighter(-300, 0, 0, 0, two) == 2 && FacedFighter(-300, 0, 0, 0, owt) == 2);   // the straightest, not the nearest
    CHECK(FacedFighter(-300, 0, 0, 0, tie) == 7);
    const std::vector<AimMark> none = {{0, 500, 0}}, self = {{8, 0.5, 0}}, moved = {{9, 1500, 1000}}, shadow = {{0, 500, 0}, {2, 500, 100}};
    CHECK(FacedFighter(-300, 0, 0, 0, shadow) == 2);       // a mark with no fighter hides nobody
    CHECK(FacedFighter(-300, 0, 0, 0, none) == 0 && FacedFighter(-300, 0, 0, 0, self) == 0 && FacedFighter(-300, 0, 0, 0, {}) == 0);
    CHECK(FacedFighter(700, 1000, 1000, 1000, moved) == 9 && FacedFighter(1000, 700, 1000, 1000, moved) == 0);   // measured from the body, not the origin
}

// The wire: two asks (client to host) and two answers (host to everyone), each with its fighters.
static void TestWire() {
    auto ok = [](ModeEventKind k, uint8_t a, uint8_t b) {
        ModeEventMsg out; const std::vector<uint8_t> w = Encode(ModeEventMsg{k, a, b});
        return Decode(w.data(), w.size(), out) && out.kind == k && out.a == a && out.b == b;
    };
    CHECK(uint8_t(ModeEventKind::RoulettePull) == 4 && uint8_t(ModeEventKind::RouletteFired) == 5 && uint8_t(ModeEventKind::RouletteClick) == 6 && uint8_t(ModeEventKind::RouletteBang) == 7);
    CHECK(ok(ModeEventKind::RoulettePull, 3, 3) && ok(ModeEventKind::RoulettePull, 3, 4) && !ok(ModeEventKind::RoulettePull, 0, 4) && !ok(ModeEventKind::RoulettePull, 3, 0));
    CHECK(ok(ModeEventKind::RouletteFired, 3, 0) && !ok(ModeEventKind::RouletteFired, 3, 4) && !ok(ModeEventKind::RouletteFired, 0, 0));
    CHECK(ok(ModeEventKind::RouletteClick, 2, 2) && ok(ModeEventKind::RouletteClick, 2, 5) && !ok(ModeEventKind::RouletteClick, 0, 5) && !ok(ModeEventKind::RouletteClick, 2, 0));
    CHECK(ok(ModeEventKind::RouletteBang, 2, 2) && ok(ModeEventKind::RouletteBang, 2, 5) && !ok(ModeEventKind::RouletteBang, 0, 5) && !ok(ModeEventKind::RouletteBang, 2, 0));
    CHECK(!ok(ModeEventKind(8), 1, 2) && !ok(ModeEventKind(0), 1, 2) && !ok(ModeEventKind::RouletteBang, 1, uint8_t(kMaxWillies + 1)));
    CHECK(ok(ModeEventKind::TagPass, 1, 2) && !ok(ModeEventKind::TagPass, 1, 1) && ok(ModeEventKind::TagBurn, 1, 0) && ok(ModeEventKind::Infected, 0, 2) && !ok(ModeEventKind::Infected, 1, 0));
    CHECK(ModeEventIsAsk(ModeEventKind::RoulettePull) && ModeEventIsAsk(ModeEventKind::RouletteFired));
    CHECK(!ModeEventIsAsk(ModeEventKind::RouletteClick) && !ModeEventIsAsk(ModeEventKind::RouletteBang) && !ModeEventIsAsk(ModeEventKind::TagPass) &&
          !ModeEventIsAsk(ModeEventKind::TagBurn) && !ModeEventIsAsk(ModeEventKind::Infected));
}

static MatchInfo Info(uint8_t holder) {
    MatchInfo m; m.format = uint8_t(Format::Roulette); m.phase = uint8_t(MatchPhase::Live); m.round = 2; m.scoreLimit = 5; m.tagIt = holder;
    m.fighters = {{1, kNoTeam, 0, 0, "Ann", 0, true, false, 0, 0}, {2, kNoTeam, 0, 0, "Bo", 0, true, false, 0, 0}, {3, kNoTeam, 0, 0, "Cy", 0, false, false, 0, 0}};
    return m;
}
static std::string Text(const FeedRow& r) { std::string s; for (const Span& p : r.spans) s += p.text; return s; }

// The HUD: the line under the round pill, the feed and the board.
static void TestScreens() {
    const MatchInfo mine = Info(1), theirs = Info(2);
    CHECK(RouletteLine(mine, 1, 6, false) == "YOUR TURN   X: on yourself   C: on the fighter you face   1 in 6");
    CHECK(RouletteLine(mine, 1, 2, false) == "YOUR TURN   X: on yourself   C: on the fighter you face   1 in 2" && RouletteLine(mine, 1, 3, true) == "LIVE ROUND: SHOOT");
    CHECK(RouletteLine(theirs, 1, 4, false) == "Bo holds the crossbow   1 in 4" && RouletteLine(theirs, 1, 4, true) == "Bo has a live round");
    CHECK(Has(RouletteLine(mine, 1, 0, false), "1 in 1") && Has(RouletteLine(mine, 1, 99, false), "1 in 6"));
    CHECK(RouletteLine(Info(0), 1, 6, false).empty());
    { MatchInfo n = mine; n.phase = uint8_t(MatchPhase::Results); CHECK(RouletteLine(n, 1, 6, false).empty()); }
    { MatchInfo n = mine; n.phase = uint8_t(MatchPhase::SuddenDeath); CHECK(!RouletteLine(n, 1, 6, false).empty()); }
    { MatchInfo n = mine; n.format = uint8_t(Format::Tag); CHECK(RouletteLine(n, 1, 6, false).empty()); }
    LocalHud l; l.me = 1; l.host = 1; l.infoAtMs = 1000; l.rouletteOneIn = 3;
    HudScene s = BuildHudScene(mine, l, 1000);
    CHECK(s.info.visible && s.info.text == RouletteLine(mine, 1, 3, false) && s.info.color.r == style::kWarn.r && s.info.color.g == style::kWarn.g && s.info.color.b == style::kWarn.b);
    CHECK(s.pill.text.find("ROUND 2") == 0 && !Has(s.pill.text, "is it"));
    s = BuildHudScene(theirs, l, 1000);
    CHECK(s.info.visible && s.info.text == "Bo holds the crossbow   1 in 3" && s.info.color.r == style::kIron.r && s.info.color.g == style::kIron.g && s.info.color.b == style::kIron.b);
    l.rouletteLoaded = true; s = BuildHudScene(theirs, l, 1000);
    CHECK(s.info.text == "Bo has a live round");
    s = BuildHudScene(Info(0), l, 1000);
    CHECK(!s.info.visible);
    l.tab = true; s = BuildHudScene(theirs, l, 1000);
    int tags = 0; for (const BoardRow& r : s.board.rows) if (r.tag == "CROSSBOW") { ++tags; CHECK(r.name == "Bo"); }
    CHECK(tags == 1);
    { MatchInfo n = theirs; n.format = uint8_t(Format::LastManStanding); s = BuildHudScene(n, l, 1000); for (const BoardRow& r : s.board.rows) CHECK(r.tag != "CROSSBOW"); }
    const ModeEventMsg clickSelf{ModeEventKind::RouletteClick, 1, 1}, clickOther{ModeEventKind::RouletteClick, 1, 2}, bangSelf{ModeEventKind::RouletteBang, 2, 2},
                       bangOther{ModeEventKind::RouletteBang, 2, 1}, ask{ModeEventKind::RoulettePull, 1, 2}, fired{ModeEventKind::RouletteFired, 1, 0};
    CHECK(Text(MakeModeFeedRow(mine, clickSelf, 1)) == "Ann pulled the trigger: click  +1" && MakeModeFeedRow(mine, clickSelf, 1).mine && !MakeModeFeedRow(mine, clickSelf, 3).mine);
    CHECK(Text(MakeModeFeedRow(mine, clickOther, 3)) == "Ann pulled on Bo: click" && MakeModeFeedRow(mine, clickOther, 2).mine);
    CHECK(Text(MakeModeFeedRow(mine, bangSelf, 1)) == "Bo pulled the trigger: BANG");
    CHECK(Text(MakeModeFeedRow(mine, bangOther, 1)) == "Bo has a live round for Ann");
    CHECK(MakeModeFeedRow(mine, ask, 1).spans.empty() && MakeModeFeedRow(mine, fired, 1).spans.empty());
    CHECK(ModeEventLine(mine, clickSelf) == "Ann pulled the trigger: click  +1" && ModeEventLine(mine, clickOther) == "Ann pulled on Bo: click");
    CHECK(ModeEventLine(mine, bangSelf) == "Bo pulled the trigger: BANG" && ModeEventLine(mine, bangOther) == "Bo has a live round for Ann");
    CHECK(ModeEventLine(mine, ask).empty() && ModeEventLine(mine, fired).empty());
}

// The lobby: its card, its rows, two players; picking it makes the peasants, leaving it puts gear and tier back.
static void TestLobby() {
    const Format f = Format::Roulette;
    CHECK(FormatName(f) == "Roulette" && Has(FormatBlurb(f), "six chambers"));
    CHECK(SettingsFor(Cfg()) == std::vector<LobbyRow>({LobbyRow::Limit, LobbyRow::Time, LobbyRow::Respawn, LobbyRow::FreeCam, LobbyRow::Damage, LobbyRow::DeadWeight,
                                                      LobbyRow::WeaponWeight, LobbyRow::Sync}));
    CHECK(!ModeAvailable(f, 1).ok && ModeAvailable(f, 2).ok && ModeAvailable(f, 20).ok && std::string(ModeAvailable(f, 1).hint) == "Needs 2 or more players");
    bool card = false, browser = false;
    for (size_t i = 0; i < kModeOrderCount; ++i) card = card || kModeOrder[i] == f;
    for (const std::string& n : BrowserModes()) browser = browser || n == "Roulette";
    CHECK(card && browser && kModeOrderCount + 2 == kModeCardCount);
    Format parsed = Format::FreeForAll;
    CHECK(ParseServerFormat("roulette", &parsed) && parsed == f);
    for (const MapInfo& m : kMaps) CHECK(MapAllowsFormat(m, f) == MapAllowsFormat(m, Format::LastManStanding));
    MatchConfig from; from.gear = Gear::Mirror; from.tier = 6; from.crossbows = uint8_t(Crossbows::Half); from.damagePct = 150;
    const MatchConfig in = SelectMode(from, f, false);
    CHECK(in.format == f && in.gear == Gear::Fists && in.tier == kOneCrossbowTier && in.crossbows == 0 && in.scoreLimit == 5 && in.damagePct == 150);
    const MatchConfig out = SelectMode(in, Format::LastManStanding, false);
    CHECK(out.format == Format::LastManStanding && out.gear == Gear::Own && out.tier == MatchConfig{}.tier && out.crossbows == 0 && out.damagePct == 150 && out.scoreLimit == 3);
    MatchConfig tagLike = in; tagLike.damagePct = kTagDamagePct;               // only Tag's light hits go back with Tag
    CHECK(SelectMode(tagLike, Format::Duel, false).damagePct == kTagDamagePct);
    CHECK(IsBrawl(SelectMode(in, Format::FreeForAll, true)) && SelectMode(in, Format::FreeForAll, true).tier == kBrawlTier);
    CHECK(SelectMode(in, f, false).gear == Gear::Fists && SelectMode(in, f, false).tier == kOneCrossbowTier);   // the same card again: nothing changes
    LobbyLocal l; l.phase = LobbyPhase::InLobby; l.host = true; l.myName = "Ann"; l.version = "0.2.0"; l.config = in;
    for (const char* name : {"Ann", "Bob", "Cat"}) { LobbyPlayer p; p.name = name; p.me = l.players.empty(); p.host = l.players.empty(); l.players.push_back(p); }
    const SettingRow limit = lobby_detail::Row(l, LobbyRow::Limit);
    CHECK(limit.label == "Points to win" && Has(limit.hint, "every blank you pull on yourself"));
    { LobbyLocal o = l; o.config = SelectMode(in, Format::OneCrossbow, false); CHECK(lobby_detail::Row(o, LobbyRow::Limit).label == "Rounds to win" && !Has(lobby_detail::Row(o, LobbyRow::Limit).hint, "blank")); }
    const LobbyScene s = BuildLobbyScene(l);
    bool selected = false;
    for (const ModeCard& m : s.modes) if (m.format == f && !m.brawl && !m.progression) selected = m.selected && m.name == "ROULETTE" && m.enabled;
    CHECK(selected && s.summary.find("ROULETTE") == 0);
}

int main() {
    TestFormat(); TestPoints(); TestCylinder(); TestTurns(); TestView(); TestFaced(); TestWire(); TestScreens(); TestLobby();
    if (g_fail) { std::printf("%d check(s) failed\n", g_fail); return 1; }
    std::printf("roulette tests passed\n");
    return 0;
}
