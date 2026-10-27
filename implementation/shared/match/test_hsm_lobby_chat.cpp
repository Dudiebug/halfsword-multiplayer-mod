// Lobby chat (hsm_lobby_chat.h): the per-sender rate limit and the sender's colour.
#include "match/hsm_lobby_chat.h"
#include <cstdio>

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++g_fail; } } while (0)
using namespace hsm;

static bool Same(Rgba a, Rgba b) { return a.r == b.r && a.g == b.g && a.b == b.b && a.a == b.a; }

static void TestGate() {
    LobbyChatGate g;
    uint32_t t = 5000;
    for (uint32_t i = 0; i < kChatBurst; ++i) CHECK(g.Allow(1, t));
    CHECK(!g.Allow(1, t));                                              // one sender's burst is spent...
    CHECK(g.Allow(2, t));                                               // ...another's is not
    CHECK(g.Allow(1, t + kChatRefillMs));                               // one line per refill
    CHECK(!g.Allow(1, t + kChatRefillMs));
    CHECK(g.tracked() == 2);
    g.Forget(1);
    CHECK(g.tracked() == 1 && g.Allow(1, t));                           // a member who left and came back starts fresh
    g.Clear();
    CHECK(g.tracked() == 0);
}

static void TestInk() {
    CHECK(Same(LobbyChatInk(Format::TwoTeams, 0, -1), style::TeamInk(0)));
    CHECK(Same(LobbyChatInk(Format::TwoTeams, 1, -1), style::TeamInk(1)));
    CHECK(Same(LobbyChatInk(Format::TwoTeams, 0, 1), style::TeamInk(1)));      // the host's move wins
    CHECK(Same(LobbyChatInk(Format::TwoTeams, -1, -1), style::kIron));         // no pick: Auto
    CHECK(Same(LobbyChatInk(Format::TwoTeams, 2, -1), style::kIron));          // a team the format does not have
    CHECK(Same(LobbyChatInk(Format::TwoTeams, 3, 3), style::kIron));
    CHECK(Same(LobbyChatInk(Format::ThreeTeams, 2, -1), style::TeamInk(2)));
    CHECK(Same(LobbyChatInk(Format::Duel, 0, 1), style::kIron));               // no teams in this format
}

int main() {
    TestGate();
    TestInk();
    if (g_fail) { std::printf("%d lobby chat checks failed\n", g_fail); return 1; }
    std::printf("all lobby chat tests passed\n");
    return 0;
}
