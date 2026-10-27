// Spectate: who counts as alive for a client / the host, and when the camera is re-applied.
#include "match/hsm_spectate.h"
#include <cstdio>

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++g_fail; } } while (0)
using namespace hsm;

int main() {
    bool yes = true, no = false;
    CHECK(ClientAlive(&yes));
    CHECK(!ClientAlive(&no));
    CHECK(!ClientAlive(nullptr));
    CHECK(HostAlive(true, false));
    CHECK(!HostAlive(true, true));
    CHECK(!HostAlive(false, false));
    CHECK(ShouldApplyView(2, 1, 1000, 1001));           // new target: now
    CHECK(!ShouldApplyView(2, 2, 1000, 1400));          // same, recent
    CHECK(ShouldApplyView(2, 2, 1000, 1500));           // same, 500 ms on
    CHECK(ShouldApplyView(2, 2, 0xFFFFFF00u, 0x200u));  // clock wrap
    if (g_fail) { std::printf("%d failure(s)\n", g_fail); return 1; }
    std::puts("hsm_spectate_alive ok");
    return 0;
}
