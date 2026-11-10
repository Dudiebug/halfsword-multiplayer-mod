// Unit tests for hsm_link_hsm.h: what HSMMod writes in its half of the link page, and which changes it logs.
// Run via implementation/shared/run_tests.sh test_hsm_link_hsm.
#include "interop/hsm_link_hsm.h"
#include <array>
#include <cstdio>

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++g_fail; } } while (0)

using namespace hsm::interop;

static HsmFacts Facts(bool inLobby, bool joined, bool isHost, bool hostPhysics, bool typing = false) {
    HsmFacts f;
    f.inLobby = inLobby; f.joined = joined; f.isHost = isHost; f.hostPhysics = hostPhysics; f.typing = typing;
    return f;
}

static void TestState() {
    // Solo play and the menus: nothing but typing.
    HsmState s = HsmLinkState(Facts(false, false, false, false));
    CHECK(s == HsmState());
    s = HsmLinkState(Facts(false, false, false, false, true));
    CHECK(s.typing && !s.inSession && s.syncMode == SyncMode::None);
    // A sync mode left over from the last session does not count outside one.
    s = HsmLinkState(Facts(false, false, false, true));
    CHECK(!s.inSession && !s.hostPlaysMine && s.syncMode == SyncMode::None);
    s = HsmLinkState(Facts(false, false, true, true));
    CHECK(!s.hostPlaysMine && s.syncMode == SyncMode::None);

    // A lobby, before the host has welcomed this machine: in a session already.
    s = HsmLinkState(Facts(true, false, false, false));
    CHECK(s.inSession && s.syncMode == SyncMode::Owner && !s.hostPlaysMine);
    // Joined with no lobby (the lobby went first): still in the session.
    s = HsmLinkState(Facts(false, true, false, false));
    CHECK(s.inSession && s.syncMode == SyncMode::Owner);

    // Owner sync: everyone plays their own fighter.
    CHECK(!HsmLinkState(Facts(true, true, false, false)).hostPlaysMine);
    CHECK(!HsmLinkState(Facts(true, true, true, false)).hostPlaysMine);
    // Host physics: the host plays its own fighter; a client's is played on the host.
    s = HsmLinkState(Facts(true, true, true, true));
    CHECK(s.inSession && !s.hostPlaysMine && s.syncMode == SyncMode::Host);
    s = HsmLinkState(Facts(true, true, false, true, true));
    CHECK(s.inSession && s.hostPlaysMine && s.typing && s.syncMode == SyncMode::Host);
    CHECK(!s.replayingInput);   // only ever set in place, around a replay

    // Through the page: the state reads back as written, and a full write clears the replay flag.
    std::array<uint8_t, kLinkBytes> p{};
    WriteHsm(p.data(), s);
    SetHsmFlag(p.data(), kHsmReplayingInput, true);
    CHECK(ReadHsm(p.data()).replayingInput);
    WriteHsm(p.data(), s);
    CHECK(ReadHsm(p.data()) == s);
}

static void TestNotes() {
    LinkNotes n;
    LinkNews e = n.Tick(false, false);
    CHECK(!e.arrived && !e.gone && !e.held && !e.released);   // no Fetch: nothing, ever
    e = n.Tick(false, false);
    CHECK(!e.arrived && !e.gone && !e.held && !e.released);
    e = n.Tick(true, false);
    CHECK(e.arrived && !e.gone && !e.held && !e.released);    // Fetch is here: once
    e = n.Tick(true, false);
    CHECK(!e.arrived && !e.gone && !e.held && !e.released);
    e = n.Tick(true, true);
    CHECK(!e.arrived && !e.gone && e.held && !e.released);    // its menu opened: once per opening
    e = n.Tick(true, true);
    CHECK(!e.arrived && !e.gone && !e.held && !e.released);
    e = n.Tick(true, false);
    CHECK(!e.arrived && !e.gone && !e.held && e.released);    // and closed
    e = n.Tick(true, true);
    CHECK(e.held && !e.released);                             // opened again: a new line
    e = n.Tick(false, false);
    CHECK(e.gone && e.released && !e.arrived && !e.held);     // gone with the menu open: both lines
    e = n.Tick(false, false);
    CHECK(!e.arrived && !e.gone && !e.held && !e.released);
    e = n.Tick(true, true);
    CHECK(e.arrived && e.held);                               // back, its menu open at once
}

int main() {
    TestState(); TestNotes();
    if (g_fail) { std::printf("%d failed\n", g_fail); return 1; }
    std::puts("ok");
    return 0;
}
