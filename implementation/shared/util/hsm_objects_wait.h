// The startup wait for the game's object array (GObjects), read by the engine layer before anything touches it.
//
// Until 0.8.0-pre53 the mod gave up after 60 s with fewer than 10000 objects, and stayed off for that launch (#3079).
// Reports showed that a slow cold start (median 3.4 s, but a tail past 60 s, some machines stuck at about 4681 objects
// for a long time) is the cause, not a wrong address: every such machine worked on its next launch. So the wait is long
// (10 min), it stops at once only on signs of a wrong array, and it logs progress so a slow start shows in the log.
#pragma once
#include <cstdint>
#include <initializer_list>

namespace hsm::util {

constexpr int32_t kObjectsReady = 10000;          // the engine is up past CoreUObject
constexpr int32_t kObjectsChunk = 64 * 1024;      // UE's elements per chunk; MaxElements is a whole number of chunks
constexpr uint64_t kObjectsWaitMs = 10 * 60 * 1000;
constexpr uint64_t kObjectsWrongMs = 1000;        // a wrong look must last this long (not a torn read during set-up)

struct ObjectArrayLook {
    bool hasObjects = false;   // the chunk table pointer is set
    int32_t maxElements = 0, numElements = 0, maxChunks = 0, numChunks = 0;
};

enum class ObjectsState { Waiting, Ready, Wrong };

inline ObjectsState LookAtObjects(const ObjectArrayLook& a) {
    if (a.maxElements < 0 || a.numElements < 0 || a.maxChunks < 0 || a.numChunks < 0) return ObjectsState::Wrong;
    if (a.maxElements == 0)   // not allocated yet: everything still zero
        return a.numElements == 0 && a.numChunks == 0 ? ObjectsState::Waiting : ObjectsState::Wrong;
    if (a.maxElements % kObjectsChunk != 0 || a.numElements > a.maxElements || a.numChunks > a.maxChunks)
        return ObjectsState::Wrong;
    return a.numElements >= kObjectsReady && a.hasObjects ? ObjectsState::Ready : ObjectsState::Waiting;
}

// The wait's decision after waitedMs, given how long the array has looked wrong without a break (wrongMs).
enum class ObjectsWait { KeepWaiting, Go, GiveUpWrong, GiveUpSlow };

inline ObjectsWait NextObjectsWait(ObjectsState s, uint64_t waitedMs, uint64_t wrongMs) {
    if (s == ObjectsState::Ready) return ObjectsWait::Go;
    if (s == ObjectsState::Wrong && wrongMs >= kObjectsWrongMs) return ObjectsWait::GiveUpWrong;
    if (waitedMs >= kObjectsWaitMs) return s == ObjectsState::Wrong ? ObjectsWait::GiveUpWrong : ObjectsWait::GiveUpSlow;
    return ObjectsWait::KeepWaiting;
}

// True when a "still waiting" line is due: once each at 30 s, 60 s and 120 s, then every 2 min. lastMs is the waited
// time at the previous poll.
inline bool ObjectsWaitLogDue(uint64_t lastMs, uint64_t nowMs) {
    if (nowMs <= lastMs) return false;
    for (const uint64_t at : {30000ull, 60000ull}) if (lastMs < at && nowMs >= at) return true;
    constexpr uint64_t kEvery = 120000;
    return nowMs >= kEvery && lastMs / kEvery != nowMs / kEvery;
}

}  // namespace hsm::util
