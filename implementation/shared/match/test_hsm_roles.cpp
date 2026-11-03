// Roles (match/hsm_roles.h): who gets which role in each format. Default formats change nothing; Riot deals one Knight against
// Rioters at tiers 1..4; Juggernaut deals K Juggernauts by player count against Skirmishers. The special roles rotate at every
// rematch, are deterministic by seed, and a late joiner never gets one. Also the wire byte, the role tags, and that a generated
// loadout made for one role is never served to another.
#include "match/hsm_roles.h"
#include "protocol/hsm_protocol.h"
#include <cmath>
#include <cstdio>
#include <set>

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++g_fail; } } while (0)
using namespace hsm;

static std::vector<uint8_t> Ids(int n) { std::vector<uint8_t> v; for (int i = 1; i <= n; ++i) v.push_back(uint8_t(i)); return v; }
static MatchConfig Cfg(Format f) { MatchConfig c; c.format = f; c.tier = 5; c.gear = Gear::Mirror; c.weapons = WeaponFilter::Polearms; return c; }
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
            CHECK(x.id == kRoleFighter && x.tier == 5 && x.gear == Gear::Mirror && x.weapons == WeaponFilter::Polearms);
            CHECK(x.weight == kRoleUnchanged && x.height == kRoleUnchanged && x.team == kNoTeam);
        }
    }
    // The lobby's own role is what RoleSpec(Fighter) reads from the config, and an unknown id is that role too.
    const MatchConfig c = Cfg(Format::TeamDuel);
    CHECK(RoleSpec(kRoleFighter, c) == RoleSpec(200, c) && RoleSpec(kRoleCount, c).id == kRoleFighter);
    CHECK(RoleSpec(kRoleFighter, c).tier == 5 && RoleSpec(kRoleFighter, c).weapons == WeaponFilter::Polearms);
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
                    CHECK(x.tier == kSkirmisherTier && x.gear == Gear::RandomTier && x.weapons == WeaponFilter::Any);
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
    for (uint8_t i = 0; i < kRoleCount; ++i) { FighterEntry f; f.entity = uint8_t(i + 1); f.name = "p"; f.role = i; m.fighters.push_back(f); }
    auto bytes = Encode(m);
    MatchInfo o;
    CHECK(Decode(bytes.data(), bytes.size(), o) && o.fighters.size() == kRoleCount);
    for (uint8_t i = 0; i < kRoleCount; ++i) CHECK(o.fighters[i].role == i);
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
    p.Put(kKnightTier, WeaponFilter::Any, knight);
    CHECK(p.Take(kSkirmisherTier, WeaponFilter::Any).empty());          // another role's tier: nothing, and the knight's roll is dropped
    CHECK(p.Take(kKnightTier, WeaponFilter::Any).empty());
    p.Put(kSkirmisherTier, WeaponFilter::Any, skirmisher);
    CHECK(p.Take(kSkirmisherTier, WeaponFilter::Polearms).empty());     // same tier, another weapon filter: nothing
    p.Put(kSkirmisherTier, WeaponFilter::Any, skirmisher);
    CHECK(p.Take(kSkirmisherTier, WeaponFilter::Any) == skirmisher);    // its own role: served
    p.Put(kJuggernautTier, WeaponFilter::Any, jugg);
    CHECK(p.Wants(kSkirmisherTier, WeaponFilter::Any, 1) && p.size() == 0);   // asking for another role's tier empties it
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
    TestFormatRules(); TestPoolNeverCrossesRoles();
    if (g_fail) { std::printf("%d check(s) failed\n", g_fail); return 1; }
    std::printf("roles tests passed\n");
    return 0;
}
