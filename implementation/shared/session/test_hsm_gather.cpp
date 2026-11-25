// Unit tests for hsm_gather.h (the Crypts gather's wire, protocol rev 47; the flags byte rev 48). Run via implementation/shared/run_tests.sh test_hsm_gather.
#include "session/hsm_gather.h"
#include "session/hsm_budget.h"
#include <cstdio>

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++g_fail; } } while (0)

using namespace hsm;

static void TestClockMsg() {
    GatherMsg m; m.phase = GatherPhase::Gathering; m.leftMs = 599999; m.seq = 0x1234;
    std::vector<uint8_t> b = Encode(m);
    MsgType t; CHECK(PeekType(b.data(), b.size(), t) && t == MsgType::Gather && uint8_t(t) == 71);
    GatherMsg y; CHECK(Decode(b.data(), b.size(), y) && y.phase == GatherPhase::Gathering && y.leftMs == 599999 && y.seq == 0x1234);
    for (size_t cut = 0; cut < b.size(); ++cut) CHECK(!Decode(b.data(), cut, y));
    { auto x = b; x.push_back(0); CHECK(!Decode(x.data(), x.size(), y)); }              // nothing after it
    GatherNoteMsg note; CHECK(!Decode(b.data(), b.size(), note));                        // the host's is no note
    for (uint8_t p = 0; p < kGatherPhaseCount; ++p) { GatherMsg z = m; z.phase = GatherPhase(p); auto e = Encode(z); CHECK(Decode(e.data(), e.size(), y) && uint8_t(y.phase) == p); }
    { GatherMsg z = m; z.phase = GatherPhase(3); auto e = Encode(z); CHECK(!Decode(e.data(), e.size(), y)); }
    { GatherMsg z = m; z.leftMs = uint32_t(kGatherMaxS) * 1000u; auto e = Encode(z); CHECK(Decode(e.data(), e.size(), y) && y.leftMs == 3600000); }
    { GatherMsg z = m; z.leftMs = uint32_t(kGatherMaxS) * 1000u + 1; auto e = Encode(z); CHECK(!Decode(e.data(), e.size(), y)); }
    {
    // Co-op Crypts: no clock, only while gathering and Together
    GatherMsg u;
    { GatherMsg z{GatherPhase::Gathering, kGatherUntimedMs, 4, true}; auto e = Encode(z); CHECK(Decode(e.data(), e.size(), u) && GatherUntimed(u.leftMs) && u.together); }
    { GatherMsg z{GatherPhase::Gathering, kGatherUntimedMs, 4, false}; auto e = Encode(z); CHECK(!Decode(e.data(), e.size(), u)); }
    { GatherMsg z{GatherPhase::Collecting, kGatherUntimedMs, 4, true}; auto e = Encode(z); CHECK(!Decode(e.data(), e.size(), u)); }
    { GatherMsg z{GatherPhase::Gathering, kGatherUntimedMs - 1, 4, true}; auto e = Encode(z); CHECK(!Decode(e.data(), e.size(), u)); }
    }
    { GatherMsg z = m; z.seq = 0; auto e = Encode(z); CHECK(!Decode(e.data(), e.size(), y)); }
    CHECK(b.back() == 0 && !y.together);                                                 // Apart: no flag
    { GatherMsg z = m; z.together = true; auto e = Encode(z); CHECK(e.size() == b.size() && e.back() == kGatherTogether);
      CHECK(Decode(e.data(), e.size(), y) && y.together && y.phase == GatherPhase::Gathering && y.leftMs == 599999 && y.seq == 0x1234); }
    { auto x = b; x.back() = kGatherTogether; CHECK(Decode(x.data(), x.size(), y) && y.together); }
    for (int f = 2; f < 256; ++f) { auto x = b; x.back() = uint8_t(f); CHECK(!Decode(x.data(), x.size(), y)); }   // no other flag
    { GatherMsg z = m; z.together = true; auto e = Encode(z); e.back() = 3; CHECK(!Decode(e.data(), e.size(), y)); }
    { auto x = b; x.back() = kGatherTogether; CHECK(Decode(x.data(), x.size(), y)); x.back() = 0; CHECK(Decode(x.data(), x.size(), y) && !y.together); }   // a reused message reads afresh
    { GatherMsg z; z.phase = GatherPhase::Off; z.leftMs = 0; z.seq = 1; auto e = Encode(z); CHECK(Decode(e.data(), e.size(), y) && y.phase == GatherPhase::Off); }
}

static void TestNoteMsg() {
    GatherNoteMsg m; m.note = GatherNote::Extracted; m.seq = 9;
    std::vector<uint8_t> b = Encode(m);
    MsgType t; CHECK(PeekType(b.data(), b.size(), t) && t == MsgType::Gather);
    GatherNoteMsg y; CHECK(Decode(b.data(), b.size(), y) && y.note == GatherNote::Extracted && y.seq == 9);
    for (size_t cut = 0; cut < b.size(); ++cut) CHECK(!Decode(b.data(), cut, y));
    { auto x = b; x.push_back(0); CHECK(!Decode(x.data(), x.size(), y)); }
    GatherMsg clock; CHECK(!Decode(b.data(), b.size(), clock));                         // a note is no clock
    for (uint8_t n = 0; n < 8; ++n) {
        GatherNoteMsg z = m; z.note = GatherNote(n); auto e = Encode(z);
        CHECK(Decode(e.data(), e.size(), y) == ValidGatherNote(n));
    }
    { GatherNoteMsg z = m; z.seq = 0; auto e = Encode(z); CHECK(!Decode(e.data(), e.size(), y)); }
}

static void TestBudget() {
    CHECK(BudgetClassOf(MsgType::Gather) == BudgetClass::Control);
    CHECK(kProtocolRevision == 52 && kLastDecodedMsgType >= uint8_t(MsgType::Gather));
}

int main() {
    TestClockMsg();
    TestNoteMsg();
    TestBudget();
    if (g_fail) { std::printf("test_hsm_gather: %d failure(s)\n", g_fail); return 1; }
    std::printf("test_hsm_gather: ok\n");
    return 0;
}
