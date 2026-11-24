// #3282 hand-field dump: which names count, and the dump schedule (now, again 2 s after the latest grip change, never
// two within 2 s, at most 200).
#include "sync/hsm_hand_dump.h"
#include <cstdio>

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++g_fail; } } while (0)
using namespace hsm::sync;

static void TestNames() {
    CHECK(IsHandFieldName("R Fisting Rate") && IsHandFieldName("Fist R Spawned"));
    CHECK(IsHandFieldName("Grab L Target Found") && IsHandFieldName("R Hand Constraint Locked"));
    CHECK(IsHandFieldName("L Hand Attached Correctly") && IsHandFieldName("Thrust Grip Alpha"));
    CHECK(IsHandFieldName("hand") && IsHandFieldName("HAND") && IsHandFieldName("xLoCkx") && IsHandFieldName("Attach"));
    CHECK(!IsHandFieldName("") && !IsHandFieldName("Thrust Alpha") && !IsHandFieldName("Downed") && !IsHandFieldName("Give Up"));
    CHECK(!IsHandFieldName("han") && !IsHandFieldName("gri p") && !IsHandFieldName("Locomotion") && !IsHandFieldName("Fis"));
    CHECK(IsHandFieldName("Block") == true);   // "Block" holds "lock": a few extra fields are logged, none missed
}

static void TestSchedule() {
    HandDump d;
    CHECK(!d.Due(0) && !d.Due(5000));            // nothing before a grip change
    d.GripChanged(10000);
    CHECK(d.Due(10000));                         // at once
    CHECK(!d.Due(10001) && !d.Due(11999));       // not again within 2 s
    CHECK(d.Due(12000));                         // the settled hand, 2 s after the change
    CHECK(!d.Due(12001) && !d.Due(30000));       // then nothing until the next change
    CHECK(d.Count() == 2);
    // A change within 2 s of the last dump: no dump at once, only the later one.
    d.GripChanged(13000);
    CHECK(!d.Due(13000) && !d.Due(13999));
    CHECK(!d.Due(14500));                        // the gap is over, but the later shot is due at 15000
    CHECK(d.Due(15000) && !d.Due(20000));
    // Changes in quick succession push the later dump back to 2 s after the latest one.
    d.GripChanged(30000);
    CHECK(d.Due(30000));
    d.GripChanged(31000);
    CHECK(!d.Due(31000));
    d.GripChanged(31500);
    CHECK(!d.Due(31500));                        // the rig asks every tick, so the at-once chance is this one
    CHECK(!d.Due(32999) && !d.Due(33499) && d.Due(33500) && !d.Due(40000));
    // The later dump waits for the gap when an at-once dump ran after it was due.
    HandDump g;
    g.GripChanged(0);
    CHECK(g.Due(0));
    g.GripChanged(2500);                         // at once (the gap is over), later at 4500
    CHECK(g.Due(2500) && !g.Due(4499) && g.Due(4500));
    // A missed tick: the at-once dump is taken late, the later one still 2 s after that.
    HandDump m;
    m.GripChanged(0);
    CHECK(m.Due(1500) && !m.Due(2000) && !m.Due(3499) && m.Due(3500));
    // The cap.
    HandDump c;
    for (int i = 0; i < 1000; ++i) { c.GripChanged(i * 10000.0); c.Due(i * 10000.0); c.Due(i * 10000.0 + 5000); }
    CHECK(c.Count() == HandDump::kMax);
    c.GripChanged(1e8);
    CHECK(!c.Due(1e8) && !c.Due(1e8 + 5000));
    CHECK(HandDump::kGapMs == 2000 && HandDump::kLaterMs == 2000 && HandDump::kMax == 200);
}

int main() {
    TestNames();
    TestSchedule();
    if (g_fail) { std::printf("test_hsm_hand_dump: %d failures\n", g_fail); return 1; }
    std::printf("test_hsm_hand_dump: ok\n");
    return 0;
}
