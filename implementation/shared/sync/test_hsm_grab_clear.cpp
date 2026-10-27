// GrabInbox::Clear keeps what was retired. Run via implementation/shared/run_tests.sh test_hsm_grab_clear.
#include "sync/hsm_grab.h"
#include <cstdio>

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++g_fail; } } while (0)
using namespace hsm;
using namespace hsm::sync;

static GrabStateMsg State(uint32_t seq, uint32_t grabId) {
    GrabStateMsg m;
    m.grabber = 2;
    m.seq = seq;
    m.hands[0].held = true; m.hands[0].target = 5; m.hands[0].grabId = grabId; m.hands[0].bone = "head";
    return m;
}

int main() {
    const auto mine = [](uint8_t e) { return e == 5; };
    const auto alive = [](uint8_t) { return false; };
    GrabInbox in;
    CHECK(in.Accept(State(1, 9), 0));
    CHECK(in.Pulls(0, mine, alive).size() == 1);
    in.Forget(2, 0);                                  // the grabber respawned: grab 9 retired
    in.Clear();                                       // the session paused or restarted its tables
    CHECK(in.Pulls(10, mine, alive).empty());
    CHECK(in.Accept(State(1, 9), 10));                // a refresh of the retired grab still on its way
    CHECK(in.Pulls(10, mine, alive).empty());         // does not bring it back
    CHECK(in.Accept(State(2, 10), 10));
    CHECK(in.Pulls(10, mine, alive).size() == 1);     // a new grab does
    if (g_fail) { std::printf("%d failure(s)\n", g_fail); return 1; }
    std::printf("test_hsm_grab_clear: ok\n");
    return 0;
}
