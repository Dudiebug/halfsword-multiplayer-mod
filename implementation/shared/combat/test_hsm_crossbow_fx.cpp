// Unit tests for hsm_crossbow_fx.h. Run via implementation/shared/run_tests.sh test_hsm_crossbow_fx.
#include "combat/hsm_crossbow_fx.h"
#include <cstdio>
#include <cstring>

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++g_fail; } } while (0)

using namespace hsm::combat;
using Steps = std::vector<CrossbowStep>;

namespace {

constexpr CrossbowState kEmpty{false, false}, kDrawn{true, false}, kLoaded{true, true}, kBoltOnly{false, true};

// The owner's own load cycle (Willie_BP Timeline_26: Load String; Timeline_27: Load Bolt; input: Shoot), seen frame by
// frame, reports draw, bolt, shot in that order and nothing on the frames in between.
void TestOwnerCycle() {
    CHECK(!CrossbowEdge(kEmpty, kEmpty));
    CHECK(CrossbowEdge(kEmpty, kDrawn) == CrossbowAction::String);
    CHECK(!CrossbowEdge(kDrawn, kDrawn));
    CHECK(CrossbowEdge(kDrawn, kLoaded) == CrossbowAction::Bolt);
    CHECK(!CrossbowEdge(kLoaded, kLoaded));
    CHECK(CrossbowEdge(kLoaded, kEmpty) == CrossbowAction::Shoot);
}

// Event Shoot needs only String Loaded: a drawn string with no bolt still snaps.
void TestDryShot() {
    CHECK(CrossbowEdge(kDrawn, kEmpty) == CrossbowAction::ShootEmpty);
    CHECK(CrossbowEdge(kLoaded, kBoltOnly) == CrossbowAction::Shoot);   // the string is what makes it a shot
}

// A spawn armed crossbow (both flags in one frame) is a bolt report: the replay draws the string first.
void TestSameFrame() {
    CHECK(CrossbowEdge(kEmpty, kLoaded) == CrossbowAction::Bolt);
}

// A bolt that falls off a drawn crossbow has no event to replay.
void TestLostBolt() {
    CHECK(!CrossbowEdge(kLoaded, kDrawn));
    CHECK(!CrossbowEdge(kBoltOnly, kEmpty));
    CHECK(CrossbowBoltLost(kLoaded, kDrawn));
    CHECK(!CrossbowBoltLost(kLoaded, kEmpty));      // the shot
    CHECK(!CrossbowBoltLost(kLoaded, kLoaded) && !CrossbowBoltLost(kDrawn, kDrawn) && !CrossbowBoltLost(kDrawn, kLoaded));
    CHECK(!CrossbowBoltLost(kBoltOnly, kEmpty));    // no drawn string: not the case being looked for
    CHECK(!CrossbowBoltLost(kBoltOnly, kDrawn));    // the string drawn in the same frame the bolt left
    CHECK(!CrossbowBoltLost(kEmpty, kEmpty));
}

void TestReplayPlans() {
    CHECK(PlanCrossbowReplay(kEmpty, CrossbowAction::String) == Steps{CrossbowStep::LoadString});
    CHECK(PlanCrossbowReplay(kDrawn, CrossbowAction::String).empty());
    CHECK(PlanCrossbowReplay(kDrawn, CrossbowAction::Bolt) == Steps{CrossbowStep::LoadBolt});
    CHECK((PlanCrossbowReplay(kEmpty, CrossbowAction::Bolt) == Steps{CrossbowStep::LoadString, CrossbowStep::LoadBolt}));
    CHECK(PlanCrossbowReplay(kLoaded, CrossbowAction::Bolt).empty());              // never a second bolt
    CHECK(PlanCrossbowReplay(kLoaded, CrossbowAction::Shoot) == Steps{CrossbowStep::Shoot});
    CHECK((PlanCrossbowReplay(kDrawn, CrossbowAction::Shoot) == Steps{CrossbowStep::LoadBolt, CrossbowStep::Shoot}));
    CHECK((PlanCrossbowReplay(kEmpty, CrossbowAction::Shoot) ==
           Steps{CrossbowStep::LoadString, CrossbowStep::LoadBolt, CrossbowStep::Shoot}));   // missed every earlier report
    CHECK(PlanCrossbowReplay(kDrawn, CrossbowAction::ShootEmpty) == Steps{CrossbowStep::Shoot});
    CHECK((PlanCrossbowReplay(kEmpty, CrossbowAction::ShootEmpty) == Steps{CrossbowStep::LoadString, CrossbowStep::Shoot}));
    // A dry shot on a stand-in that does hold a bolt still shoots (its bolt flies, as the owner's empty one could not).
    CHECK(PlanCrossbowReplay(kLoaded, CrossbowAction::ShootEmpty) == Steps{CrossbowStep::Shoot});
}

// Every report the owner sends, replayed on a stand-in in step, leaves the stand-in in the owner's state.
void TestReplayFollowsOwner() {
    auto apply = [](CrossbowState s, const Steps& steps) {
        for (CrossbowStep st : steps) {
            if (st == CrossbowStep::LoadString) s.string = true;
            else if (st == CrossbowStep::LoadBolt) { if (s.string) s.bolt = true; }   // Event Load Bolt needs the string
            else if (s.string) s = kEmpty;                                           // Event Shoot needs the string
        }
        return s;
    };
    const CrossbowState owner[] = {kEmpty, kDrawn, kLoaded, kEmpty, kDrawn, kEmpty, kLoaded, kEmpty};
    CrossbowState standIn = kEmpty;
    for (size_t i = 1; i < sizeof owner / sizeof owner[0]; ++i) {
        if (auto a = CrossbowEdge(owner[i - 1], owner[i])) standIn = apply(standIn, PlanCrossbowReplay(standIn, *a));
        CHECK(standIn.string == owner[i].string && standIn.bolt == owner[i].bolt);
    }
}

// #4118: the owner dropped its drawn crossbow, shot it empty and took it back; the shot reached a stand-in holding no
// crossbow. Kept as the state after it, the stand-in's still-drawn crossbow is shot slack once it is held again, where a
// replay of the next draw does nothing.
void TestReconcile() {
    auto apply = [](CrossbowState s, const Steps& steps) {
        for (CrossbowStep st : steps) {
            if (st == CrossbowStep::LoadString) s.string = true;
            else if (st == CrossbowStep::LoadBolt) { if (s.string) s.bolt = true; }
            else if (s.string) s = kEmpty;
        }
        return s;
    };
    const CrossbowState states[] = {kEmpty, kDrawn, kLoaded};
    const CrossbowAction actions[] = {CrossbowAction::String, CrossbowAction::Bolt, CrossbowAction::Shoot, CrossbowAction::ShootEmpty};
    for (CrossbowAction a : actions) {
        const CrossbowTarget t = CrossbowAfter(a);
        for (CrossbowState s : states) {
            const CrossbowState r = apply(s, PlanCrossbowReconcile(s, t));
            CHECK(r.string == t.string);
            if (t.boltKnown) CHECK(r.bolt == t.bolt);
            else CHECK(r.bolt == s.bolt);   // a draw leaves the bolt as it was
        }
    }
    // The report's case: drawn on the stand-in, the owner shot empty.
    CHECK((PlanCrossbowReconcile(kDrawn, CrossbowAfter(CrossbowAction::ShootEmpty)) == Steps{CrossbowStep::Shoot}));
    CHECK((PlanCrossbowReplay(kDrawn, CrossbowAction::String).empty()));   // what the old path did with the next draw
    // Already there: nothing to run.
    CHECK(PlanCrossbowReconcile(kEmpty, CrossbowAfter(CrossbowAction::Shoot)).empty());
    CHECK(PlanCrossbowReconcile(kLoaded, CrossbowAfter(CrossbowAction::Bolt)).empty());
    CHECK(PlanCrossbowReconcile(kLoaded, CrossbowAfter(CrossbowAction::String)).empty());
    CHECK((PlanCrossbowReconcile(kEmpty, CrossbowAfter(CrossbowAction::Bolt)) == Steps{CrossbowStep::LoadString, CrossbowStep::LoadBolt}));
    CHECK(!CrossbowAfter(CrossbowAction(9)).string);
}

void TestNames() {
    CHECK(IsCrossbowHand("hand_r") && IsCrossbowHand("hand_l"));
    CHECK(!IsCrossbowHand("") && !IsCrossbowHand("hand_R") && !IsCrossbowHand("hand_r ") && !IsCrossbowHand("pelvis"));
    CHECK(std::strcmp(CrossbowActionName(CrossbowAction::Shoot), "shoot") == 0);
    CHECK(std::strcmp(CrossbowActionName(CrossbowAction(9)), "?") == 0);
    CHECK(kCrossbowActionMax == 4);
}

}  // namespace

int main() {
    TestOwnerCycle();
    TestDryShot();
    TestSameFrame();
    TestLostBolt();
    TestReplayPlans();
    TestReplayFollowsOwner();
    TestReconcile();
    TestNames();
    if (g_fail) { std::printf("test_hsm_crossbow_fx: %d failure(s)\n", g_fail); return 1; }
    std::printf("test_hsm_crossbow_fx: ok\n");
    return 0;
}
