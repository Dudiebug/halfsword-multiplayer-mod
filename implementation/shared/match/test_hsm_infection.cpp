// Infection (match/hsm_modes.h, Match): patient zeros by player count and rotation, a survivor killed by an infected fighter is
// infected and respawns, a survivor killed by a survivor, itself or the world is out for good, the infected win when no survivor is
// left, the survivors win when the clock finds one standing, a late joiner is infected, the zombie trait table, the dark green kit,
// the HUD and the lobby's rows.
#include "match/hsm_hud_scene.h"
#include "match/hsm_lobby_scene.h"
#include "match/hsm_maps.h"
#include "match/hsm_modes.h"
#include "match/hsm_nametag.h"
#include "match/hsm_team_kit.h"
#include "protocol/hsm_protocol.h"
#include <cmath>
#include <cstdio>
#include <set>

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++g_fail; } } while (0)
using namespace hsm;

using V = std::vector<uint8_t>;
constexpr uint32_t kLive = Match::kCountdownMs;
static bool Has(const std::string& s, const char* part) { return s.find(part) != std::string::npos; }

static MatchConfig Cfg(uint16_t timeS = 300, uint16_t respawnS = 3) {
    MatchConfig c; c.format = Format::Infection; c.timeLimitS = timeS; c.respawnDelayS = respawnS; return c;
}
// A live Infection match: fighter i (entity 1..) is on teams[i].
static Match Live(const std::vector<int8_t>& teams, MatchConfig c = Cfg()) {
    Match m; std::vector<FighterInfo> f;
    for (size_t i = 0; i < teams.size(); ++i) f.push_back({uint8_t(i + 1), teams[i], true});
    m.Start(c, f, 0); m.Tick(kLive);
    return m;
}
static V Ids(int n) { V v; for (int i = 1; i <= n; ++i) v.push_back(uint8_t(i)); return v; }

// Criterion: one or two patient zeros by player count, never all the fighters, none for a lone fighter.
static void TestZeroCount() {
    CHECK(PatientZeroCount(0) == 0 && PatientZeroCount(1) == 0);
    for (int n = 2; n <= 5; ++n) CHECK(PatientZeroCount(n) == 1);
    for (int n = 6; n <= 9; ++n) CHECK(PatientZeroCount(n) == 2);
    for (int n = 2; n <= 12; ++n) CHECK(PatientZeroCount(n) < n);
}

static int Zeros(const std::map<uint8_t, int8_t>& m) { int n = 0; for (const auto& [e, t] : m) n += t == kInfectedTeam; return n; }
static std::set<uint8_t> ZeroSet(const std::map<uint8_t, int8_t>& m) { std::set<uint8_t> s; for (const auto& [e, t] : m) if (t == kInfectedTeam) s.insert(e); return s; }

// Criterion: the zeros are the right number, the rest survive, the same arguments give the same teams, and over the rematches
// every fighter has been a patient zero.
static void TestAssign() {
    for (int n = 2; n <= 9; ++n) {
        const auto t = AssignInfection(Ids(n), 1234, 0);
        CHECK(int(t.size()) == n && Zeros(t) == PatientZeroCount(n));
        for (const auto& [e, team] : t) CHECK(team == kInfectedTeam || team == kSurvivorTeam);
        CHECK(t == AssignInfection(Ids(n), 1234, 0));
        V rev = Ids(n); std::reverse(rev.begin(), rev.end());
        CHECK(t == AssignInfection(rev, 1234, 0));                                            // order is no input
    }
    CHECK(AssignInfection({}, 1, 0).empty());
    CHECK(AssignInfection({5, 5}, 1, 0).size() == 1);                                        // repeats ignored
    CHECK(Zeros(AssignInfection({5}, 1, 0)) == 0);                                            // a lone fighter is not infected
    for (int n : {3, 4, 6, 8}) {
        std::set<uint8_t> every;
        for (uint32_t r = 0; r < uint32_t(n); ++r) { const auto z = ZeroSet(AssignInfection(Ids(n), 99, r)); every.insert(z.begin(), z.end()); }
        CHECK(int(every.size()) == n);                                                        // everybody gets a turn
        CHECK(ZeroSet(AssignInfection(Ids(n), 99, 0)) != ZeroSet(AssignInfection(Ids(n), 99, 1)));
    }
    bool seedMatters = false;
    for (uint32_t s = 1; s < 30; ++s) seedMatters = seedMatters || ZeroSet(AssignInfection(Ids(5), s, 0)) != ZeroSet(AssignInfection(Ids(5), 1, 0));
    CHECK(seedMatters);
    CHECK(InfectionLateJoinerTeam() == kInfectedTeam);
}

// Criterion: only a survivor killed by an infected fighter is infected.
static void TestInfectsOnKill() {
    CHECK(InfectsOnKill(kInfectedTeam, kSurvivorTeam, false));
    CHECK(!InfectsOnKill(kSurvivorTeam, kSurvivorTeam, false));      // by a survivor
    CHECK(!InfectsOnKill(kInfectedTeam, kSurvivorTeam, true));       // by itself
    CHECK(!InfectsOnKill(kNoTeam, kSurvivorTeam, false));            // by the world (no killer)
    CHECK(!InfectsOnKill(kInfectedTeam, kInfectedTeam, false));      // an infected one is infected already
    CHECK(!InfectsOnKill(kSurvivorTeam, kInfectedTeam, false));
}

// Criterion: a survivor killed by an infected fighter is infected at once, the killer is credited, the headcounts follow, and it
// respawns after the respawn delay; the survivor killed by anything else does not come back.
static void TestInfectionAndRespawn() {
    Match m = Live({1, 0, 0, 0});                                    // entity 1 is the patient zero
    CHECK(m.phase() == MatchPhase::Live && m.Score(kSurvivorTeam) == 3 && m.Score(kInfectedTeam) == 1);
    m.OnDeath(2, 1, kLive + 100);                                    // the zero kills a survivor
    CHECK(!m.Alive(2) && m.TeamOf(2) == kInfectedTeam && m.Kills(1) == 1 && m.Deaths(2) == 1);
    CHECK(m.Score(kSurvivorTeam) == 2 && m.Score(kInfectedTeam) == 2 && m.phase() == MatchPhase::Live);
    CHECK(m.Tick(kLive + 2999).respawn.empty());
    Match::Events ev = m.Tick(kLive + 3100);
    CHECK(ev.respawn == V{2});                                       // the infected come back
    m.MarkRespawned(2);
    CHECK(m.Alive(2) && m.TeamOf(2) == kInfectedTeam && m.Score(kSurvivorTeam) == 2);
    // The newly infected infect the next: a survivor killed by entity 2.
    m.OnDeath(3, 2, kLive + 4000);
    CHECK(m.TeamOf(3) == kInfectedTeam && m.Kills(2) == 1 && m.Score(kInfectedTeam) == 3);
    // A survivor killed by a survivor, by itself and by the world: out, stays a survivor, never respawns.
    Match o = Live({1, 0, 0, 0, 0});
    o.OnDeath(2, 3, kLive + 10);                                     // by a survivor (friendly fire on)
    o.OnDeath(3, 3, kLive + 20);                                     // by itself
    o.OnDeath(4, 0, kLive + 30);                                     // by the world
    for (uint8_t e : V{2, 3, 4}) CHECK(!o.Alive(e) && o.TeamOf(e) == kSurvivorTeam);
    CHECK(o.Kills(3) == 0 && o.Deaths(2) == 1);
    CHECK(o.Score(kSurvivorTeam) == 1 && o.Score(kInfectedTeam) == 1 && o.phase() == MatchPhase::Live);   // entity 5 stands
    for (uint32_t t = kLive + 3000; t < kLive + 60000; t += 5000) CHECK(o.Tick(t).respawn.empty());
    // An infected fighter killed by anyone comes back as infected, whoever killed it.
    Match p = Live({1, 0, 0});
    p.OnDeath(1, 2, kLive + 10);                                     // the zero is killed by a survivor
    CHECK(p.TeamOf(1) == kInfectedTeam && p.Kills(2) == 0 && p.Tick(kLive + 3010).respawn == V{1});
    p.MarkRespawned(1);
    p.OnDeath(1, 1, kLive + 3100);                                   // and kills itself
    CHECK(p.TeamOf(1) == kInfectedTeam && p.Score(kInfectedTeam) == 1 && p.Tick(kLive + 6200).respawn == V{1});
    // Killing a fighter of its own side infects nobody and adds nothing.
    Match q = Live({1, 1, 0});
    q.OnDeath(2, 1, kLive + 10);
    CHECK(q.Kills(1) == 0 && q.TeamOf(2) == kInfectedTeam && q.Score(kSurvivorTeam) == 1);
}

// Criterion: the infected win when every survivor is down or gone; the survivors win when the clock finds one standing.
static void TestWinConditions() {
    Match a = Live({1, 0, 0});
    a.OnDeath(2, 1, kLive + 10);
    CHECK(a.phase() == MatchPhase::Live);
    a.OnDeath(3, 1, kLive + 20);                                     // the last survivor
    CHECK(a.phase() == MatchPhase::Results && a.winner() == kInfectedTeam && a.Tick(kLive + 20).ended);
    // The last survivor dying to the world also ends it for the infected: nobody is left to survive.
    Match w = Live({1, 0});
    w.OnDeath(2, 0, kLive + 10);
    CHECK(w.phase() == MatchPhase::Results && w.winner() == kInfectedTeam);
    // At the time limit a survivor still standing wins for the survivors.
    Match b = Live({1, 0, 0}, Cfg(60));
    b.OnDeath(2, 1, kLive + 1000);
    CHECK(b.Tick(kLive + 59999).ended == false && b.phase() == MatchPhase::Live);
    Match::Events ev = b.Tick(kLive + 60000);
    CHECK(ev.ended && b.phase() == MatchPhase::Results && b.winner() == kSurvivorTeam);   // no sudden death, no tie
    CHECK(b.TimeLeftMs(kLive + 60000) == Match::kResultsMs);
    // Everybody infected before the clock: the infected took it earlier.
    Match c = Live({1, 0}, Cfg(60));
    c.OnDeath(2, 1, kLive + 100);
    CHECK(c.winner() == kInfectedTeam);
    // The last survivor leaving (a leaver, then a removal) ends it for the infected.
    Match d = Live({1, 0, 0});
    CHECK(d.Leave(2, kLive + 10) && d.phase() == MatchPhase::Live && d.TeamOf(2) == kSurvivorTeam);
    d.RemoveFighter(3);
    CHECK(d.phase() == MatchPhase::Results && d.winner() == kInfectedTeam);
    Match e = Live({1, 0, 0}); e.RemoveFighter(1);                   // the infected leaving: the survivors play on
    CHECK(e.phase() == MatchPhase::Live && e.Score(kInfectedTeam) == 0);
    CHECK(e.Tick(kLive + 300000).ended && e.winner() == kSurvivorTeam);
    // Finish() still ends it with a team key.
    Match f = Live({1, 0}); f.Finish(kSurvivorTeam, kLive + 5); CHECK(f.winner() == kSurvivorTeam);
}

// Criterion: a death in the countdown is not a kill; the fighter is back with the first respawns. A fighter joining a running
// match comes in infected and counts in the headcounts.
static void TestJoinAndCountdown() {
    Match m; m.Start(Cfg(), {{1, kInfectedTeam, true}, {2, kSurvivorTeam, true}}, 0);
    m.OnDeath(2, 0, 500);                                            // before the fight: no scoring
    CHECK(!m.Alive(2) && m.Deaths(2) == 0 && m.Score(kSurvivorTeam) == 0);
    CHECK(m.Tick(3499).respawn.empty() && m.Tick(3500).respawn == V{2});
    m.MarkRespawned(2);
    CHECK(m.Score(kSurvivorTeam) == 1);
    m.Tick(kLive);
    m.AddFighter({3, InfectionLateJoinerTeam(), true});
    CHECK(m.TeamOf(3) == kInfectedTeam && m.Alive(3) && m.Score(kInfectedTeam) == 2 && m.Score(kSurvivorTeam) == 1 && m.phase() == MatchPhase::Live);
    m.OnDeath(3, 2, kLive + 10);                                     // killed by a survivor: back as infected
    CHECK(m.Tick(kLive + 3010).respawn == V{3});
    // A leaver's slot given to a new player: the new player is infected.
    m.RemoveFighter(3); m.AddFighter({3, InfectionLateJoinerTeam(), true});
    CHECK(m.TeamOf(3) == kInfectedTeam);
}

// Criterion: a rematch is a fresh deal: everybody alive, the headcounts follow the new teams, the old scores do not carry.
static void TestRematch() {
    Match m = Live({1, 0, 0}, Cfg(60));
    m.OnDeath(2, 1, kLive + 100); m.OnDeath(3, 1, kLive + 200);
    CHECK(m.winner() == kInfectedTeam);
    Match::Events ev;
    for (uint32_t t = kLive + 200; !ev.rematch && t < kLive + 200 + 20000; t += 1000) ev = m.Tick(t);
    CHECK(ev.rematch && m.phase() == MatchPhase::Countdown && m.Alive(2) && m.Alive(3));
    CHECK(m.Score(kInfectedTeam) == 3 && m.Score(kSurvivorTeam) == 0);               // the old teams until the host deals again
    const auto teams = AssignInfection(Ids(3), 7, 1);
    for (const auto& [e, t] : teams) m.SetTeam(e, t);                                  // the host's re-deal
    CHECK(m.Score(kInfectedTeam) == 1 && m.Score(kSurvivorTeam) == 2 && m.Kills(1) == 0 && m.Deaths(2) == 0);
    // A host migration does not bring the old scores: headcounts are recomputed.
    ScoreCarry carry; carry.format = Format::Infection; carry.teams = {{0, 9}, {1, 9}};
    Match n; n.Start(Cfg(), {{1, 1, true}, {2, 0, true}}, 0, carry);
    CHECK(n.Score(kSurvivorTeam) == 1 && n.Score(kInfectedTeam) == 1);
}

// Criterion: Infection always has a clock, no friendly fire, no crossbows and no ground clear; it is two fixed teams without picks.
static void TestConfig() {
    MatchConfig c; c.format = Format::Infection; c.timeLimitS = 0; c.friendlyFire = true; c.crossbows = 3; c.clearGroundMin = 3; c.teams = 4;
    const MatchConfig s = Sanitize(c);
    CHECK(s.timeLimitS == kInfectionDefaultTimeS && kInfectionDefaultTimeS == 300 && !s.friendlyFire && s.crossbows == 0 && s.clearGroundMin == 0);
    c.timeLimitS = 900; CHECK(Sanitize(c).timeLimitS == 900);
    CHECK(HasTeams(Format::Infection) && !PicksTeams(Format::Infection) && !HasRoles(Format::Infection) && !IsRoundFormat(Format::Infection));
    CHECK(TeamCount(Format::Infection, 4) == 2 && !ScoresPerFighter(Format::Infection) && !IsBotFormat(Format::Infection));
    const auto t = ResolveTeams({{0, kNoTeam, kNoTeam}, {1, 3, kNoTeam}}, Format::Infection, 4);
    CHECK(t.size() == 2);                                                              // the lobby's picks are not used: the infection decides
    CHECK(MapAllowsFormat(kMaps[0], Format::Infection) == MapAllowsFormat(kMaps[0], Format::FreeForAll));
    for (const MapInfo& m : kMaps) CHECK(MapAllowsFormat(m, Format::Infection) == (MapAllowsFormat(m, Format::FreeForAll) || m.abyss));   // the Abyss too
}

// Criterion: the trait table names the zombie flag and the pain fields the stand-ins zero, the consciousness floor is half of the
// spawn value, and the capability line says what was found.
static void TestTraits() {
    CHECK(std::string(kInfectedZombieFlag) == "Is Zombie?");
    std::set<std::string> pain(std::begin(kInfectedPainDoubles), std::end(kInfectedPainDoubles));
    CHECK(pain.size() == std::size(kInfectedPainDoubles) && pain.count("Pain Shock Rate") && pain.count("Pain Head") && pain.count("Pain Leg L") && pain.count("Pain Grab Rate"));
    for (const std::string& p : pain) CHECK(p.rfind("Pain", 0) == 0);
    CHECK(std::string(kInfectedPainBools[0]) == "Pain Shock" && std::string(kInfectedConsciousnessFields[0]) == "Consciousness");
    CHECK(InfectedConsciousnessFloor(100.0) == 50.0 && InfectedConsciousnessFloor(1.0) == 0.5);
    CHECK(std::isnan(InfectedConsciousnessFloor(0.0)) && std::isnan(InfectedConsciousnessFloor(-3.0)) && std::isnan(InfectedConsciousnessFloor(std::nan(""))));
    CHECK(InfectedConsciousness(10.0, 50.0) == 50.0 && InfectedConsciousness(80.0, 50.0) == 80.0 && InfectedConsciousness(50.0, 50.0) == 50.0);
    CHECK(InfectedConsciousness(-5.0, 50.0) == 50.0);
    CHECK(InfectedConsciousness(10.0, std::nan("")) == 10.0);                           // no floor: untouched
    CHECK(std::isnan(InfectedConsciousness(std::nan(""), 50.0)));                       // no reading: untouched
    CHECK(InfectedTraitLine("Willie_BP_C_2", true, 12, "Consciousness") == "infection: zombie traits on Willie_BP_C_2: Is Zombie? yes, pain fields 12, consciousness Consciousness");
    CHECK(InfectedTraitLine("Bo", false, 0, "") == "infection: zombie traits on Bo: Is Zombie? missing, pain fields 0, consciousness missing");
}

// Criterion: the infected wear a dark green of their own (not the Green team's), the survivors keep their own colours, and the kit is
// always the full one.
static void TestKit() {
    CHECK(KitTeamFor(Format::Infection, kInfectedTeam) == kInfectedKitTeam && KitTeamFor(Format::Infection, kSurvivorTeam) == kNoTeam);
    CHECK(KitTeamFor(Format::TwoTeams, 1) == 1 && KitTeamFor(Format::Tag, kNoTeam) == kNoTeam);
    const Rgba g = TeamColor(kInfectedKitTeam), green = TeamColor(2);
    CHECK(g.a == 1.f && g.g > g.r && g.g > g.b && g.g < green.g && g.r != green.r);
    CHECK(TeamColor(4).a == 0.f && TeamColor(5).a == 0.f && TeamColor(7).a == 0.f);     // no other team index changed
    CHECK(EffectiveKit(Format::Infection, TeamKit::Off) == TeamKit::Full && EffectiveKit(Format::TwoTeams, TeamKit::Off) == TeamKit::Off);
    const KitColors k = KitColorsFor(kInfectedKitTeam);
    CHECK(k.base.a == 1.f && k.plate.g < k.base.g);
    CHECK(KitColor(TeamKit::Full, KitSlot::Cloth, kInfectedKitTeam).g == g.g && KitColor(TeamKit::Full, KitSlot::Blade, kInfectedKitTeam).a == 0.f);
    CHECK(style::TextContrast(style::kInfectedInk) > 4.5);
}

static MatchInfo Info(MatchPhase ph, std::vector<int8_t> teams, std::vector<bool> alive) {
    MatchInfo m; m.format = uint8_t(Format::Infection); m.phase = uint8_t(ph); m.timeLimitS = 300; m.timeLeftMs = 61000;
    const char* names[] = {"Ann", "Bo", "Cy", "Di"};
    int surv = 0, inf = 0;
    for (size_t i = 0; i < teams.size(); ++i) {
        FighterEntry f; f.entity = uint8_t(i + 1); f.team = teams[i]; f.name = names[i]; f.alive = alive[i];
        (teams[i] == kInfectedTeam ? inf : surv) += teams[i] == kInfectedTeam || alive[i];
        m.fighters.push_back(f);
    }
    m.scores = {{kSurvivorTeam, int16_t(surv)}, {kInfectedTeam, int16_t(inf)}};
    return m;
}

// Criterion: survivors and infected are named as such, in their inks, on the strip, the pill, the board and the results; the dead
// survivor reads ELIMINATED, the dead infected RESPAWNING.
static void TestHud() {
    const MatchInfo m = Info(MatchPhase::Live, {1, 0, 0, 0}, {true, true, true, false});
    LocalHud l; l.me = 2; l.host = 1; l.infoAtMs = 1000;
    HudScene s = BuildHudScene(m, l, 1000);
    CHECK(s.strip.size() == 2 && s.strip[0].label == "SURVIVORS" && s.strip[0].score == 2 && s.strip[1].label == "INFECTED" && s.strip[1].score == 1);
    CHECK(s.strip[0].mine && !s.strip[1].mine);
    CHECK(s.strip[1].color.g == style::kInfectedInk.g && s.strip[0].color.r == style::kBone.r);
    CHECK(s.pill.text == "SURVIVE \xC2\xB7 2 LEFT");
    l.me = 1; s = BuildHudScene(m, l, 1000);
    CHECK(s.pill.text == "YOU ARE INFECTED" && s.pill.color.g == style::kInfectedInk.g);
    CHECK(FighterInk(m, 1).g == style::kInfectedInk.g && FighterInk(m, 2).r == style::kBone.r);
    CHECK(NameTagInk(Format::Infection, kInfectedTeam, false).g == style::kInfectedInk.g && NameTagInk(Format::Infection, kSurvivorTeam, false).r == style::kBone.r);
    CHECK(Has(StatusLine(m, 2), "SURVIVORS 2   INFECTED 1") && !Has(StatusLine(m, 2), "first to") && Has(StatusLine(m, 2), "1:01"));
    l.tab = true; s = BuildHudScene(m, l, 1000);
    int bands = 0; for (const BoardRow& r : s.board.rows) if (r.kind == BoardRow::Kind::Band) { ++bands; CHECK(r.name == "SURVIVORS" || r.name == "INFECTED"); }
    CHECK(bands == 2 && Has(s.board.title, "INFECTION") && Has(s.board.title, "SURVIVE THE CLOCK"));
    // The dead panel.
    l.tab = false; l.me = 4; l.dead = true; l.diedAtMs = 500;
    s = BuildHudScene(m, l, 1000);
    CHECK(s.dead.visible && s.dead.title == "ELIMINATED" && Has(s.dead.detail, "rematch"));
    MatchInfo n = Info(MatchPhase::Live, {1, 0, 0, 1}, {true, true, true, false});     // Di was infected
    s = BuildHudScene(n, l, 1000);
    CHECK(s.dead.visible && s.dead.title == "RESPAWNING");
    // The results.
    MatchInfo r = Info(MatchPhase::Results, {1, 0}, {true, true}); r.winner = kInfectedTeam;
    CHECK(ResultShot(r, 2).text == "INFECTED WIN" && Has(StatusLine(r, 2), "INFECTED WIN"));
    r.winner = kSurvivorTeam;
    CHECK(ResultShot(r, 2).text == "SURVIVORS WIN" && Has(StatusLine(r, 2), "SURVIVORS WIN"));
    // The FIGHT banner names the side; a survivor turned infected gets a banner.
    CHECK(FightSub(m, 1) == "YOU ARE A PATIENT ZERO" && FightSub(m, 2) == "SURVIVE");
    BannerTracker b;
    b.Observe(Info(MatchPhase::Live, {1, 0, 0}, {true, true, true}), 2, 100);
    b.Observe(Info(MatchPhase::Live, {1, 0, 0}, {true, false, true}), 2, 200);
    CHECK(b.shot().text.empty());                                                     // dead, not yet infected
    b.Observe(Info(MatchPhase::Live, {1, 1, 0}, {true, false, true}), 2, 300);
    CHECK(b.shot().text == "INFECTED" && b.shot().atMs == 300);
    BannerTracker j; j.Observe(Info(MatchPhase::Live, {1, 1}, {true, true}), 2, 50);
    CHECK(j.shot().text.empty());
    // Feed.
    CHECK(ModeEventLine(m, ModeEventMsg{ModeEventKind::Infected, 1, 2}) == "Ann infected Bo");
    CHECK(ModeEventLine(m, ModeEventMsg{ModeEventKind::Infected, 0, 2}) == "Bo was infected");
    const FeedRow inf = MakeModeFeedRow(m, ModeEventMsg{ModeEventKind::Infected, 1, 2}, 2);
    CHECK(inf.spans.size() == 3 && inf.spans[1].text == " infected " && inf.mine && inf.spans[0].color.g == style::kInfectedInk.g);
    const FeedRow was = MakeModeFeedRow(m, ModeEventMsg{ModeEventKind::Infected, 0, 2}, 3);
    CHECK(was.spans.size() == 2 && was.spans[1].text == " was infected" && !was.mine);
}

static LobbyLocal InLobby(Format f, int players = 4) {
    LobbyLocal l; l.phase = LobbyPhase::InLobby; l.host = true; l.config.format = f; l.config = Sanitize(l.config); l.playerCount = players;
    return l;
}
static std::vector<LobbyRow> RowIds(const LobbyScene& s) { std::vector<LobbyRow> v; for (const SettingRow& r : s.settings) v.push_back(r.id); return v; }

// Criterion: the card, its rows (Time, Respawn, Damage, FreeCam, Sync, Gear/Tier/Weapons; no Teams, no Crossbows, no ground clear),
// the clock's stops without "no clock", 2+ players.
static void TestLobby() {
    LobbyScene s = BuildLobbyScene(InLobby(Format::Infection));
    const ModeCard* card = nullptr;
    for (const ModeCard& m : s.modes) if (m.format == Format::Infection) card = &m;
    CHECK(card && card->name == "INFECTION" && card->selected && card->enabled && card->group == ModeGroup::Versus && !card->description.empty());
    CHECK(RowIds(s) == (std::vector<LobbyRow>{LobbyRow::Time, LobbyRow::Respawn, LobbyRow::FreeCam, LobbyRow::Gear, LobbyRow::Tier, LobbyRow::Bodies, LobbyRow::Damage, LobbyRow::DeadWeight, LobbyRow::WeaponWeight, LobbyRow::Sync}));
    for (const SettingRow& r : s.settings) {
        if (r.id == LobbyRow::Time) CHECK(r.label == "Time limit" && r.value == "5 min" && Has(r.hint, "survivors"));
        if (r.id == LobbyRow::Respawn) CHECK(r.label == "Respawn delay" && Has(r.hint, "infected"));
    }
    MatchConfig c = Sanitize(InLobby(Format::Infection).config);
    CHECK(StepSetting(c, LobbyRow::Time, 1).timeLimitS == 600 && StepSetting(c, LobbyRow::Time, -1).timeLimitS == 1200);   // never 0
    c.timeLimitS = 1200; CHECK(StepSetting(c, LobbyRow::Time, 1).timeLimitS == 300);
    c.format = Format::FreeForAll; c.timeLimitS = 300; CHECK(StepSetting(c, LobbyRow::Time, -1).timeLimitS == 0);       // other modes keep "no clock"
    CHECK(!ModeAvailable(Format::Infection, 1).ok && ModeAvailable(Format::Infection, 2).ok);
    LobbyLocal rolled = InLobby(Format::Infection); rolled.config.gear = Gear::Mirror;
    CHECK(RowIds(BuildLobbyScene(rolled)).size() == 10);
    LobbyLocal withPlayers = InLobby(Format::Infection);
    LobbyPlayer ann, bo; ann.name = "Ann"; ann.host = ann.me = true; bo.name = "Bo";
    withPlayers.players = {ann, bo};
    s = BuildLobbyScene(withPlayers);
    CHECK(s.roster.size() == 2 && s.roster[0].team.empty() && !s.balance);               // no team picks, no auto-balance
    CHECK(Has(s.summary, "INFECTION"));
    bool browser = false; for (const std::string& n : BrowserModes()) browser = browser || n == "Infection";
    CHECK(browser && Has(FormatBlurb(Format::Infection), "Patient zero"));
    CHECK(ModeName(Format::Infection, Gear::Own, 3) == "Infection");
}

int main() {
    TestZeroCount(); TestAssign(); TestInfectsOnKill(); TestInfectionAndRespawn(); TestWinConditions(); TestJoinAndCountdown(); TestRematch();
    TestConfig(); TestTraits(); TestKit(); TestHud(); TestLobby();
    if (g_fail) { std::printf("%d check(s) failed\n", g_fail); return 1; }
    std::printf("infection tests passed\n");
    return 0;
}
