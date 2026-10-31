// Unit tests for hsm_handloss.h. Run via implementation/shared/run_tests.sh test_hsm_handloss.
#include "combat/hsm_handloss.h"
#include <cstdio>

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++g_fail; } } while (0)

using namespace hsm::combat;

namespace {
// The report's case: the host's flail went free with the grab still held.
HandLossInput Flail() {
    HandLossInput in;
    in.ownLive = in.wasHeld = in.gone = in.grabPressed = true;
    return in;
}
}  // namespace

int main() {
    CHECK(LeftHandUnasked(Flail()));

    { HandLossInput in = Flail(); in.grabPressed = false; CHECK(!LeftHandUnasked(in)); }   // the player let go
    { HandLossInput in = Flail(); in.dropping = true; CHECK(!LeftHandUnasked(in)); }       // the game's release path
    { HandLossInput in = Flail(); in.sheathed = true; CHECK(!LeftHandUnasked(in)); }       // put on the belt
    { HandLossInput in = Flail(); in.destroyed = true; CHECK(!LeftHandUnasked(in)); }      // the actor is gone
    { HandLossInput in = Flail(); in.gone = false; CHECK(!LeftHandUnasked(in)); }          // still in the hand (or swapped)
    { HandLossInput in = Flail(); in.wasHeld = false; CHECK(!LeftHandUnasked(in)); }       // fists or an empty hand
    { HandLossInput in = Flail(); in.ownLive = false; CHECK(!LeftHandUnasked(in)); }       // a stand-in: not this log
    CHECK(!LeftHandUnasked(HandLossInput{}));

    static_assert(LeftHandUnasked(HandLossInput{true, true, true, true, false, false, false}));
    static_assert(!LeftHandUnasked(HandLossInput{true, true, true, false, false, false, false}));

    if (g_fail) { std::printf("%d failure(s)\n", g_fail); return 1; }
    std::printf("test_hsm_handloss: ok\n");
    return 0;
}
