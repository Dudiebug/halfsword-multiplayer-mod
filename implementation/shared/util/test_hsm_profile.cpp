// Unit tests for hsm_profile.h. Run via implementation/shared/run_tests.sh test_hsm_profile.
#include "util/hsm_profile.h"
#include <cstdio>
#include <string>

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++g_fail; } } while (0)

using namespace hsm::prof;

static int A, B, C;   // distinct keys
static int g_named = 0;
static std::string Name(const char* n) { ++g_named; return n; }

static const Stat* Find(const Profiler& p, const std::string& name, uint8_t kind) {
    for (const Stat& s : p.stats()) if (s.name == name && s.kind == kind) return &s;
    return nullptr;
}

// A(0..100) calls B(10..40) and B(50..60): A self 60, incl 100; B self = incl = 40 over 2 calls, max 30.
static void TestSelfAndInclusive() {
    Profiler p;
    p.Start(0, {});
    CHECK(p.Begin(&A, kScript, 0, [] { return Name("A"); }));
    CHECK(p.Begin(&B, kScript, 10, [] { return Name("B"); })); p.End(40);
    CHECK(p.Begin(&B, kScript, 50, [] { return Name("B"); })); p.End(60);
    p.End(100);
    p.Stop(100);
    const Stat* a = Find(p, "A", kScript);
    const Stat* b = Find(p, "B", kScript);
    CHECK(a && a->calls == 1 && a->selfTicks == 60 && a->inclTicks == 100 && a->maxTicks == 100);
    CHECK(b && b->calls == 2 && b->selfTicks == 40 && b->inclTicks == 40 && b->maxTicks == 30);
    CHECK(p.calls() == 3);
    CHECK(p.depth() == 0);
}

// The same key with another kind is a separate row; a name is asked once per row.
static void TestKindsAndNaming() {
    Profiler p;
    g_named = 0;
    p.Start(0, {});
    p.Begin(&A, kEvent, 0, [] { return Name("A"); });
    p.Begin(&A, kScript, 1, [] { return Name("A"); });
    p.Begin(&A, kHook, 2, [] { return Name("A"); }); p.End(3);
    p.End(5);
    p.End(9);
    p.Begin(&A, kEvent, 10, [] { return Name("A"); }); p.End(11);
    CHECK(g_named == 3);
    CHECK(p.stats().size() == 3);
    const Stat* ev = Find(p, "A", kEvent);
    const Stat* bp = Find(p, "A", kScript);
    CHECK(ev && ev->calls == 2 && ev->inclTicks == 10 && ev->selfTicks == 5 + 1);
    CHECK(bp && bp->inclTicks == 4 && bp->selfTicks == 3);
}

// Recursion: A(0..100) -> A(10..30). Inclusive counts the outer call only; self splits.
static void TestRecursion() {
    Profiler p;
    p.Start(0, {});
    p.Begin(&A, kScript, 0, [] { return Name("A"); });
    p.Begin(&A, kScript, 10, [] { return Name("A"); }); p.End(30);
    p.End(100);
    const Stat* a = Find(p, "A", kScript);
    CHECK(a && a->calls == 2 && a->inclTicks == 100 && a->selfTicks == 100);
    CHECK(a && a->active == 0);
}

// Not recording: Begin pushes nothing. A call begun before Stop ends without counting; one begun in an earlier capture
// ends in the next capture without touching its stats or its parent.
static void TestCaptureBoundaries() {
    Profiler p;
    CHECK(!p.Begin(&A, kScript, 0, [] { return Name("A"); }));
    CHECK(p.depth() == 0);
    p.Start(0, {});
    CHECK(p.Begin(&A, kScript, 0, [] { return Name("A"); }));   // spans the restart
    p.Stop(5);
    p.Start(10, {});
    CHECK(p.Begin(&B, kScript, 20, [] { return Name("B"); })); p.End(30);
    p.End(40);                                                 // A's End: from the earlier capture
    CHECK(p.depth() == 0);
    CHECK(!Find(p, "A", kScript));
    const Stat* b = Find(p, "B", kScript);
    CHECK(b && b->calls == 1 && b->inclTicks == 10);
    p.Begin(&C, kScript, 50, [] { return Name("C"); });
    p.Stop(55);
    p.End(60);
    const Stat* c = Find(p, "C", kScript);
    CHECK(c && c->calls == 0 && c->active == 0);
    CHECK(p.calls() == 1);
    p.End(70);                                                 // unbalanced End: ignored
    CHECK(p.depth() == 0);
}

static void TestDepthLimit() {
    Profiler p;
    p.Start(0, {});
    size_t pushed = 0;
    for (size_t i = 0; i < Profiler::kMaxDepth + 5; ++i) pushed += p.Begin(&A, kScript, i, [] { return Name("A"); });
    CHECK(pushed == Profiler::kMaxDepth);
    CHECK(p.overflow() == 5);
    for (size_t i = 0; i < pushed; ++i) p.End(1000);
    CHECK(p.depth() == 0);
}

// The timeline keeps calls at least traceMinTicks long, up to traceMaxEvents, and counts the rest.
static void TestTrace() {
    Profiler p;
    p.Start(0, {10, 2});
    p.Begin(&A, kScript, 0, [] { return Name("A"); });
    p.Begin(&B, kScript, 1, [] { return Name("B"); }); p.End(5);     // 4 ticks: too short
    p.Begin(&B, kScript, 5, [] { return Name("B"); }); p.End(20);    // kept, depth 1
    p.End(50);                                                       // kept, depth 0
    p.Begin(&C, kScript, 60, [] { return Name("C"); }); p.End(90);   // long enough, but full
    CHECK(p.trace().size() == 2);
    CHECK(p.dropped() == 1);
    CHECK(p.trace()[0].depth == 1 && p.trace()[0].start == 5 && p.trace()[0].dur == 15);
    CHECK(p.trace()[1].depth == 0 && p.trace()[1].dur == 50);
    Profiler off;
    off.Start(0, {});
    off.Begin(&A, kScript, 0, [] { return Name("A"); }); off.End(100);
    CHECK(off.trace().empty() && off.dropped() == 0);
}

static void TestFramesAndPercentile() {
    Profiler p;
    p.FrameMark(1);                      // not recording
    p.Start(0, {});
    for (uint64_t t : {0, 10, 20, 50, 60}) p.FrameMark(t);
    std::vector<uint64_t> ft = p.FrameTicks();
    CHECK((ft == std::vector<uint64_t>{10, 10, 30, 10}));
    CHECK(Percentile(ft, 0.5) == 10);
    CHECK(Percentile(ft, 0.99) == 30);
    CHECK(Percentile({}, 0.5) == 0);
    p.Stop(60);
    p.FrameMark(70);                     // after the capture: not a frame of it
    CHECK(p.FrameTicks().size() == 4);
}

static void TestReport() {
    Profiler p;
    p.Start(0, {1, 100});
    p.FrameMark(0);
    p.Begin(&A, kEvent, 0, [] { return Name("Willie_BP_C.ReceiveTick"); });
    p.Begin(&B, kScript, 1000, [] { return Name("Willie_BP_C.Get Damage"); }); p.End(3000);
    p.End(4000);
    p.FrameMark(16000);
    p.FrameMark(48000);
    p.Stop(48000);
    ReportInfo r;
    r.ticksPerMs = 1000;
    r.title = "T";
    r.traceFile = "x.json";
    const std::string s = Report(p, r);
    CHECK(s.find("Captured 0.05 s, 2 frames") != std::string::npos);
    CHECK(s.find("max 32.00 ms") != std::string::npos);
    CHECK(s.find("Willie_BP_C.Get Damage") != std::string::npos);
    CHECK(s.find("Top by self time") < s.find("Top by inclusive time"));
    // ReceiveTick: self 2 ms over 2 frames = 1.000 ms/f, incl 4 ms = 2.000 ms/f
    CHECK(s.find("       1.000       2.000        0.5    4000.0    4000.0  event  Willie_BP_C.ReceiveTick") != std::string::npos);
    CHECK(s.find("frame      1     32.00 ms  at     0.016 s") != std::string::npos);   // slowest frame first
    CHECK(s.find("2 calls written to x.json") != std::string::npos);
}

static void TestTraceJson() {
    Profiler p;
    p.Start(100, {0, 10});
    p.FrameMark(100);
    p.Begin(&A, kScript, 100, [] { return std::string("Odd \"name\"\\\n"); }); p.End(2100);
    p.FrameMark(3100);
    std::string json;
    WriteTrace(p, 1000.0, [&](std::string_view s) { json += s; });
    CHECK(json.find("\"name\":\"Odd \\\"name\\\"\\\\\\u000a\",\"ts\":0.000,\"dur\":2000.000}") != std::string::npos);
    CHECK(json.find("\"cat\":\"bp\"") != std::string::npos);
    CHECK(json.find("\"name\":\"Frame 0\",\"ts\":0.000,\"dur\":3000.000}") != std::string::npos);
    CHECK(json.rfind("]}\n") == json.size() - 3);
    int depth = 0, minDepth = 0;   // brackets balance (no string in this trace contains a bracket)
    for (char c : json) { if (c == '{' || c == '[') ++depth; if (c == '}' || c == ']') --depth; minDepth = std::min(minDepth, depth); }
    CHECK(depth == 0 && minDepth == 0);
}

int main() {
    TestSelfAndInclusive();
    TestKindsAndNaming();
    TestRecursion();
    TestCaptureBoundaries();
    TestDepthLimit();
    TestTrace();
    TestFramesAndPercentile();
    TestReport();
    TestTraceJson();
    if (g_fail) { std::printf("test_hsm_profile: %d failure(s)\n", g_fail); return 1; }
    std::printf("test_hsm_profile: all passed\n");
    return 0;
}
