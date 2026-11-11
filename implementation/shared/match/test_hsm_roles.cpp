// Roles (match/hsm_roles.h): who gets which role in each format. Default formats change nothing; Riot deals one Knight against
// Rioters at tiers 1..4; Juggernaut deals K Juggernauts by player count against Skirmishers. The special roles rotate at every
// rematch, are deterministic by seed, and a late joiner never gets one. Also the wire byte, the role tags, and that a generated
// loadout made for one role is never served to another.
#include "match/hsm_roles.h"
#include "match/hsm_hud_scene.h"
#include "match/hsm_lobby_scene.h"
#include "protocol/hsm_protocol.h"
#include "session/hsm_autoflow.h"
#include <cmath>
#include <cstdio>
#include <set>

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++g_fail; } } while (0)
using namespace hsm;

static std::vector<uint8_t> Ids(int n) { std::vector<uint8_t> v; for (int i = 1; i <= n; ++i) v.push_back(uint8_t(i)); return v; }
static MatchConfig Cfg(Format f) { MatchConfig c; c.format = f; c.tier = 5; c.gear = Gear::Mirror; c.weapons = WeaponBit(7); return c; }
static int Count(const std::map<uint8_t, FighterRole>& r, uint8_t id) { int n = 0; for (const auto& [e, x] : r) n += x.id == id; return n; }
static bool IsRioter(uint8_t id) { return id >= kRoleRioterFirst && id <= kRoleRioterLast; }
static uint8_t Special(const std::map<uint8_t, FighterRole>& r, uint8_t id) { for (const auto& [e, x] : r) if (x.id == id) return e; return 0; }
static std::set<uint8_t> Specials(const std::map<uint8_t, FighterRole>& r, uint8_t id) { std::set<uint8_t> s; for (const auto& [e, x] : r) if (x.id == id) s.insert(e); return s; }

// Criterion: every format without roles gives everyone the lobby's role - no behaviour change for the existing modes.
static void TestDefaultFormats() {
    for (uint8_t i = 0; i < kFormatCount; ++i) {
        const Format f = Format(i);
        if (HasRoles(f)) continue;
        const MatchConfig c = Cfg(f);
        const auto r = AssignRoles(f, c, Ids(6), 1234, 7);
        CHECK(r.size() == 6);
        for (const auto& [e, x] : r) {
            CHECK(x.id == kRoleFighter && x.tier == 5 && x.gear == Gear::Mirror && x.weapons == WeaponBit(7));
            CHECK(x.weight == kRoleUnchanged && x.height == kRoleUnchanged && x.team == kNoTeam);
        }
    }
    // The lobby's own role is what RoleSpec(Fighter) reads from the config, and an unknown id is that role too.
    const MatchConfig c = Cfg(Format::TeamDuel);
    CHECK(RoleSpec(kRoleFighter, c) == RoleSpec(200, c) && RoleSpec(kRoleCount, c).id == kRoleFighter);
    CHECK(RoleSpec(kRoleFighter, c).tier == 5 && RoleSpec(kRoleFighter, c).weapons == WeaponBit(7));
}

// Criterion: Riot deals exactly one Knight (tier 7, Mirror gear, team 0), everyone else a Rioter at tiers 1..4 on team 1.
static void TestRiotRoles() {
    const MatchConfig c = Cfg(Format::Riot);
    std::set<int> tiers;
    for (int n = 1; n <= 9; ++n)
        for (uint32_t seed = 0; seed < 60; ++seed) {
            const auto r = AssignRoles(Format::Riot, c, Ids(n), seed, seed % 5);
            CHECK(int(r.size()) == n);                                 // every fighter gets a role
            CHECK(Count(r, kRoleKnight) == 1);                         // exactly one Knight, even alone
            for (const auto& [e, x] : r) {
                if (x.id == kRoleKnight) {
                    CHECK(x.tier == 7 && x.gear == Gear::Mirror && x.team == kTeamSpecial && x.weight == TierWeight(7));
                } else {
                    CHECK(IsRioter(x.id) && x.tier >= 1 && x.tier <= 4 && x.team == kTeamRest && x.gear == Gear::RandomTier);
                    CHECK(x.tier == x.id - kRoleRioterFirst + 1 && x.weight == TierWeight(x.tier));
                    tiers.insert(x.tier);
                }
            }
        }
    CHECK(tiers == (std::set<int>{1, 2, 3, 4}));                       // spread over the whole 1..4 range, never outside it
    CHECK(RioterRoleId(0) == RioterRoleId(1) && RioterRoleId(9) == RioterRoleId(4) && RioterRoleId(1) == kRoleRioterFirst && RioterRoleId(4) == kRoleRioterLast);
}

// Criterion: the same arguments deal the same roles; another seed deals another Knight; the input order and repeats do not matter.
static void TestDeterminism() {
    const MatchConfig c = Cfg(Format::Riot);
    CHECK(AssignRoles(Format::Riot, c, Ids(7), 99, 3) == AssignRoles(Format::Riot, c, Ids(7), 99, 3));
    std::set<uint8_t> knights;
    for (uint32_t seed = 0; seed < 40; ++seed) knights.insert(Special(AssignRoles(Format::Riot, c, Ids(7), seed), kRoleKnight));
    CHECK(knights.size() > 3);                                         // the seed moves the Knight around
    const auto sorted = AssignRoles(Format::Riot, c, {1, 3, 5, 8}, 17, 2);
    CHECK(AssignRoles(Format::Riot, c, {8, 5, 3, 1}, 17, 2) == sorted);
    CHECK(AssignRoles(Format::Riot, c, {5, 3, 3, 8, 1, 5}, 17, 2) == sorted && sorted.size() == 4);
    CHECK(AssignRoles(Format::Riot, c, {}, 17).empty() && AssignRoles(Format::Juggernaut, c, {}, 17).empty());
    // A Rioter's tier does not move when somebody else comes or goes (it is spread by the fighter's own id).
    const auto five = AssignRoles(Format::Riot, c, Ids(5), 8, 1), six = AssignRoles(Format::Riot, c, Ids(6), 8, 1);
    for (uint8_t e : {uint8_t(1), uint8_t(2), uint8_t(3), uint8_t(4), uint8_t(5)})
        if (five.at(e).id != kRoleKnight && six.at(e).id != kRoleKnight) CHECK(five.at(e).tier == six.at(e).tier);
}

// Criterion: the Knight (and the Juggernauts) rotate each rematch, so everybody gets a turn.
static void TestRotation() {
    const MatchConfig c = Cfg(Format::Riot);
    for (int n = 2; n <= 9; ++n)
        for (uint32_t seed : {0u, 5u, 123456u}) {
            std::set<uint8_t> turns; uint8_t prev = 0;
            for (uint32_t rematch = 0; rematch < uint32_t(n); ++rematch) {
                const uint8_t k = Special(AssignRoles(Format::Riot, c, Ids(n), seed, rematch), kRoleKnight);
                CHECK(k != 0 && k != prev);                            // never the same fighter twice running
                turns.insert(k); prev = k;
            }
            CHECK(int(turns.size()) == n);                             // after n rematches everybody has been the Knight
        }
    const MatchConfig j = Cfg(Format::Juggernaut);
    for (int n : {3, 4, 5, 6, 7, 8, 9}) {
        const int k = JuggernautCount(n);
        std::map<uint8_t, int> turns;
        const int rematches = (n + k - 1) / k;                         // enough steps of k to go all the way round
        for (int rematch = 0; rematch < rematches; ++rematch)
            for (uint8_t e : Specials(AssignRoles(Format::Juggernaut, j, Ids(n), 42, uint32_t(rematch)), kRoleJuggernaut)) ++turns[e];
        CHECK(int(turns.size()) == n);                                 // everybody was a Juggernaut at least once
    }
    // The next match's deal differs from this one's: the Juggernaut is somebody else.
    CHECK(Specials(AssignRoles(Format::Juggernaut, j, Ids(5), 42, 0), kRoleJuggernaut) != Specials(AssignRoles(Format::Juggernaut, j, Ids(5), 42, 1), kRoleJuggernaut));
}

// Criterion: Juggernaut deals K Juggernauts by player count, tall and heavy, against short, light Skirmishers with a bigger budget.
static void TestJuggernautRoles() {
    const int want[] = {0, 0, 1, 1, 1, 1, 2, 2, 2, 3};                 // 0..9 players
    for (int n = 0; n <= 9; ++n) CHECK(JuggernautCount(n) == want[n]);
    CHECK(JuggernautCount(-3) == 0 && JuggernautCount(20) == 3);
    for (int n = 2; n <= 9; ++n) CHECK(JuggernautCount(n) < n);        // a side is never empty
    const MatchConfig c = Cfg(Format::Juggernaut);
    for (int n = 1; n <= 9; ++n)
        for (uint32_t seed : {0u, 1u, 77u}) {
            const auto r = AssignRoles(Format::Juggernaut, c, Ids(n), seed, seed);
            CHECK(int(r.size()) == n && Count(r, kRoleJuggernaut) == JuggernautCount(n) && Count(r, kRoleSkirmisher) == n - JuggernautCount(n));
            for (const auto& [e, x] : r) {
                if (x.id == kRoleJuggernaut) {
                    CHECK(x.team == kTeamSpecial && x.weight == kJuggernautWeight && x.height == kJuggernautHeight && x.tier == kJuggernautTier);
                } else {
                    CHECK(x.id == kRoleSkirmisher && x.team == kTeamRest && x.weight == kSkirmisherWeight && x.height == kSkirmisherHeight);
                    CHECK(x.tier == kSkirmisherTier && x.gear == Gear::RandomTier && x.weapons == 0);
                }
                CHECK(x.weight >= 0.0 && x.weight <= 1.0 && x.height >= 0.0 && x.height <= 1.0);   // inside the passport's range (BuildWeight clamps to it)
            }
        }
    // Tall and heavy against short and light; the Juggernaut has the top tier's gear.
    CHECK(kJuggernautWeight > kSkirmisherWeight && kJuggernautHeight > kSkirmisherHeight);
    CHECK(kJuggernautTier == 7 && kSkirmisherTier == 4);                 // the boss in the best gear, the rest in tier 4
    CHECK(kTierBudget[kJuggernautTier] > kTierBudget[kSkirmisherTier]);
    CHECK(BuildWeight(0.2, kJuggernautWeight, true) == kJuggernautWeight);   // what the spawn applies is exactly the role's Weight
    // The Juggernaut's size: a scale written as the passport Height the game maps to it (0.875 + 0.125 * Height).
    for (double s : {kJuggernautScaleMin, kJuggernautScaleDefault, kJuggernautScaleMax})
        CHECK(std::fabs(ScaleForHeight(HeightForScale(s)) - s) < 1e-12);
    CHECK(std::fabs(HeightForScale(1.0) - 1.0) < 1e-12);                     // scale 1 is the stock top Height
    CHECK(std::fabs(HeightForScale(kJuggernautScaleDefault) - 3.8) < 1e-12);  // the default: a clearly bigger body (pre24 said 1.075)
    CHECK(std::fabs(HeightForScale(kJuggernautScaleMax) - kMaxSpawnHeight) < 1e-12);
    CHECK(ScaleForHeight(0.0) == 0.875);   // what the spawn applies is exactly the role's Weight
}

// Criterion: a late joiner never gets a special role.
static void TestLateJoiner() {
    for (uint8_t e = 1; e <= 9; ++e)
        for (uint32_t seed : {0u, 3u, 999u}) {
            const FighterRole r = LateJoinerRole(Format::Riot, Cfg(Format::Riot), e, seed, seed);
            CHECK(IsRioter(r.id) && r.team == kTeamRest && r.tier >= 1 && r.tier <= 4);
            const FighterRole s = LateJoinerRole(Format::Juggernaut, Cfg(Format::Juggernaut), e, seed, seed);
            CHECK(s.id == kRoleSkirmisher && s.team == kTeamRest);
            const FighterRole d = LateJoinerRole(Format::TeamDuel, Cfg(Format::TeamDuel), e, seed);
            CHECK(d.id == kRoleFighter && d.team == kNoTeam && d.weight == kRoleUnchanged);
        }
    // A joiner is dealt the same Rioter that a fresh deal would give that id.
    const MatchConfig c = Cfg(Format::Riot);
    for (const auto& [e, x] : AssignRoles(Format::Riot, c, Ids(6), 31, 4))
        if (x.id != kRoleKnight) CHECK(LateJoinerRole(Format::Riot, c, e, 31, 4) == x);
}

// Criterion: the wire carries the role id; the id alone rebuilds the role; a bad id is refused.
static void TestWire() {
    MatchInfo m; m.format = uint8_t(Format::Riot); m.teams = 2;
    const std::vector<uint8_t> ids = {0, 1, 2, 3, 4, 5, 6, 7, kRoleChaosFirst, uint8_t(kRoleChaosFirst + 40), kRoleChaosLast};   // every old role, three of Chaos's
    for (size_t i = 0; i < ids.size(); ++i) { FighterEntry f; f.entity = uint8_t(i + 1); f.name = "p"; f.role = ids[i]; m.fighters.push_back(f); }
    auto bytes = Encode(m);
    MatchInfo o;
    CHECK(Decode(bytes.data(), bytes.size(), o) && o.fighters.size() == ids.size());
    for (size_t i = 0; i < ids.size(); ++i) CHECK(o.fighters[i].role == ids[i]);
    for (size_t cut = 0; cut < bytes.size(); ++cut) CHECK(!Decode(bytes.data(), cut, o));
    MatchInfo bad = m; bad.fighters[3].role = kRoleCount;               // the first value past the last role
    bytes = Encode(bad); CHECK(!Decode(bytes.data(), bytes.size(), o));
    bad.fighters[3].role = 0xFF; bytes = Encode(bad); CHECK(!Decode(bytes.data(), bytes.size(), o));
    FighterEntry plain; CHECK(plain.role == kRoleFighter);              // a fighter nobody dealt a role is the lobby's
    // Every id rebuilds the same role on a client that the host dealt.
    const MatchConfig c = Cfg(Format::Riot);
    for (const auto& [e, x] : AssignRoles(Format::Riot, c, Ids(8), 5, 2)) {
        FighterRole rebuilt = RoleSpec(x.id, c); rebuilt.team = x.team;
        CHECK(rebuilt == x);
    }
    for (const auto& [e, x] : AssignRoles(Format::Juggernaut, c, Ids(8), 5, 2)) {
        FighterRole rebuilt = RoleSpec(x.id, c); rebuilt.team = x.team;
        CHECK(rebuilt == x);
    }
}

// Chaos (#1422): every fighter its own role each deal, no teams; the id carries the tier, the height and the weight; the host's
// weapon type and the round's damage and weights come from the same seed and deal.
static void TestChaos() {
    const MatchConfig c = Cfg(Format::Chaos);
    CHECK(HasRoles(Format::Chaos) && !HasRoleTeams(Format::Chaos) && IsRoundFormat(Format::Chaos) && !HasTeams(Format::Chaos) && TeamCount(Format::Chaos, 3) == 0);
    CHECK(HasRoleTeams(Format::Riot) && HasRoleTeams(Format::Juggernaut) && TeamCount(Format::Riot, 4) == 2 && !IsTeamRound(Format::Chaos));
    CHECK(ScoresPerFighter(Format::Chaos) && Sanitize(c).gear == Gear::RandomTier && Sanitize(c).format == Format::Chaos && !PicksTeams(Format::Chaos));
    CHECK(kRoleChaosFirst == 8 && kRoleChaosLast == 70 && kRoleCount == 73);   // 71 and 72: Hide and Seek's
    // the id is the build, both ways, for every id
    std::set<uint8_t> seen;
    for (uint8_t t = 1; t <= kChaosTiers; ++t) for (uint8_t h = 0; h < kChaosHeights; ++h) for (uint8_t w = 0; w < kChaosWeights; ++w) {
        const uint8_t id = ChaosRoleId({t, h, w});
        CHECK(IsChaosRole(id) && ValidRoleId(id) && (ChaosBuildOf(id) == ChaosBuild{t, h, w}) && seen.insert(id).second);
        const FighterRole r = RoleSpec(id, c);
        CHECK(r.id == id && r.tier == t && r.gear == Gear::RandomTier && r.weapons == 0 && r.weight == kChaosWeight[w] && r.height == kChaosHeight[h] && r.team == kNoTeam);
    }
    CHECK(seen.size() == 63 && *seen.begin() == kRoleChaosFirst && *seen.rbegin() == kRoleChaosLast);
    CHECK(!IsChaosRole(kRoleSkirmisher) && !IsChaosRole(kRoleCount) && ChaosBuildOf(kRoleKnight) == ChaosBuild{} && ChaosBuildOf(kRoleCount) == ChaosBuild{});
    CHECK(ChaosRoleId({0, 9, 9}) == ChaosRoleId({1, 2, 2}) && ChaosRoleId({9, 0, 0}) == ChaosRoleId({7, 0, 0}));   // out of range: clamped
    CHECK(kChaosHeight[0] == 0.0 && kChaosHeight[1] == 1.0 && ScaleForHeight(kChaosHeight[2]) == 1.25 && kChaosHeight[2] <= kMaxSpawnHeight);
    CHECK(kChaosWeight[0] == 0.15 && kChaosWeight[1] == 0.5 && kChaosWeight[2] == 1.0);
    // a deal: everybody a Chaos role, nobody on a team; the same arguments deal the same; a late joiner gets the deal's own draw
    const auto a = AssignRoles(Format::Chaos, c, Ids(8), 77, 0);
    CHECK(a.size() == 8 && a == AssignRoles(Format::Chaos, c, Ids(8), 77, 0));
    for (const auto& [e, x] : a) {
        CHECK(IsChaosRole(x.id) && x.team == kNoTeam && x.id == ChaosRoleFor(77, 0, e) && LateJoinerRole(Format::Chaos, c, e, 77, 0) == x);
        CHECK(RoleSpec(x.id, c) == x);                                       // what a client builds from the id
    }
    for (const auto& [e, x] : AssignRoles(Format::Chaos, c, Ids(8), 77, 5))   // a later deal: its own draws, for a late joiner too
        CHECK(x.id == ChaosRoleFor(77, 5, e) && LateJoinerRole(Format::Chaos, c, e, 77, 5) == x);
    CHECK(AssignRoles(Format::Chaos, c, Ids(5), 77, 0).at(3) == a.at(3));    // others coming and going do not move a fighter's draw
    // over many deals: every tier, height, weight and weapon type comes up; the next deal and another seed differ
    std::set<uint8_t> tiers, heights, weights, types; int same = 0, sameSeed = 0, sameFighter = 0;
    for (uint32_t deal = 0; deal < 200; ++deal) for (uint8_t e = 1; e <= 8; ++e) {
        const ChaosBuild b = ChaosBuildOf(ChaosRoleFor(77, deal, e));
        tiers.insert(b.tier); heights.insert(b.height); weights.insert(b.weight);
        same += ChaosRoleFor(77, deal, e) == ChaosRoleFor(77, deal + 1, e);
        sameSeed += ChaosRoleFor(77, deal, e) == ChaosRoleFor(78, deal, e);
        sameFighter += ChaosRoleFor(77, deal, e) == ChaosRoleFor(77, deal, uint8_t(e + 1));
        const WeaponMask m = ChaosWeapons(77, deal, e);
        CHECK(WeaponTypesTicked(m) == 1 && CleanWeaponMask(m) == m && m == ChaosWeapons(77, deal, e));
        types.insert(GameWeaponTypeByte(m));
    }
    CHECK(tiers == std::set<uint8_t>({1, 2, 3, 4, 5, 6, 7}) && heights.size() == 3 && weights.size() == 3 && types.size() == kWeaponTypeCount);
    CHECK(same < 100 && sameSeed < 100 && sameFighter < 100);              // 1600 draws each: about 1 in 63 repeats
    // the round's values: the same for a deal, in the rows' ranges, and every choice comes up
    std::set<uint16_t> dmg; std::set<uint8_t> dead, weapon; int repeats = 0, seedSame = 0, differ = 0;
    for (uint32_t deal = 0; deal < 400; ++deal) {
        const ChaosRound r = ChaosRoundOf(77, deal);
        CHECK(r == ChaosRoundOf(77, deal) && r.deadWeightPct <= kWeightMaxPct && r.weaponWeightPct <= kWeightMaxPct && r.deadWeightPct % 10 == 0 && r.weaponWeightPct % 10 == 0);
        dmg.insert(r.damagePct); dead.insert(r.deadWeightPct); weapon.insert(r.weaponWeightPct);
        repeats += r == ChaosRoundOf(77, deal + 1);
        differ += r.deadWeightPct != r.weaponWeightPct;
        seedSame += r == ChaosRoundOf(78, deal);
    }
    CHECK(dmg == std::set<uint16_t>({50, 75, 100, 150, 200}) && dead.size() == 11 && weapon.size() == 11 && *dead.begin() == 0 && *dead.rbegin() == 100);
    CHECK(repeats < 20 && seedSame < 20 && differ > 300);                  // the two weights are separate draws
    // the match itself: rounds to the last one standing, everybody back with a fresh kit each round, nothing made ahead
    MatchConfig mc = Sanitize(c); mc.scoreLimit = 2;
    Match m; m.Start(mc, {{1, kNoTeam, true}, {2, kNoTeam, true}, {3, kNoTeam, true}}, 0);
    const uint32_t t = Match::kCountdownMs;
    m.Tick(t);
    m.OnDeath(1, 2, t + 1); m.OnDeath(2, 3, t + 2);
    CHECK(m.roundWinner() == 3 && m.Score(3) == 1 && !m.RoundInPlay());
    const auto ev = m.Tick(t + 2 + 5000);
    CHECK(ev.roundReset && ev.regear == Regear::Each && ev.respawn == std::vector<uint8_t>({1, 2, 3}) && m.round() == 2);
    m.OnDeath(1, 3, t + 8000); m.OnDeath(2, 3, t + 8001);
    CHECK(m.phase() == MatchPhase::Results && m.winner() == 3 && m.LoadoutsAhead() == 0);
    CHECK(DefaultScoreLimit(Format::Chaos) == 3 && DropClearFor(Format::Chaos) == DropClear::EachRound && RegearFor(mc, RespawnCause::RoundReset) == Regear::Each);
    CHECK(std::string(RoleName(kRoleChaosFirst)) == "chaos" && std::string(RoleTag(kRoleChaosLast)).empty() && RoleLine(kRoleChaosFirst).empty() && RoleMayHoldCrossbow(kRoleChaosFirst));
}

// Chaos on the screen: the FIGHT banner says what the fighter was dealt, the line under the pill what the round runs at; the
// lobby has the mode's own rows only.
static void TestChaosScreens() {
    CHECK(ChaosRoleLine(ChaosRoleId({7, 2, 2})) == "TALL, HEAVY, KNIGHT GEAR" && ChaosRoleLine(ChaosRoleId({1, 0, 0})) == "SHORT, LIGHT, PEASANT GEAR");
    CHECK(ChaosRoleLine(ChaosRoleId({4, 1, 1})) == "SOLDIER GEAR" && ChaosRoleLine(ChaosRoleId({4, 1, 2})) == "HEAVY, SOLDIER GEAR" && ChaosRoleLine(ChaosRoleId({4, 0, 1})) == "SHORT, SOLDIER GEAR");
    CHECK(ChaosRoleLine(kRoleKnight).empty() && ChaosRoleLine(kRoleFighter).empty());
    MatchInfo m; m.format = uint8_t(Format::Chaos); m.phase = uint8_t(MatchPhase::Live); m.round = 2; m.scoreLimit = 3;
    m.damagePct = 150; m.physics[kPinnedDeadWeight] = 0.2f; m.physics[kPinnedWeaponWeight] = 0.8f;
    FighterEntry me; me.entity = 1; me.name = "Ann"; me.alive = true; me.role = ChaosRoleId({5, 2, 0});
    FighterEntry other; other.entity = 2; other.name = "Bo"; other.alive = true; other.role = ChaosRoleId({2, 1, 1});
    m.fighters = {me, other};
    CHECK(ChaosLine(m) == "Damage 150%   Dead weight 20%   Weapon weight 80%");
    CHECK(FightSub(m, 1) == "ROUND 2 \xC2\xB7 TALL, LIGHT, VETERAN GEAR" && FightSub(m, 2) == "ROUND 2 \xC2\xB7 COMMONER GEAR");
    LocalHud l; l.me = 1; l.infoAtMs = 1000;
    HudScene s = BuildHudScene(m, l, 1000);
    CHECK(s.info.visible && s.info.text == ChaosLine(m));
    MatchInfo n = m; n.damagePct = kDamageStock; n.physics[kPinnedDeadWeight] = -1; n.physics[kPinnedWeaponWeight] = 1.0f;
    CHECK(ChaosLine(n) == "Damage stock   Weapon weight 100%");
    n = m; n.physics[kPinnedDeadWeight] = 0; CHECK(ChaosLine(n) == "Damage 150%   Dead weight 0%   Weapon weight 80%");
    n = m; n.phase = uint8_t(MatchPhase::Idle); CHECK(ChaosLine(n).empty());
    n = m; n.format = uint8_t(Format::LastManStanding); CHECK(ChaosLine(n).empty() && !BuildHudScene(n, l, 1000).info.visible);
    // the lobby: the match's own rows; two players are enough (Riot and Juggernaut still need three)
    MatchConfig c; c.format = Format::Chaos;
    const std::vector<LobbyRow> rows = SettingsFor(Sanitize(c));
    CHECK(rows == std::vector<LobbyRow>({LobbyRow::Limit, LobbyRow::Time, LobbyRow::Respawn, LobbyRow::FreeCam, LobbyRow::Crossbows, LobbyRow::Sync}));
    CHECK(!ModeAvailable(Format::Chaos, 1).ok && ModeAvailable(Format::Chaos, 2).ok && !ModeAvailable(Format::Riot, 2).ok && ModeAvailable(Format::Riot, 3).ok && !ModeAvailable(Format::Juggernaut, 2).ok);
    CHECK(FormatName(Format::Chaos) == "Chaos" && std::string(FormatBlurb(Format::Chaos)).find("every round") != std::string::npos);
    Format f = Format::FreeForAll;
    CHECK(ParseServerFormat("chaos", &f) && f == Format::Chaos);
    for (const MapInfo& mi : kMaps) CHECK(MapAllowsFormat(mi, Format::Chaos) == MapAllowsFormat(mi, Format::LastManStanding));   // the same maps
    // the round pill reads like Last man standing's
    CHECK(StatusLine(m, 1).find("Rounds") == 0);
}

// Criterion: the scoreboard tags only the special roles; the spawn line names the role.
static void TestNames() {
    CHECK(std::string(RoleTag(kRoleKnight)) == "KNIGHT" && std::string(RoleTag(kRoleJuggernaut)) == "JUGGERNAUT");
    for (uint8_t id : {kRoleFighter, kRoleSkirmisher, uint8_t(3), kRoleRioterLast, uint8_t(99)}) CHECK(std::string(RoleTag(id)).empty());
    CHECK(RoleLine(kRoleKnight) == "YOU ARE THE KNIGHT" && RoleLine(kRoleJuggernaut) == "YOU ARE A JUGGERNAUT");
    CHECK(RoleLine(kRoleSkirmisher) == "YOU ARE A SKIRMISHER" && RoleLine(kRoleRioterFirst) == "YOU ARE A RIOTER" && RoleLine(kRoleRioterLast) == "YOU ARE A RIOTER");
    CHECK(RoleLine(kRoleFighter).empty() && RoleLine(kRoleCount).empty());
    CHECK(std::string(RoleName(kRoleKnight)) == "knight" && std::string(RoleName(4)) == "rioter" && std::string(RoleName(kRoleFighter)) == "fighter");
    CHECK(ValidRoleId(0) && ValidRoleId(kRoleCount - 1) && !ValidRoleId(kRoleCount));
}

// Criterion: the format rules - Riot and Juggernaut are team rounds with fixed teams, rolled gear and no pool made ahead.
static void TestFormatRules() {
    for (Format f : {Format::Riot, Format::Juggernaut}) {
        CHECK(HasRoles(f) && IsTeamRound(f) && IsRoundFormat(f) && HasTeams(f) && !PicksTeams(f) && !ScoresPerFighter(f));
        CHECK(TeamCount(f, 4) == 2 && TeamCount(f, 2) == 2);
        MatchConfig c; c.format = f; c.gear = Gear::Own; c.tier = 3;
        const MatchConfig s = Sanitize(c);
        CHECK(s.format == f && s.gear == Gear::RandomTier);            // the roles roll every loadout: RegearFor gives Each
        CHECK(RegearFor(s, RespawnCause::RoundReset) == Regear::Each && RegearFor(s, RespawnCause::Rematch) == Regear::Each);
        CHECK(StartLoadoutsAhead(s, 5) == 0);                          // nothing made ahead at the lobby's tier: the roles roll at their own
        CHECK(DefaultScoreLimit(f) == 3);
    }
    CHECK(HasTeams(Format::TeamDuel) && PicksTeams(Format::TeamDuel) && IsTeamRound(Format::TeamDuel) && !HasRoles(Format::TeamDuel));
    for (int t = 0; t <= 6; ++t) CHECK(TeamCount(Format::TeamDuel, t) == std::clamp(t, 2, 4));
    for (Format f : {Format::FreeForAll, Format::Duel, Format::LastManStanding, Format::PlayersVsBots, Format::Zombies}) CHECK(!HasRoles(f) && !IsTeamRound(f));
    CHECK(ScoresPerFighter(Format::FreeForAll) && ScoresPerFighter(Format::Duel) && ScoresPerFighter(Format::LastManStanding));
    CHECK(!ScoresPerFighter(Format::TwoTeams) && !ScoresPerFighter(Format::TeamDuel));
    MatchConfig d; d.format = Format::Duel; d.gear = Gear::Own;
    CHECK(Sanitize(d).gear == Gear::Own);                              // only the role formats force the gear
    // Duel's rounds still make their loadouts ahead; a role format makes none.
    MatchConfig dr; dr.format = Format::Duel; dr.gear = Gear::RandomTier;
    CHECK(StartLoadoutsAhead(dr, 2) == 2);
    Match riot; riot.Start(Sanitize(MatchConfig{Format::Riot}), {{1, 0, true}, {2, 1, true}, {3, 1, true}}, 0);
    CHECK(riot.LoadoutsAhead() == 0);
    riot.Tick(Match::kCountdownMs); riot.OnDeath(2, 1, Match::kCountdownMs + 10); riot.OnDeath(3, 1, Match::kCountdownMs + 20);
    CHECK(riot.RoundInPlay() == false && riot.LoadoutsAhead() == 0);   // in the round gap too
}

// Criterion: a generated loadout made for one role is never served to another (the pool is keyed by tier and weapons).
static void TestPoolNeverCrossesRoles() {
    GearPool p;
    const std::vector<uint8_t> knight = {7, 7}, skirmisher = {4, 4}, jugg = {3, 3};
    p.Put(kKnightTier, 0, knight);
    CHECK(p.Take(kSkirmisherTier, 0).empty());          // another role's tier: nothing, and the knight's roll is dropped
    CHECK(p.Take(kKnightTier, 0).empty());
    p.Put(kSkirmisherTier, 0, skirmisher);
    CHECK(p.Take(kSkirmisherTier, WeaponBit(7)).empty());     // same tier, another weapon filter: nothing
    p.Put(kSkirmisherTier, 0, skirmisher);
    CHECK(p.Take(kSkirmisherTier, 0) == skirmisher);    // its own role: served
    p.Put(kJuggernautTier, 0, jugg);
    CHECK(p.Wants(kSkirmisherTier, 0, 1) && p.size() == 0);   // asking for another role's tier empties it
}

// Integration (Wave 2): the Crossbows row never arms the Knight or a Juggernaut; the Rioters, Skirmishers and plain Fighters may hold one.
static void TestCrossbowsSkipSpecialRoles() {
    CHECK(!RoleMayHoldCrossbow(kRoleKnight) && !RoleMayHoldCrossbow(kRoleJuggernaut));
    CHECK(RoleMayHoldCrossbow(kRoleFighter) && RoleMayHoldCrossbow(kRoleSkirmisher));
    for (uint8_t id = kRoleRioterFirst; id <= kRoleRioterLast; ++id) CHECK(RoleMayHoldCrossbow(id));
    for (Format f : {Format::Riot, Format::Juggernaut}) {
        const MatchConfig cfg = Sanitize(MatchConfig{f});
        for (size_t n : {3u, 5u, 9u}) {
            std::vector<uint8_t> fighters;
            for (uint8_t e = 1; e <= n; ++e) fighters.push_back(e);
            for (uint32_t rematch = 0; rematch < n; ++rematch) {
                std::map<uint8_t, uint8_t> roleOf;
                size_t special = 0;
                for (const auto& [e, r] : AssignRoles(f, cfg, fighters, 77, rematch)) { roleOf[e] = r.id; special += !RoleMayHoldCrossbow(r.id); }
                CHECK(special >= 1);
                const std::vector<uint8_t> all = PickCrossbowHoldersByRole(4, fighters, roleOf, 5);
                CHECK(all.size() == n - special);                                         // All: every ordinary fighter, no special one
                for (uint8_t e : all) CHECK(RoleMayHoldCrossbow(roleOf[e]));
                for (uint32_t seed = 1; seed < 40; ++seed)
                    for (uint8_t mode = 1; mode <= 4; ++mode)
                        for (uint8_t e : PickCrossbowHoldersByRole(mode, fighters, roleOf, seed)) CHECK(RoleMayHoldCrossbow(roleOf[e]));
                CHECK(PickCrossbowHoldersByRole(3, fighters, roleOf, 5).size() == CrossbowCount(3, n - special));   // Half of the eligible
            }
        }
    }
    // A format without roles: everyone is eligible, as PickCrossbowHolders alone.
    const std::vector<uint8_t> three = {1, 2, 3};
    CHECK(PickCrossbowHoldersByRole(4, three, {}, 9) == three);
    CHECK(PickCrossbowHoldersByRole(0, three, {}, 9).empty());
}

int main() {
    TestCrossbowsSkipSpecialRoles();
    TestDefaultFormats(); TestRiotRoles(); TestDeterminism(); TestRotation(); TestJuggernautRoles(); TestLateJoiner(); TestWire(); TestNames();
    TestFormatRules(); TestPoolNeverCrossesRoles(); TestChaos(); TestChaosScreens();
    if (g_fail) { std::printf("%d check(s) failed\n", g_fail); return 1; }
    std::printf("roles tests passed\n");
    return 0;
}
