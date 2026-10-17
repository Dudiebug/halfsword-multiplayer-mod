#include "hsm_stall.h"
#include <cassert>
#include <cstring>
#include <thread>

using namespace hsm::diagnostics;
int main() {
    StallTrace trace;
    StallTrace::Snapshot s;
    trace.Heartbeat(100);
    auto outer = trace.Push("tick", 0, 101);
    auto inner = trace.Push("claim", 19, 102);
    trace.Name(inner, "Deal Complex Damage", 103);
    trace.Stage(inner, "native body", 104);
    assert(trace.Read(s) && s.depth == 2 && s.heartbeat == 100 && s.progress == 104);
    assert(s.frames[1].detail == 19 && s.frames[1].since == 102);
    assert(std::strcmp(s.frames[1].name.data(), "Deal Complex Damage") == 0);
    trace.Pop(inner, 105);
    assert(trace.Read(s) && s.depth == 1 && std::strcmp(s.frames[0].stage, "tick") == 0);
    trace.Pop(outer, 106);
    assert(trace.Read(s) && s.depth == 0);
    // Deep re-entry cannot overwrite the outer breadcrumbs; unwind restores them.
    for (size_t i = 0; i < 40; ++i) assert(trace.Push("recursive", i, 200) == i);
    assert(trace.Read(s) && s.depth == 40 && s.frames[31].detail == 31);
    trace.Name(39, "omitted", 201); trace.Stage(39, "omitted", 201);
    for (size_t i = 40; i; --i) trace.Pop(i - 1, 202);
    auto reused = trace.Push("reused", 20, 203);
    assert(trace.Read(s) && s.frames[0].name[0] == 0);
    trace.Name(reused, "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-long", 204);
    assert(trace.Read(s) && std::strlen(s.frames[0].name.data()) == 63);
    // Concurrent publication must never expose a frame assembled from two calls.
    StallTrace concurrent;
    std::atomic<bool> done{false};
    std::thread producer([&] {
        for (uint64_t i = 1; i <= 10000; ++i) {
            auto slot = concurrent.Push("concurrent", i, i);
            concurrent.Name(slot, "native", i);
            concurrent.Pop(slot, i);
        }
        done = true;
    });
    do {
        if (concurrent.Read(s) && s.depth) assert(s.depth == 1 && s.frames[0].since == s.frames[0].detail);
    } while (!done.load());
    producer.join();
    assert(concurrent.Read(s) && s.depth == 0);
    StallAlarm alarm;
    assert(alarm.Poll(90000, 0) == StallAlarm::Event::None);
    assert(alarm.Poll(5099, 100) == StallAlarm::Event::None);
    assert(alarm.Poll(5100, 100) == StallAlarm::Event::Stalled);
    assert(alarm.Poll(15099, 100) == StallAlarm::Event::None);
    assert(alarm.Poll(15100, 100) == StallAlarm::Event::Stalled);
    assert(alarm.Poll(15101, 15100) == StallAlarm::Event::Recovered);
    assert(alarm.Poll(15102, 15100) == StallAlarm::Event::None);
    assert(alarm.Poll(20100, 15100) == StallAlarm::Event::Stalled);
}
