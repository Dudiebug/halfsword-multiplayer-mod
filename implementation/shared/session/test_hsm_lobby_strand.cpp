// A machine left out of its own lobby (session/hsm_lobby_strand.h).
#include "session/hsm_lobby_strand.h"
#include <cstdio>

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++g_fail; } } while (0)
using namespace hsm;
using S = StrandStep;

int main() {
    const uint64_t lobby = 77, other = 88, owner = 5;

    // Criterion (#4296): the old host's Steam connection dropped and came back; Steam no longer lists it in its lobby and
    // names no owner. Once Steam is connected and that lasts kStrandConfirmMs, it rejoins, once.
    {
        LobbyStrand s;
        CHECK(s.Tick(lobby, true, owner, true, 1000) == S::Member);
        CHECK(s.Tick(lobby, false, 0, false, 2000) == S::Waiting);          // Steam disconnected: not yet
        CHECK(s.Tick(lobby, false, 0, false, 9000) == S::Waiting);          // however long it lasts
        CHECK(s.Tick(lobby, false, 0, true, 10000) == S::Waiting);          // back: the wait starts now
        CHECK(s.Tick(lobby, false, 0, true, 10000 + kStrandConfirmMs - 1) == S::Waiting);
        CHECK(s.Tick(lobby, false, 0, true, 10000 + kStrandConfirmMs) == S::Rejoin);
        CHECK(s.Tick(lobby, false, 0, true, 10000 + kStrandConfirmMs + 1) == S::Waiting);   // returned once
        CHECK(s.Tick(lobby, false, 0, true, 10000 + 2 * kStrandConfirmMs + 1) == S::Waiting);   // and no GaveUp for it either
        CHECK(s.Tick(lobby, false, 0, true, 20000) == S::Waiting);
        CHECK(s.Tick(lobby, true, 9, true, 21000) == S::Member);            // in again, under the new owner
    }
    // A short gap in the list (Steam updating it) is not a strand.
    {
        LobbyStrand s;
        CHECK(s.Tick(lobby, false, owner, true, 0) == S::Waiting);
        CHECK(s.Tick(lobby, true, owner, true, kStrandConfirmMs - 1) == S::Member);
        CHECK(s.Tick(lobby, false, owner, true, kStrandConfirmMs) == S::Waiting);   // the wait starts over
        CHECK(s.Tick(lobby, false, owner, true, 2 * kStrandConfirmMs - 1) == S::Waiting);
        CHECK(s.Tick(lobby, false, owner, true, 2 * kStrandConfirmMs) == S::Rejoin);
    }
    // Listed but no owner, or an owner but not listed: either way out of it.
    {
        LobbyStrand s;
        CHECK(s.Tick(lobby, true, 0, true, 0) == S::Waiting);
        CHECK(s.Tick(lobby, true, 0, true, kStrandConfirmMs) == S::Rejoin);
        LobbyStrand t;
        CHECK(t.Tick(lobby, false, owner, true, 0) == S::Waiting);
        CHECK(t.Tick(lobby, false, owner, true, kStrandConfirmMs) == S::Rejoin);
    }
    // A disconnect in the middle of the wait starts it over once Steam is back.
    {
        LobbyStrand s;
        CHECK(s.Tick(lobby, false, 0, true, 0) == S::Waiting);
        CHECK(s.Tick(lobby, false, 0, false, kStrandConfirmMs - 1) == S::Waiting);
        CHECK(s.Tick(lobby, false, 0, true, kStrandConfirmMs) == S::Waiting);
        CHECK(s.Tick(lobby, false, 0, true, 2 * kStrandConfirmMs - 1) == S::Waiting);
        CHECK(s.Tick(lobby, false, 0, true, 2 * kStrandConfirmMs) == S::Rejoin);
    }
    // Left out again soon after the rejoin: no loop; GaveUp once, then a rejoin is allowed again after kStrandRetryMs.
    {
        LobbyStrand s;
        CHECK(s.Tick(lobby, false, 0, true, 0) == S::Waiting);
        CHECK(s.Tick(lobby, false, 0, true, kStrandConfirmMs) == S::Rejoin);
        CHECK(s.Tick(lobby, true, owner, true, 5000) == S::Member);
        CHECK(s.Tick(lobby, false, 0, true, 6000) == S::Waiting);
        CHECK(s.Tick(lobby, false, 0, true, 6000 + kStrandConfirmMs) == S::GaveUp);
        CHECK(s.Tick(lobby, false, 0, true, 6000 + kStrandConfirmMs + 1) == S::Waiting);   // once
        CHECK(s.Tick(lobby, true, owner, true, 20000) == S::Member);
        const uint32_t later = kStrandConfirmMs + kStrandRetryMs;
        CHECK(s.Tick(lobby, false, 0, true, later) == S::Waiting);
        CHECK(s.Tick(lobby, false, 0, true, later + kStrandConfirmMs) == S::Rejoin);
    }
    // Another lobby is its own: a recent rejoin of the first does not stop one of the second, and the wait starts over.
    {
        LobbyStrand s;
        CHECK(s.Tick(lobby, false, 0, true, 0) == S::Waiting);
        CHECK(s.Tick(lobby, false, 0, true, kStrandConfirmMs) == S::Rejoin);
        CHECK(s.Tick(other, false, 0, true, kStrandConfirmMs + 1) == S::Waiting);
        CHECK(s.Tick(other, false, 0, true, 2 * kStrandConfirmMs) == S::Waiting);
        CHECK(s.Tick(other, false, 0, true, 2 * kStrandConfirmMs + 1) == S::Rejoin);
    }
    // Moved to another lobby in the middle of a wait: the wait starts over for that one.
    {
        LobbyStrand s;
        CHECK(s.Tick(lobby, false, 0, true, 0) == S::Waiting);
        CHECK(s.Tick(other, false, 0, true, 2000) == S::Waiting);
        CHECK(s.Tick(other, false, 0, true, 2000 + kStrandConfirmMs - 1) == S::Waiting);
        CHECK(s.Tick(other, false, 0, true, 2000 + kStrandConfirmMs) == S::Rejoin);
    }
    // A member the whole time never rejoins; the 32-bit clock wraps without a false strand.
    {
        LobbyStrand s;
        for (uint32_t t = 0xFFFFF000u; t != 0x00002000u; t += 512) CHECK(s.Tick(lobby, true, owner, true, t) == S::Member);
        CHECK(s.Tick(lobby, false, 0, true, 0xFFFFFF00u) == S::Waiting);
        CHECK(s.Tick(lobby, false, 0, true, 0xFFFFFF00u + kStrandConfirmMs - 1) == S::Waiting);   // past the wrap
        CHECK(s.Tick(lobby, false, 0, true, 0xFFFFFF00u + kStrandConfirmMs) == S::Rejoin);
    }
    if (g_fail) { std::printf("%d lobby strand check(s) failed\n", g_fail); return 1; }
    std::printf("lobby strand tests passed\n");
    return 0;
}
