// The attach latch (session/hsm_object_attach.h): an Attached object is held against BodyStates already in flight from its
// attacher, until a Detached/Destroyed, a new grant, or the in-flight window has passed.
#include "session/hsm_object_attach.h"
#include <cstdio>

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++g_fail; } } while (0)
using namespace hsm;

static void TestBlocks() {
    AttachLatch l;
    const uint64_t id = 0x4000000000000001ull;
    CHECK(!l.Blocks(id, 2, 0.0) && !l.Latched(id));                       // nothing attached: nothing blocked
    l.OnAttached(id, 2, LeaseLevel::Hold, 1000.0);
    CHECK(l.Latched(id) && l.Size() == 1);
    CHECK(l.Blocks(id, 2, 1000.0));                                        // the attacher's state, right after
    CHECK(l.Blocks(id, 2, 1000.0 + kAttachStaleMs - 1.0));
    CHECK(!l.Blocks(id, 3, 1500.0));                                       // another streamer is not held
    CHECK(!l.Blocks(id + 1, 2, 1500.0));                                   // another object neither
    CHECK(l.Blocks(id, 2, 1000.0));                                        // still latched after those
    // Past the window the latch is gone: a state from the attacher is a let-go streamed again.
    CHECK(!l.Blocks(id, 2, 1000.0 + kAttachStaleMs) && !l.Latched(id));
    CHECK(!l.Blocks(id, 2, 1000.0 + kAttachStaleMs + 1.0));
}

static void TestEnds() {
    AttachLatch l;
    const uint64_t id = 0x4000000000000002ull;
    l.OnAttached(id, 1, LeaseLevel::Touch, 0.0);
    l.OnDetached(id); CHECK(!l.Blocks(id, 1, 10.0) && !l.Latched(id));
    // A grant to the same holder at the same level does not end it; another holder or level does.
    l.OnAttached(id, 1, LeaseLevel::Touch, 0.0);
    l.OnGrant(id, 1, LeaseLevel::Touch); CHECK(l.Blocks(id, 1, 10.0));
    l.OnGrant(id, 1, LeaseLevel::Hold); CHECK(!l.Latched(id));
    l.OnAttached(id, 1, LeaseLevel::Touch, 0.0);
    l.OnGrant(id, 0, LeaseLevel::Touch); CHECK(!l.Blocks(id, 1, 10.0));
    l.OnGrant(id + 5, 0, LeaseLevel::Free);                                 // unknown object: nothing happens
    l.OnDetached(id + 5);
    // A second Attached refreshes the window and the holder.
    l.OnAttached(id, 1, LeaseLevel::Touch, 0.0);
    l.OnAttached(id, 2, LeaseLevel::Hold, 1500.0);
    CHECK(l.Size() == 1 && !l.Blocks(id, 1, 1600.0) && l.Blocks(id, 2, 3000.0) && !l.Blocks(id, 2, 3500.0));
    l.OnAttached(id, 2, LeaseLevel::Hold, 4000.0); l.Clear(); CHECK(l.Size() == 0 && !l.Latched(id));
}

static void TestBounded() {
    AttachLatch l;
    for (size_t i = 0; i < kMaxAttachLatches; ++i) l.OnAttached(0x4000000000000000ull + i, 1, LeaseLevel::Hold, 0.0);
    CHECK(l.Size() == kMaxAttachLatches);
    const uint64_t extra = 0x4000000000000000ull + kMaxAttachLatches + 5;
    l.OnAttached(extra, 1, LeaseLevel::Hold, 0.0);
    CHECK(l.Size() == kMaxAttachLatches && !l.Latched(extra));              // full: a new one is not held
    l.OnAttached(0x4000000000000000ull, 2, LeaseLevel::Hold, 5.0);           // but a known one is refreshed
    CHECK(l.Blocks(0x4000000000000000ull, 2, 6.0) && l.Size() == kMaxAttachLatches);
}

int main() {
    TestBlocks(); TestEnds(); TestBounded();
    if (g_fail) { std::printf("test_hsm_object_attach: %d FAILED\n", g_fail); return 1; }
    std::printf("test_hsm_object_attach: all passed\n");
    return 0;
}
