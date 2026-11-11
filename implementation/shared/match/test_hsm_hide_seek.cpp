// Hide and Seek (#1082; match/hsm_modes.h HideSeekGame): rounds of seekers against hiders, dealt anew each round. The
// format's tables and its two clocks, the roles and their deal, the game rule, the match's rounds, who is held and
// whose name tag is hidden, the wire, the HUD and the lobby.
#include "match/hsm_crossbow.h"
#include "match/hsm_hud_scene.h"
#include "match/hsm_lobby_scene.h"
#include "match/hsm_maps.h"
#include "match/hsm_modes.h"
#include "match/hsm_nametag.h"
#include "match/hsm_roles.h"
#include "protocol/hsm_protocol.h"
#include "session/hsm_autoflow.h"
#include <cstdio>
#include <set>
#include <string>
#include <vector>

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++g_fail; } } while (0)
using namespace hsm;
using Phase = HideSeekGame::Phase;
using Outcome = HideSeekGame::Outcome;

static MatchConfig Cfg() { MatchConfig c; c.format = Format::HideSeek; return Sanitize(c); }
static bool Has(const std::string& s, const char* part) { return s.find(part) != std::string::npos; }
static MatchConfig With(uint16_t timeS, uint8_t fuseS) { MatchConfig o; o.format = Format::HideSeek; o.timeLimitS = timeS; o.fuseS = fuseS; o.crossbows = 4; return Sanitize(o); }

// The format: rounds, roles without teams; the Time row is a round's Seek time and the Fuse row the Hide time.
static void TestFormat() {
    const Format f = Format::HideSeek;
    CHECK(uint8_t(f) == 19 && kFormatCount == 20 && IsHideSeek(f) && !IsHideSeek(Format::Roulette) && !IsHideSeek(Format::Tag));
    CHECK(IsRoundFormat(f) && HasRoles(f) && !HasRoleTeams(f) && !HasTeams(f) && !IsQueueFormat(f) && ScoresPerFighter(f) && DefaultScoreLimit(f) == 3);
    CHECK(!IsRoundFormat(Format::FreeForAll) && !HasRoles(Format::LastManStanding) && HasRoles(Format::Chaos) && HasRoles(Format::Riot));
    const MatchConfig c = Cfg();
    CHECK(c.gear == Gear::RandomTier && c.timeLimitS == kSeekDefaultS && kSeekDefaultS == 300 && c.fuseS == 30 && c.crossbows == 0);
    CHECK(With(0, 30).timeLimitS == 300 && With(60, 30).timeLimitS == 120 && With(120, 30).timeLimitS == 120 && With(420, 30).timeLimitS == 420);
    CHECK(With(600, 30).timeLimitS == 600 && With(3600, 30).timeLimitS == 600);
    CHECK(With(300, 5).fuseS == 15 && With(300, 15).fuseS == 15 && With(300, 45).fuseS == 45 && With(300, 60).fuseS == 60 && With(300, 200).fuseS == 60);
    CHECK(With(300, 30).crossbows == 0 && kHideMinS == 15 && kHideMaxS == 60 && kSeekMinS == 120 && kSeekMaxS == 600);
    MatchConfig lms; lms.format = Format::LastManStanding; lms.timeLimitS = 60; lms.fuseS = 10; lms.crossbows = 4;   // another format keeps its own
    const MatchConfig s = Sanitize(lms);
    CHECK(s.timeLimitS == 60 && s.fuseS == 10 && s.crossbows == 4);
}

// The roles: peasants both; the seekers with a two-handed sword, the hiders with nothing. One seeker, two with six
// fighters or more, drawn anew for every deal.
static void TestRoles() {
    CHECK(kRoleSeeker == 71 && kRoleHider == 72 && kRoleCount == 73 && ValidRoleId(kRoleSeeker) && ValidRoleId(kRoleHider) && !ValidRoleId(73));
    CHECK(!IsChaosRole(kRoleSeeker) && !IsChaosRole(kRoleHider) && IsChaosRole(kRoleChaosLast));
    const MatchConfig c = Cfg();
    const FighterRole s = RoleSpec(kRoleSeeker, c), h = RoleSpec(kRoleHider, c);
    CHECK(s.id == kRoleSeeker && s.tier == 1 && s.gear == Gear::Mirror && s.weapons == WeaponBit(2) && GameWeaponTypeByte(s.weapons, 5) == 2 && !s.bare);
    CHECK(s.weight == TierWeight(1) && s.height == kRoleUnchanged && kHideSeekTier == 1 && kSeekerWeaponType == 2);
    CHECK(h.id == kRoleHider && h.tier == 1 && h.gear == Gear::Mirror && h.weapons == 0 && h.bare && h.weight == TierWeight(1) && h.height == kRoleUnchanged);
    CHECK(!RoleSpec(kRoleKnight, c).bare && !RoleSpec(kRoleFighter, c).bare && !RoleSpec(kRoleChaosFirst, c).bare && !RoleSpec(kRoleSkirmisher, c).bare);
    CHECK(SeekerCount(2) == 1 && SeekerCount(3) == 1 && SeekerCount(5) == 1 && SeekerCount(6) == 2 && SeekerCount(20) == 2);
    CHECK(std::string(RoleTag(kRoleSeeker)) == "SEEKER" && std::string(RoleTag(kRoleHider)).empty() && std::string(RoleTag(kRoleKnight)) == "KNIGHT" && std::string(RoleTag(kRoleJuggernaut)) == "JUGGERNAUT");
    CHECK(RoleLine(kRoleSeeker) == "YOU SEEK" && RoleLine(kRoleHider) == "HIDE" && RoleLine(kRoleKnight) == "YOU ARE THE KNIGHT");
    CHECK(std::string(RoleName(kRoleSeeker)) == "seeker" && std::string(RoleName(kRoleHider)) == "hider" && std::string(RoleName(kRoleChaosFirst)) == "chaos");
    auto seekersOf = [&](const std::vector<uint8_t>& ids, uint32_t seed, uint32_t deal) {
        std::set<uint8_t> out;
        for (const auto& [e, r] : AssignRoles(Format::HideSeek, c, ids, seed, deal)) {
            CHECK(r.team == kNoTeam && (r.id == kRoleSeeker || r.id == kRoleHider) && r.bare == (r.id == kRoleHider));
            if (r.id == kRoleSeeker) out.insert(e);
        }
        return out;
    };
    const std::vector<uint8_t> five = {1, 2, 3, 4, 5}, six = {1, 2, 3, 4, 5, 6}, three = {4, 7, 9}, none, one = {3}, two = {3, 8};
    std::set<uint8_t> seen; std::set<std::set<uint8_t>> pairs; int moved = 0, other = 0;
    for (uint32_t deal = 0; deal < 60; ++deal) {
        const std::set<uint8_t> s5 = seekersOf(five, 77, deal), s6 = seekersOf(six, 77, deal), s3 = seekersOf(three, 77, deal);
        CHECK(s5.size() == 1 && s6.size() == 2 && s3.size() == 1 && AssignRoles(Format::HideSeek, c, six, 77, deal).size() == 6);
        CHECK(seekersOf(five, 77, deal) == s5);                                 // the same deal again: the same seekers
        seen.insert(s5.begin(), s5.end()); pairs.insert(s6);
        if (deal && s5 != seekersOf(five, 77, deal - 1)) ++moved;
        if (s5 != seekersOf(five, 78, deal)) ++other;
        for (uint8_t e : s3) CHECK(e == 4 || e == 7 || e == 9);
        const uint8_t a = *s6.begin(), b = *s6.rbegin();
        CHECK(b == a + 1 || (a == 1 && b == 6));                                // two in a row, round the ring
    }
    CHECK(seen == std::set<uint8_t>({1, 2, 3, 4, 5}) && pairs.size() >= 4 && moved > 30 && other > 30);   // drawn anew: no fixed order, and another lobby draws otherwise
    CHECK(seekersOf(none, 77, 0).empty() && seekersOf(one, 77, 0).empty() && seekersOf(two, 77, 0).size() == 1);   // nobody to seek: no seeker
    const FighterRole late = LateJoinerRole(Format::HideSeek, c, 9, 77, 3);
    CHECK(late.id == kRoleHider && late.team == kNoTeam && late.bare);
    CHECK(LateJoinerRole(Format::Chaos, c, 9, 77, 3).id != kRoleHider && LateJoinerRole(Format::LastManStanding, c, 9, 77, 3).id == kRoleFighter);
}

// Who is held, and whose name tag is hidden.
static void TestHeldAndTags() {
    const Format f = Format::HideSeek;
    CHECK(SeekerHeld(f, MatchPhase::Live, kRoleSeeker, true, 1) && SeekerHeld(f, MatchPhase::SuddenDeath, kRoleSeeker, true, 30000));
    CHECK(!SeekerHeld(f, MatchPhase::Live, kRoleSeeker, true, 0));              // the Hide time is over
    CHECK(!SeekerHeld(f, MatchPhase::Live, kRoleHider, true, 1) && !SeekerHeld(f, MatchPhase::Live, kRoleFighter, true, 1));
    CHECK(!SeekerHeld(f, MatchPhase::Live, kRoleSeeker, false, 1));             // down
    CHECK(!SeekerHeld(f, MatchPhase::Countdown, kRoleSeeker, true, 1) && !SeekerHeld(f, MatchPhase::Results, kRoleSeeker, true, 1) && !SeekerHeld(f, MatchPhase::Idle, kRoleSeeker, true, 1));
    CHECK(!SeekerHeld(Format::Chaos, MatchPhase::Live, kRoleSeeker, true, 1) && !SeekerHeld(Format::Tag, MatchPhase::Live, kRoleSeeker, true, 5000));
    CHECK(NameTagHidden(f, kRoleHider) && !NameTagHidden(f, kRoleSeeker) && !NameTagHidden(f, kRoleFighter));
    CHECK(!NameTagHidden(Format::Chaos, kRoleHider) && !NameTagHidden(Format::LastManStanding, kRoleHider));
}

// The game rule: the Hide time, the Seek time, and whose round it is.
static void TestGame() {
    HideSeekGame g;
    CHECK(!g.active() && g.phase() == Phase::Off && g.FirstSeeker() == 0 && g.HideLeftMs(5) == 0 && g.RoundLeftMs(5) == 0);
    HideSeekGame::Step st = g.Update({1, 2, 3}, 100);
    CHECK(!st.released && st.outcome == Outcome::None && st.winners.empty() && !g.active());
    g.BeginRound({2}, 1000, 30, 300);
    CHECK(g.active() && g.phase() == Phase::Hiding && g.IsSeeker(2) && !g.IsSeeker(1) && g.FirstSeeker() == 2);
    CHECK(g.HideLeftMs(1000) == 30000 && g.HideLeftMs(30999) == 1 && g.HideLeftMs(31000) == 0 && g.HideLeftMs(99999) == 0);
    CHECK(g.RoundLeftMs(1000) == 330000 && g.RoundLeftMs(330999) == 1 && g.RoundLeftMs(331000) == 0 && g.RoundLeftMs(400000) == 0);
    st = g.Update({1, 2, 3}, 30999);
    CHECK(!st.released && st.outcome == Outcome::None && g.phase() == Phase::Hiding);
    st = g.Update({1, 2, 3}, 31000);
    CHECK(st.released && st.outcome == Outcome::None && g.phase() == Phase::Seeking && g.HideLeftMs(31000) == 0 && g.RoundLeftMs(31000) == 300000);
    st = g.Update({1, 2, 3}, 31001);
    CHECK(!st.released && st.outcome == Outcome::None);                        // let go once
    st = g.Update({2, 3}, 100000);
    CHECK(st.outcome == Outcome::None && g.active());                          // a hider down: the round goes on
    st = g.Update({2, 3}, 330999);
    CHECK(st.outcome == Outcome::None);
    st = g.Update({2, 3}, 331000);                                             // the Seek time is up: the hiders still standing
    CHECK(st.outcome == Outcome::Hiders && st.winners == std::vector<uint8_t>({3}) && !g.active() && g.phase() == Phase::Off && !g.IsSeeker(2) && g.FirstSeeker() == 0);
    st = g.Update({2, 3}, 331001);
    CHECK(st.outcome == Outcome::None && st.winners.empty());                  // over: nothing more
    g.BeginRound({2, 5}, 0, 15, 120);                                          // every hider down: the seekers standing
    CHECK(g.FirstSeeker() == 2 && g.IsSeeker(5) && g.HideLeftMs(0) == 15000 && g.RoundLeftMs(0) == 135000);
    st = g.Update({1, 2, 5}, 20000); CHECK(st.released && st.outcome == Outcome::None);
    st = g.Update({5, 2}, 20001);
    CHECK(st.outcome == Outcome::Seekers && st.winners == std::vector<uint8_t>({5, 2}) && !g.active());
    g.BeginRound({2}, 0, 30, 300);                                             // while the seekers still wait: theirs all the same
    st = g.Update({2}, 5000); CHECK(st.outcome == Outcome::Seekers && !st.released && st.winners == std::vector<uint8_t>({2}));
    g.BeginRound({2}, 0, 30, 300);                                             // no seeker standing: the hiders standing
    st = g.Update({1, 3}, 5000); CHECK(st.outcome == Outcome::Hiders && st.winners == std::vector<uint8_t>({1, 3}));
    g.BeginRound({2}, 0, 30, 300);                                             // nobody standing: a round without a winner
    st = g.Update({}, 5000); CHECK(st.outcome == Outcome::Seekers && st.winners.empty() && !g.active());
    g.BeginRound({2}, 0, 30, 300);                                             // the last hider falls as the Seek time ends: the seekers'
    st = g.Update({2}, 330000); CHECK(st.outcome == Outcome::Seekers && st.released);
    g.BeginRound({2}, 0, 30, 300); g.EndRound(); CHECK(!g.active() && !g.IsSeeker(2) && g.RoundLeftMs(0) == 0 && g.HideLeftMs(0) == 0);
    g.BeginRound({2}, 0, 30, 300); g.BeginRound({3}, 100, 30, 300);            // a new round forgets the last one's seekers
    CHECK(g.IsSeeker(3) && !g.IsSeeker(2) && g.HideLeftMs(100) == 30000 && g.phase() == Phase::Hiding);
    st = g.Update({1, 3}, 40000); CHECK(st.released);
    g.BeginRound({3}, 50000, 30, 300); CHECK(g.phase() == Phase::Hiding && g.HideLeftMs(50000) == 30000);   // and its Seek phase
}

// The match: one standing ends nothing; the game rule's winners take a point each; the Seek time is no match clock.
static void TestMatch() {
    MatchConfig c = Cfg(); c.scoreLimit = 2;
    Match m; m.Start(c, {{1, kNoTeam, true}, {2, kNoTeam, true}, {3, kNoTeam, true}, {4, kNoTeam, true}}, 0);
    const uint32_t t = Match::kCountdownMs;
    const std::vector<uint8_t> none, one = {1}, pair = {2, 3}, ghost = {9}, tie = {1, 2};
    CHECK(!m.AwardRound(one, 10) && m.Score(1) == 0);                          // the countdown is no round
    m.Tick(t);
    m.OnDeath(2, 1, t + 1); m.OnDeath(3, 1, t + 2); m.OnDeath(4, 1, t + 3);
    CHECK(m.RoundInPlay() && m.Score(1) == 0 && m.roundWinner() == 0);         // one standing ends nothing here
    CHECK(m.AwardRound(one, t + 4) && !m.RoundInPlay() && m.Score(1) == 1 && m.roundWinner() == 1 && m.phase() == MatchPhase::Live);
    CHECK(!m.AwardRound(one, t + 5) && m.Score(1) == 1);                       // once a round
    auto ev = m.Tick(t + 4 + 5000);
    CHECK(ev.roundReset && m.round() == 2 && m.RoundInPlay());
    CHECK(m.AwardRound(pair, t + 6000) && m.Score(2) == 1 && m.Score(3) == 1 && m.roundWinner() == 0 && m.phase() == MatchPhase::Live);   // a point each
    ev = m.Tick(t + 6000 + 5000); CHECK(ev.roundReset && m.round() == 3);
    CHECK(m.AwardRound(none, t + 12000) && !m.RoundInPlay() && m.Score(1) == 1 && m.roundWinner() == 0);   // a round without a winner
    ev = m.Tick(t + 12000 + 5000); CHECK(ev.roundReset && m.round() == 4);
    CHECK(m.AwardRound(ghost, t + 18000) && m.Score(9) == 0 && m.phase() == MatchPhase::Live);             // not a fighter: no point
    ev = m.Tick(t + 18000 + 5000); CHECK(ev.roundReset);
    CHECK(m.AwardRound(tie, t + 24000) && m.Score(1) == 2 && m.Score(2) == 2 && m.phase() == MatchPhase::Live);   // the limit, tied at the top: another round
    ev = m.Tick(t + 24000 + 5000); CHECK(ev.roundReset);
    CHECK(m.AwardRound(one, t + 30000) && m.Score(1) == 3 && m.phase() == MatchPhase::Results && m.winner() == 1);
    Match q; MatchConfig qc = Cfg(); qc.scoreLimit = 1;                         // the limit reached ends the match at once
    q.Start(qc, {{1, kNoTeam, true}, {2, kNoTeam, true}, {3, kNoTeam, true}}, 0); q.Tick(t);
    CHECK(q.AwardRound(one, t + 1) && q.Score(1) == 1 && q.phase() == MatchPhase::Results && q.winner() == 1);
    Match far; MatchConfig lc = Cfg(); lc.timeLimitS = 120;                    // the Seek time is a round's clock: the match does not end on it
    far.Start(lc, {{1, kNoTeam, true}, {2, kNoTeam, true}, {3, kNoTeam, true}}, 0); far.Tick(t);
    far.Tick(t + 121000); far.Tick(t + 700000);
    CHECK(far.phase() == MatchPhase::Live && far.RoundInPlay());
    MatchConfig lms; lms.format = Format::LastManStanding; lms.timeLimitS = 120;   // Last Man Standing's clock still ends its match; it has no such round
    Match o; o.Start(Sanitize(lms), {{1, kNoTeam, true}, {2, kNoTeam, true}, {3, kNoTeam, true}}, 0); o.Tick(t);
    CHECK(!o.AwardRound(one, t + 1) && o.Score(1) == 0 && o.RoundInPlay());
    o.OnDeath(2, 1, t + 2); o.OnDeath(3, 1, t + 3);
    CHECK(!o.RoundInPlay() && o.Score(1) == 1);                                // and its rounds still go to the last one standing
    Match clock; clock.Start(Sanitize(lms), {{1, kNoTeam, true}, {2, kNoTeam, true}}, 0); clock.Tick(t);
    clock.Tick(t + 121000); CHECK(clock.phase() != MatchPhase::Live);
}

// The wire: the two role ids travel in the MatchInfo, the Hide time in the fuse and the round's clock in the time left.
static void TestWire() {
    MatchInfo m; m.format = uint8_t(Format::HideSeek); m.teams = 2; m.timeLimitS = 300; m.timeLeftMs = 330000; m.tagIt = 1; m.fuseLeftMs = 30000;
    const std::vector<uint8_t> ids = {kRoleSeeker, kRoleHider, kRoleHider};
    for (size_t i = 0; i < ids.size(); ++i) { FighterEntry f; f.entity = uint8_t(i + 1); f.name = "p"; f.role = ids[i]; m.fighters.push_back(f); }
    auto bytes = Encode(m); MatchInfo o;
    CHECK(Decode(bytes.data(), bytes.size(), o) && o.format == uint8_t(Format::HideSeek) && o.fighters.size() == 3 && o.fighters[0].role == kRoleSeeker && o.fighters[2].role == kRoleHider);
    CHECK(o.tagIt == 1 && o.fuseLeftMs == 30000 && o.timeLeftMs == 330000 && o.timeLimitS == 300);
    MatchInfo bad = m; bad.fighters[1].role = 73; bytes = Encode(bad); CHECK(!Decode(bytes.data(), bytes.size(), o));
    bad = m; bad.format = 20; bytes = Encode(bad); CHECK(!Decode(bytes.data(), bytes.size(), o));
}

static MatchInfo Info(uint16_t hideLeftMs) {
    MatchInfo m; m.format = uint8_t(Format::HideSeek); m.phase = uint8_t(MatchPhase::Live); m.round = 2; m.scoreLimit = 3; m.tagIt = 1; m.fuseLeftMs = hideLeftMs;
    m.timeLimitS = 300; m.timeLeftMs = 300000u + hideLeftMs;
    m.fighters = {{1, kNoTeam, 0, 0, "Ann", 0, true, false, 0, kRoleSeeker}, {2, kNoTeam, 0, 0, "Bo", 0, true, false, 0, kRoleHider}, {3, kNoTeam, 0, 0, "Cy", 0, false, false, 0, kRoleHider}};
    return m;
}

// The HUD: what the round pill says to a seeker, a hider and a watcher, the clock and the board.
static void TestScreens() {
    const MatchInfo hiding = Info(22500), seeking = Info(0);
    CHECK(HideSeekLine(hiding, 1) == "WAIT 23 s" && HideSeekLine(hiding, 2) == "HIDE 23 s" && HideSeekLine(hiding, 3) == "HIDING 23 s" && HideSeekLine(hiding, 9) == "HIDING 23 s");
    CHECK(HideSeekLine(seeking, 1) == "SEEK" && HideSeekLine(seeking, 2) == "STAY HIDDEN" && HideSeekLine(seeking, 3) == "SEEKING" && HideSeekLine(seeking, 9) == "SEEKING");
    { MatchInfo n = hiding; n.phase = uint8_t(MatchPhase::Results); CHECK(HideSeekLine(n, 1).empty()); }
    { MatchInfo n = seeking; n.fighters[0].alive = false; CHECK(HideSeekLine(n, 1) == "SEEKING"); }     // a seeker that is down only watches
    { MatchInfo n = hiding; n.fighters[0].alive = false; CHECK(HideSeekLine(n, 1) == "HIDING 23 s"); }
    { MatchInfo n = hiding; n.phase = uint8_t(MatchPhase::SuddenDeath); CHECK(HideSeekLine(n, 1) == "WAIT 23 s"); }
    { MatchInfo n = hiding; n.format = uint8_t(Format::Tag); CHECK(HideSeekLine(n, 1).empty()); }
    LocalHud l; l.me = 2; l.host = 1; l.infoAtMs = 1000;
    HudScene s = BuildHudScene(hiding, l, 1000);
    CHECK(s.pill.text == "ROUND 2 \xC2\xB7 HIDE 23 s" && s.timeBar && !s.clock.empty());
    s = BuildHudScene(hiding, l, 3000);
    CHECK(s.pill.text == "ROUND 2 \xC2\xB7 HIDE 21 s");                        // the clock runs on between MatchInfos
    s = BuildHudScene(seeking, l, 1000);
    CHECK(s.pill.text == "ROUND 2 \xC2\xB7 STAY HIDDEN");
    l.me = 1; s = BuildHudScene(seeking, l, 1000);
    CHECK(s.pill.text == "ROUND 2 \xC2\xB7 SEEK");
    { MatchInfo n = seeking; n.format = uint8_t(Format::LastManStanding); s = BuildHudScene(n, l, 1000); CHECK(s.pill.text == "ROUND 2"); }
    l.tab = true; s = BuildHudScene(seeking, l, 1000);
    int tags = 0; for (const BoardRow& r : s.board.rows) if (r.tag == "SEEKER") { ++tags; CHECK(r.name == "Ann"); }
    CHECK(tags == 1);
}

// The lobby: its card, its rows with their own stops and names, three players, Slums; the two clocks come and go with it.
static void TestLobby() {
    const Format f = Format::HideSeek;
    CHECK(FormatName(f) == "Hide and seek" && Has(FormatBlurb(f), "Slums or a custom map"));
    CHECK(SettingsFor(Cfg()) == std::vector<LobbyRow>({LobbyRow::Limit, LobbyRow::Time, LobbyRow::Fuse, LobbyRow::Respawn, LobbyRow::FreeCam, LobbyRow::Damage, LobbyRow::DeadWeight,
                                                      LobbyRow::WeaponWeight, LobbyRow::Sync}));
    CHECK(!ModeAvailable(f, 2).ok && ModeAvailable(f, 3).ok && ModeAvailable(f, 20).ok && std::string(ModeAvailable(f, 2).hint) == "Needs 3 or more players");
    bool card = false, browser = false;
    for (size_t i = 0; i < kModeOrderCount; ++i) card = card || kModeOrder[i] == f;
    for (const std::string& n : BrowserModes()) browser = browser || n == "Hide and seek";
    CHECK(card && browser && kModeOrderCount == 18 && kModeCardCount == 20);
    Format parsed = Format::FreeForAll;
    CHECK(ParseServerFormat("hideseek", &parsed) && parsed == f);
    int allowed = 0;
    for (const MapInfo& m : kMaps) if (MapAllowsFormat(m, f)) { ++allowed; CHECK(std::string(m.display) == "Slums" && std::string(m.path) == kSlumsMapPath); }
    CHECK(allowed == 1);
    MatchConfig c = Cfg();                                                     // the rows step through their own stops
    std::vector<int> seek, hide;
    for (int i = 0; i < 6; ++i) { seek.push_back(c.timeLimitS); c = StepSetting(c, LobbyRow::Time, 1); }
    CHECK(seek == std::vector<int>({300, 420, 600, 120, 180, 240}) && c.timeLimitS == 300);
    for (int i = 0; i < 5; ++i) { hide.push_back(c.fuseS); c = StepSetting(c, LobbyRow::Fuse, 1); }
    CHECK(hide == std::vector<int>({30, 45, 60, 15, 20}) && c.fuseS == 30);
    CHECK(StepSetting(Cfg(), LobbyRow::Time, -1).timeLimitS == 240 && StepSetting(Cfg(), LobbyRow::Fuse, -1).fuseS == 20 && StepSetting(Cfg(), LobbyRow::Time, 1).fuseS == 30);
    MatchConfig tag; tag.format = Format::Tag; tag.fuseS = 60;                 // Tag's fuse and another format's clock keep their own stops
    MatchConfig lms; lms.format = Format::LastManStanding;
    CHECK(StepSetting(tag, LobbyRow::Fuse, 1).fuseS == 10 && StepSetting(lms, LobbyRow::Time, 1).timeLimitS == 300 && StepSetting(lms, LobbyRow::Time, -1).timeLimitS == 1200);
    LobbyLocal l; l.phase = LobbyPhase::InLobby; l.host = true; l.myName = "Ann"; l.version = "0.2.0"; l.config = Cfg();
    for (const char* name : {"Ann", "Bob", "Cat"}) { LobbyPlayer p; p.name = name; p.me = l.players.empty(); p.host = l.players.empty(); l.players.push_back(p); }
    const SettingRow time = lobby_detail::Row(l, LobbyRow::Time), fuse = lobby_detail::Row(l, LobbyRow::Fuse);
    CHECK(time.label == "Seek time" && time.value == "5 min" && Has(time.hint, "hiders still standing"));
    CHECK(fuse.label == "Hide time" && fuse.value == "30 s" && Has(fuse.hint, "stand still"));
    { LobbyLocal o = l; o.config = Sanitize(tag); CHECK(lobby_detail::Row(o, LobbyRow::Fuse).label == "Fuse time" && !Has(lobby_detail::Row(o, LobbyRow::Fuse).hint, "stand still")); }
    { LobbyLocal o = l; o.config = Sanitize(lms); CHECK(lobby_detail::Row(o, LobbyRow::Time).label == "Time limit" && !Has(lobby_detail::Row(o, LobbyRow::Time).hint, "hiders")); }
    MatchConfig from; from.timeLimitS = 1200; from.fuseS = 45;                 // picking the card, and leaving it
    const MatchConfig in = SelectMode(from, f, false);
    CHECK(in.format == f && in.timeLimitS == kSeekDefaultS && in.fuseS == 45 && in.scoreLimit == 3 && Sanitize(in).gear == Gear::RandomTier);
    MatchConfig set = in; set.timeLimitS = 600; set.fuseS = 60;
    const MatchConfig out = SelectMode(set, Format::LastManStanding, false);
    CHECK(out.format == Format::LastManStanding && out.timeLimitS == MatchConfig{}.timeLimitS && out.fuseS == MatchConfig{}.fuseS);
    CHECK(SelectMode(set, f, false).timeLimitS == 600 && SelectMode(set, f, false).fuseS == 60);   // the same card again: nothing changes
    CHECK(SelectMode(set, Format::Tag, false).fuseS == MatchConfig{}.fuseS && SelectMode(set, Format::FreeForAll, true).timeLimitS == MatchConfig{}.timeLimitS);
    MatchConfig keep = SelectMode(MatchConfig{}, Format::LastManStanding, false); keep.timeLimitS = 900; keep.fuseS = 45;   // other cards leave the clock and the fuse alone
    CHECK(SelectMode(keep, Format::Duel, false).timeLimitS == 900 && SelectMode(keep, Format::Duel, false).fuseS == 45);
    const LobbyScene s = BuildLobbyScene(l);
    bool selected = false;
    for (const ModeCard& m : s.modes) if (m.format == f && !m.brawl && !m.progression) selected = m.selected && m.name == "HIDE AND SEEK" && m.enabled;
    CHECK(selected && s.summary.find("HIDE AND SEEK") == 0);
}

int main() {
    TestFormat(); TestRoles(); TestHeldAndTags(); TestGame(); TestMatch(); TestWire(); TestScreens(); TestLobby();
    if (g_fail) { std::printf("%d check(s) failed\n", g_fail); return 1; }
    std::printf("hide and seek tests passed\n");
    return 0;
}
