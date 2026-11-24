// The world context a mod call hands the engine (session/hsm_world_context.h, public #855).
#include "session/hsm_world_context.h"
#include <cstdio>

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++g_fail; } } while (0)
using namespace hsm;
using P = WorldContextPick;

namespace {
// The mod tick's order, as in Session::Tick: the mode that ticks is recorded first; the lobby and host-migration parts
// run next and use the context the session cached; only then does the session resolve the level and cache the new mode.
// `alive` stands for "the recorded slot still holds the object": a level change frees the old mode.
struct Tick {
    const void* recorded = nullptr;   // worldctx: the mode whose ReceiveTick runs
    bool recordedAlive = false;
    const void* cached = nullptr;     // Session::lastGameMode_ / Adapter::world_
    const void* handed = nullptr;     // what the lobby part's OpenLevel / PlaySound2D got as WorldContextObject
    void Run(const void* mode, bool lobbyCalls) {
        recorded = mode; recordedAlive = true;                     // the top of the tick
        if (lobbyCalls) handed = WorldContextFor(PickWorldContext(cached, recorded, recordedAlive), cached, recorded);
        cached = mode;                                             // adapter_.Resolve / lastGameMode_ = gameMode
    }
};
}  // namespace

int main() {
    int a = 0, b = 0;
    const void* modeA = &a;
    const void* modeB = &b;

    // The plain cases.
    CHECK(PickWorldContext(modeA, modeA, true) == P::Wanted);
    CHECK(PickWorldContext(modeA, modeB, true) == P::Live);       // an earlier level's mode: the live one instead
    CHECK(PickWorldContext(modeA, modeA, false) == P::None);      // the recorded mode was freed (a load under way)
    CHECK(PickWorldContext(modeA, modeB, false) == P::None);
    CHECK(PickWorldContext(modeA, nullptr, true) == P::None);     // nothing ticked yet
    CHECK(PickWorldContext(nullptr, modeA, true) == P::None);     // the caller holds no context: none (as before)
    CHECK(WorldContextFor(P::Wanted, modeA, modeB) == modeA);
    CHECK(WorldContextFor(P::Live, modeA, modeB) == modeB);
    CHECK(WorldContextFor(P::None, modeA, modeB) == nullptr);

    // The reports: level A ticks; the player leaves to the menu (level B); A is freed. On B's first tick a host migration
    // that gave up (ReturnToMenuAfterHostLoss) or a member's join (PlaySound2D) used the session's cached mode, still A,
    // because the session resolves B only after its lobby part. Before this rule the call was handed A, a freed object.
    Tick t;
    t.Run(modeA, false);
    t.Run(modeA, true);
    CHECK(t.handed == modeA);                                     // the same level: its own mode
    t.Run(modeB, true);                                           // the new level's first tick
    CHECK(t.handed != modeA);                                     // never the freed mode
    CHECK(t.handed == modeB);                                     // the call runs in the level that is open
    t.Run(modeB, true);
    CHECK(t.handed == modeB);

    // A paused game or a load: the mode does not tick, widgets and agent pumps still call. The mode is alive while paused;
    // once the load freed it, no context at all.
    CHECK(WorldContextFor(PickWorldContext(modeB, t.recorded, true), modeB, t.recorded) == modeB);
    CHECK(WorldContextFor(PickWorldContext(modeB, t.recorded, false), modeB, t.recorded) == nullptr);

    if (g_fail) { std::printf("test_hsm_world_context: %d failure(s)\n", g_fail); return 1; }
    std::printf("test_hsm_world_context: ok\n");
    return 0;
}
