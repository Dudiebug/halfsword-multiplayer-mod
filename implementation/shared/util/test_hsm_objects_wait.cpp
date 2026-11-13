// The GObjects startup wait (#3079): a slow start keeps waiting past 60 s, a wrong-looking array gives up after 1 s of
// looking wrong, progress lines come at 30 s, 60 s, 120 s and every 2 min after.
#include "util/hsm_objects_wait.h"
#include <cstdio>

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++g_fail; } } while (0)
using namespace hsm::util;

static ObjectArrayLook Look(int32_t num, int32_t max = 33 * kObjectsChunk, int32_t numChunks = 1, int32_t maxChunks = 33,
                            bool has = true) {
    ObjectArrayLook a;
    a.hasObjects = has; a.numElements = num; a.maxElements = max; a.numChunks = numChunks; a.maxChunks = maxChunks;
    return a;
}

static void TestStates() {
    CHECK(LookAtObjects(ObjectArrayLook{}) == ObjectsState::Waiting);                 // not allocated yet
    CHECK(LookAtObjects(Look(4684)) == ObjectsState::Waiting);                        // the #3079 log: CoreUObject only
    CHECK(LookAtObjects(Look(4681)) == ObjectsState::Waiting);
    CHECK(LookAtObjects(Look(9999)) == ObjectsState::Waiting);
    CHECK(LookAtObjects(Look(10000)) == ObjectsState::Ready);
    CHECK(LookAtObjects(Look(10003)) == ObjectsState::Ready);
    CHECK(LookAtObjects(Look(65536 * 33)) == ObjectsState::Ready);                    // full
    CHECK(LookAtObjects(Look(20000, 33 * kObjectsChunk, 1, 33, false)) == ObjectsState::Waiting);   // no chunk table
    CHECK(LookAtObjects(Look(20000, 33 * kObjectsChunk + 1)) == ObjectsState::Wrong); // not whole chunks
    CHECK(LookAtObjects(Look(33 * kObjectsChunk + 1)) == ObjectsState::Wrong);        // more than room for
    CHECK(LookAtObjects(Look(20000, 33 * kObjectsChunk, 34, 33)) == ObjectsState::Wrong);
    CHECK(LookAtObjects(Look(-1)) == ObjectsState::Wrong);
    CHECK(LookAtObjects(Look(5, 0, 0, 0)) == ObjectsState::Wrong);                    // count with no array
    CHECK(LookAtObjects(Look(0, 0, 1, 0)) == ObjectsState::Wrong);
    CHECK(LookAtObjects(Look(20000, -kObjectsChunk)) == ObjectsState::Wrong);
}

static void TestDecisions() {
    // A slow start: still waiting at 60 s and 9 min (the old wait gave up at 60 s), giving up only at 10 min.
    CHECK(NextObjectsWait(ObjectsState::Waiting, 60000, 0) == ObjectsWait::KeepWaiting);
    CHECK(NextObjectsWait(ObjectsState::Waiting, 64400, 0) == ObjectsWait::KeepWaiting);
    CHECK(NextObjectsWait(ObjectsState::Waiting, 9 * 60000, 0) == ObjectsWait::KeepWaiting);
    CHECK(NextObjectsWait(ObjectsState::Waiting, kObjectsWaitMs, 0) == ObjectsWait::GiveUpSlow);
    CHECK(NextObjectsWait(ObjectsState::Ready, 0, 0) == ObjectsWait::Go);
    CHECK(NextObjectsWait(ObjectsState::Ready, kObjectsWaitMs + 1, 0) == ObjectsWait::Go);
    // Wrong: a short blip keeps waiting, a lasting one gives up at once, not after 10 min.
    CHECK(NextObjectsWait(ObjectsState::Wrong, 5000, 0) == ObjectsWait::KeepWaiting);
    CHECK(NextObjectsWait(ObjectsState::Wrong, 5000, 900) == ObjectsWait::KeepWaiting);
    CHECK(NextObjectsWait(ObjectsState::Wrong, 5000, kObjectsWrongMs) == ObjectsWait::GiveUpWrong);
    CHECK(NextObjectsWait(ObjectsState::Wrong, kObjectsWaitMs, 0) == ObjectsWait::GiveUpWrong);
}

static void TestLogLines() {
    int lines = 0;
    uint64_t last = 0, firstAt = 0;
    for (uint64_t t = 109; t <= 9 * 60000; t += 109) {   // Sleep(100) polls on 15.6 ms timers
        if (ObjectsWaitLogDue(last, t)) { if (!lines) firstAt = t; ++lines; }
        last = t;
    }
    CHECK(firstAt >= 30000 && firstAt < 30200);
    CHECK(lines == 6);   // 30 s, 60 s, 2, 4, 6 and 8 min
    CHECK(!ObjectsWaitLogDue(0, 29999));
    CHECK(ObjectsWaitLogDue(29999, 30000));
    CHECK(ObjectsWaitLogDue(59999, 60000));
    CHECK(!ObjectsWaitLogDue(60000, 119999));
    CHECK(ObjectsWaitLogDue(119999, 120000));
    CHECK(ObjectsWaitLogDue(239999, 240000));
    CHECK(!ObjectsWaitLogDue(240000, 240000));
    CHECK(!ObjectsWaitLogDue(50000, 40000));
}

int main() {
    TestStates();
    TestDecisions();
    TestLogLines();
    if (g_fail) { std::printf("test_hsm_objects_wait: %d failures\n", g_fail); return 1; }
    std::printf("test_hsm_objects_wait: ok\n");
    return 0;
}
