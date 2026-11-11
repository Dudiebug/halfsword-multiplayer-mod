// Chaos's stats (#2151; match/hsm_chaos_stats.h): the sheet every machine works out for a fighter and a round, when
// it is in force, and the line the player reads.
#include "match/hsm_chaos_stats.h"
#include <cstdio>
#include <optional>
#include <set>
#include <string>

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++g_fail; } } while (0)
using namespace hsm;

static bool Stat(double v) { return v == 0.75 || v == 0.9 || v == 1.0 || v == 1.1 || v == 1.25; }

// The sheet: the same on every machine, drawn anew per fighter, round and build, each stat on its own, inside the rules.
static void TestSheet() {
    const ClassSheet plain;
    std::set<double> run, strength, grip, kick, knockback, jump;
    int rounds = 0, entities = 0, roles = 0, apart = 0, total = 0;
    for (uint8_t e = 1; e <= 20; ++e) for (uint32_t r = 1; r <= 30; ++r) {
        const uint8_t role = uint8_t(kRoleChaosFirst + (e * 7 + r * 3) % 63);
        const ClassSheet c = ChaosClass(role, e, r);
        CHECK(c == ChaosClass(role, e, r));                                     // the same again
        CHECK(c.run == c.walk && Stat(c.run) && Stat(c.strength) && Stat(c.grip) && Stat(c.kick) && Stat(c.knockback));
        CHECK(c.jump == kRuleJumpBase * 0.8 || c.jump == kRuleJumpBase || c.jump == kRuleJumpBase * 1.2);
        CHECK(c.jump >= kRuleJumpMin && c.jump <= kRuleJumpMax && c.run >= kRuleStatMin && c.run <= kRuleStatMax);
        ClassSheet rest = c;                                                    // nothing else moves: no perks, no parts, no mass
        rest.run = rest.walk = rest.strength = rest.grip = rest.kick = rest.knockback = 1; rest.jump = kRuleJumpBase;
        CHECK(rest == plain);
        run.insert(c.run); strength.insert(c.strength); grip.insert(c.grip); kick.insert(c.kick); knockback.insert(c.knockback); jump.insert(c.jump);
        if (!(c == ChaosClass(role, e, r + 1))) ++rounds;
        if (!(c == ChaosClass(role, uint8_t(e + 1), r))) ++entities;
        if (!(c == ChaosClass(role == kRoleChaosLast ? kRoleChaosFirst : uint8_t(role + 1), e, r))) ++roles;
        if (c.run != c.strength || c.strength != c.grip || c.grip != c.kick || c.kick != c.knockback) ++apart;
        ++total;
    }
    CHECK(total == 600 && run.size() == 5 && strength.size() == 5 && grip.size() == 5 && kick.size() == 5 && knockback.size() == 5 && jump.size() == 3);
    CHECK(rounds > 550 && entities > 550 && roles > 550 && apart > 550);
    CHECK(kChaosStat[0] == 0.75 && kChaosStat[4] == 1.25 && kChaosJump[0] == 0.8 && kChaosJump[2] == 1.2);
}

// When it is in force: Chaos only, from the countdown to the results, for a fighter with a Chaos build.
static void TestFor() {
    const uint8_t role = kRoleChaosFirst;
    CHECK(ChaosClassFor(Format::Chaos, MatchPhase::Live, role, 3, 2) == std::optional<ClassSheet>(ChaosClass(role, 3, 2)));
    CHECK(ChaosClassFor(Format::Chaos, MatchPhase::Countdown, role, 3, 2) && ChaosClassFor(Format::Chaos, MatchPhase::Results, role, 3, 2));
    CHECK(ChaosClassFor(Format::Chaos, MatchPhase::SuddenDeath, kRoleChaosLast, 1, 0));
    CHECK(!ChaosClassFor(Format::Chaos, MatchPhase::Idle, role, 3, 2));
    CHECK(!ChaosClassFor(Format::LastManStanding, MatchPhase::Live, role, 3, 2) && !ChaosClassFor(Format::HideSeek, MatchPhase::Live, role, 3, 2));
    CHECK(!ChaosClassFor(Format::Chaos, MatchPhase::Live, kRoleFighter, 3, 2) && !ChaosClassFor(Format::Chaos, MatchPhase::Live, kRoleKnight, 3, 2));
    CHECK(!ChaosClassFor(Format::Chaos, MatchPhase::Live, kRoleSeeker, 3, 2));
    CHECK(!ChaosClassFor(Format::Chaos, MatchPhase::Live, role, 0, 2) && ChaosClassFor(Format::Chaos, MatchPhase::Live, role, 1, 2));
}

// The line the player reads.
static void TestLine() {
    ClassSheet c;
    CHECK(ChaosStatsLine(c) == "the game's own stats");
    c.run = c.walk = 1.25; c.strength = 0.9; c.grip = 1.1; c.kick = 0.75; c.knockback = 1.0; c.jump = kRuleJumpBase * 1.2;
    CHECK(ChaosStatsLine(c) == "run +25%, strength -10%, grip +10%, kick -25%, jump +20%");
    ClassSheet k; k.knockback = 1.25; k.jump = kRuleJumpBase * 0.8;
    CHECK(ChaosStatsLine(k) == "knockback +25%, jump -20%");
    ClassSheet w; w.walk = 0.75;                                                // the walk rides with the run
    CHECK(ChaosStatsLine(w) == "the game's own stats");
    ClassSheet r; r.run = 1.25;
    CHECK(ChaosStatsLine(r) == "run +25%");
}

int main() {
    TestSheet(); TestFor(); TestLine();
    if (g_fail) { std::printf("%d check(s) failed\n", g_fail); return 1; }
    std::printf("chaos stats tests passed\n");
    return 0;
}
