// Unit tests for hsm_mod_link.h: the page HSMMod and Fetch share. The layout checks here are the contract between the two
// repositories: a change that fails them breaks the other mod's released builds.
// Run via implementation/shared/run_tests.sh test_hsm_mod_link.
#include "interop/hsm_mod_link.h"
#include <array>
#include <cstdio>

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++g_fail; } } while (0)

using namespace hsm::interop;
using Page = std::array<uint8_t, kLinkBytes>;

static uint32_t At(const Page& p, uint32_t byte) {
    uint32_t v = 0;
    std::memcpy(&v, p.data() + byte, 4);
    return v;
}
static void Put(Page& p, uint32_t byte, uint32_t v) { std::memcpy(p.data() + byte, &v, 4); }
static HsmState Hsm(bool inSession, bool typing, bool replaying, bool hostPlaysMine, SyncMode mode = SyncMode::None) {
    HsmState s;
    s.inSession = inSession; s.typing = typing; s.replayingInput = replaying; s.hostPlaysMine = hostPlaysMine; s.syncMode = mode;
    return s;
}
static FetchState Fetch(bool menuOpen, bool typing) {
    FetchState s;
    s.menuOpen = menuOpen; s.typing = typing;
    return s;
}

static void TestLayout() {
    CHECK(kLinkBytes == 256 && kLinkMagic == 0x4B4E4C48 && kHsmAt == 16 && kFetchAt == 136 && kHalfBytes == 120);
    CHECK(kHsmAt + kHalfBytes == kFetchAt && kFetchAt + kHalfBytes == kLinkBytes);
    CHECK(LinkName(1234) == L"Local\\HSMLink-1234");
    CHECK(LinkName(1) != LinkName(2));
    CHECK(kHsmInSession == 1 && kHsmTyping == 2 && kHsmReplayingInput == 4 && kHsmHostPlaysMine == 8);
    CHECK(kFetchMenuOpen == 1 && kFetchTyping == 2);
    CHECK(static_cast<uint32_t>(SyncMode::None) == 0 && static_cast<uint32_t>(SyncMode::Owner) == 1 && static_cast<uint32_t>(SyncMode::Host) == 2);

    Page p{};
    WriteHsm(p.data(), Hsm(true, false, true, true, SyncMode::Host));
    CHECK(At(p, 0) == kLinkMagic);
    CHECK(At(p, 16) == 16);    // size
    CHECK(At(p, 20) == 1);     // beat
    CHECK(At(p, 24) == 13);    // flags: session, replaying, host plays mine
    CHECK(At(p, 28) == 2);     // sync mode
    for (uint32_t i = 32; i < kLinkBytes; ++i) CHECK(p[i] == 0);   // nothing past its four words, nothing in Fetch's half

    Page q{};
    WriteFetch(q.data(), Fetch(true, true));
    CHECK(At(q, 0) == kLinkMagic);
    CHECK(At(q, 136) == 16 && At(q, 140) == 1 && At(q, 144) == 3 && At(q, 148) == 0);
    for (uint32_t i = 4; i < kFetchAt; ++i) CHECK(q[i] == 0);      // nothing in HSMMod's half
    for (uint32_t i = 152; i < kLinkBytes; ++i) CHECK(q[i] == 0);
}

static void TestRoundTrip() {
    Page p{};
    CHECK(!LinkValid(p.data()) && !LinkValid(nullptr));
    CHECK(ReadHsm(p.data()) == HsmState() && ReadFetch(p.data()) == FetchState());   // nobody there
    CHECK(ReadHsm(nullptr) == HsmState() && ReadFetch(nullptr) == FetchState());
    CHECK(HsmBeat(p.data()) == 0 && FetchBeat(p.data()) == 0 && HsmBeat(nullptr) == 0);
    WriteHsm(nullptr, {});
    WriteFetch(nullptr, {});
    SetHsmFlag(nullptr, kHsmReplayingInput, true);

    for (int bits = 0; bits < 16; ++bits) {
        const HsmState s = Hsm((bits & 1) != 0, (bits & 2) != 0, (bits & 4) != 0, (bits & 8) != 0,
                               bits % 3 == 0 ? SyncMode::None : bits % 3 == 1 ? SyncMode::Owner : SyncMode::Host);
        WriteHsm(p.data(), s);
        CHECK(ReadHsm(p.data()) == s);
        CHECK(HsmBeat(p.data()) == static_cast<uint32_t>(bits + 1));
        CHECK(ReadFetch(p.data()) == FetchState() && FetchBeat(p.data()) == 0);   // one half written: the other still not there
    }
    for (int bits = 0; bits < 4; ++bits) {
        const FetchState s = Fetch((bits & 1) != 0, (bits & 2) != 0);
        WriteFetch(p.data(), s);
        CHECK(ReadFetch(p.data()) == s);
        CHECK(FetchBeat(p.data()) == static_cast<uint32_t>(bits + 1));
    }
    CHECK(HsmBeat(p.data()) == 16);   // Fetch's writes left HSMMod's half alone

    // The replay flag alone, in place: the beat and the other flags stay.
    WriteHsm(p.data(), Hsm(true, false, false, false, SyncMode::Host));
    const uint32_t beat = HsmBeat(p.data());
    SetHsmFlag(p.data(), kHsmReplayingInput, true);
    CHECK(ReadHsm(p.data()).replayingInput && ReadHsm(p.data()).inSession && ReadHsm(p.data()).syncMode == SyncMode::Host);
    CHECK(HsmBeat(p.data()) == beat);
    SetHsmFlag(p.data(), kHsmReplayingInput, false);
    CHECK(!ReadHsm(p.data()).replayingInput && ReadHsm(p.data()).inSession);
    CHECK(At(p, 144) == 3);   // and Fetch's flags
}

// Older and newer writers: the size word says what was filled.
static void TestGrows() {
    Page p{};
    Put(p, 0, kLinkMagic);
    // An older HSMMod that filled only size, beat and flags (12 bytes): the sync mode reads as None, not as stray bytes.
    Put(p, 16, 12); Put(p, 20, 7); Put(p, 24, kHsmInSession | kHsmTyping); Put(p, 28, 2);
    HsmState s = ReadHsm(p.data());
    CHECK(s.inSession && s.typing && !s.replayingInput && s.syncMode == SyncMode::None);
    CHECK(HsmBeat(p.data()) == 7);
    // A newer one: more words, flag bits and a sync mode this build does not know. What it knows still reads; the rest is ignored.
    Put(p, 16, 40); Put(p, 24, kHsmInSession | (1u << 9)); Put(p, 28, 9); Put(p, 32, 0xFFFFFFFFu);
    s = ReadHsm(p.data());
    CHECK(s.inSession && !s.typing && !s.replayingInput && !s.hostPlaysMine && s.syncMode == SyncMode::None);
    // A full half.
    Put(p, 16, kHalfBytes);
    CHECK(ReadHsm(p.data()).inSession && HsmBeat(p.data()) == 7);
    // A size that cannot be right: the half reads as not there.
    Put(p, 16, kHalfBytes + 4);
    CHECK(ReadHsm(p.data()) == HsmState() && HsmBeat(p.data()) == 0);
    Put(p, 16, 0);
    CHECK(ReadHsm(p.data()) == HsmState() && HsmBeat(p.data()) == 0);
    Put(p, 16, 4);    // size only
    CHECK(ReadHsm(p.data()) == HsmState() && HsmBeat(p.data()) == 0);
    Put(p, 16, 8);    // size and beat
    CHECK(ReadHsm(p.data()) == HsmState() && HsmBeat(p.data()) == 7);
    Put(p, 16, 11);   // a word cut short does not count
    CHECK(ReadHsm(p.data()) == HsmState());

    // The same for Fetch's half.
    Put(p, 136, 12); Put(p, 140, 3); Put(p, 144, kFetchMenuOpen | (1u << 20));
    CHECK(ReadFetch(p.data()).menuOpen && !ReadFetch(p.data()).typing && FetchBeat(p.data()) == 3);
    Put(p, 136, 200);
    CHECK(ReadFetch(p.data()) == FetchState() && FetchBeat(p.data()) == 0);

    // No magic: nothing reads, whatever the halves hold.
    Put(p, 16, 16); Put(p, 136, 16); Put(p, 0, 0);
    CHECK(ReadHsm(p.data()) == HsmState() && ReadFetch(p.data()) == FetchState() && HsmBeat(p.data()) == 0 && FetchBeat(p.data()) == 0);
}

static void TestWatch() {
    LinkWatch w;
    CHECK(!w.live());
    CHECK(!w.Tick(0, 16));                // never written
    CHECK(!w.Tick(0, 5000));
    CHECK(w.Tick(1, 16));                 // first beat
    CHECK(w.Tick(2, 16) && w.Tick(3, 16));
    CHECK(w.Tick(3, 1000) && w.live());   // quiet for a second: still there
    CHECK(w.Tick(3, 999));                // 1999 ms
    CHECK(!w.Tick(3, 1) && !w.live());    // 2000 ms: gone
    CHECK(!w.Tick(3, 16));
    CHECK(w.Tick(4, 16));                 // back
    CHECK(!w.Tick(4, 0xFFFFFFF0u));       // one huge tick does not wrap round to live
    CHECK(!w.Tick(4, 0xFFFFFFF0u) && !w.Tick(4, 16));
    CHECK(w.Tick(5, 16));
    w.Reset();
    CHECK(!w.live() && !w.Tick(0, 16));
    // The other mod restarted its count at a lower beat: that is a move too.
    CHECK(w.Tick(9, 16) && w.Tick(1, 16));
    // A beat of zero after a live one (the page was made again) is not a beat.
    LinkWatch z;
    CHECK(z.Tick(7, 16));
    CHECK(z.Tick(0, 1000) && z.Tick(0, 999) && !z.Tick(0, 1));
    // Any first beat counts, and the tick that brings it is not quiet time.
    LinkWatch y;
    CHECK(y.Tick(0x80000000u, 1) && y.Tick(0x80000000u, 1999) && !y.Tick(0x80000000u, 1));
}

static void TestRules() {
    const HsmState idle, typing = Hsm(false, true, false, false), replay = Hsm(false, false, true, false);
    CHECK(FetchMenuMayOpen(true, false, idle));       // offline: as always, link or not
    CHECK(FetchMenuMayOpen(true, false, typing));
    CHECK(FetchMenuMayOpen(true, true, typing));
    CHECK(!FetchMenuMayOpen(false, false, idle));     // lobby, an HSMMod without the link: shut, as before
    CHECK(FetchMenuMayOpen(false, true, idle));       // lobby with the link
    CHECK(!FetchMenuMayOpen(false, true, typing));    // ; in chat is a semicolon
    CHECK(FetchMenuMayOpen(false, true, replay));
    CHECK(!FetchMenuMustClose(true, false, typing) && !FetchMenuMustClose(false, true, idle));
    CHECK(FetchMenuMustClose(false, true, typing) && FetchMenuMustClose(false, false, idle));

    CHECK(FetchSkipsInput(true, replay));
    CHECK(!FetchSkipsInput(false, replay));           // a dead half cannot stop recording
    CHECK(!FetchSkipsInput(true, idle) && !FetchSkipsInput(true, typing));

    const FetchState none, menu = Fetch(true, false), name = Fetch(false, true), both = Fetch(true, true);
    CHECK(HsmKeysBlocked(true, menu) && HsmKeysBlocked(true, name) && HsmKeysBlocked(true, both));
    CHECK(!HsmKeysBlocked(true, none));
    CHECK(!HsmKeysBlocked(false, menu) && !HsmKeysBlocked(false, both));   // Fetch gone with its menu flag set: HSMMod's keys work
}

int main() {
    TestLayout(); TestRoundTrip(); TestGrows(); TestWatch(); TestRules();
    if (g_fail) { std::printf("%d failed\n", g_fail); return 1; }
    std::puts("ok");
    return 0;
}
