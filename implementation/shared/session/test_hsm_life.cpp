// Body lives (protocol rev 19): LifeBook and LifeCurrent in session/hsm_authority.h.
#include "session/hsm_authority.h"
#include <cstdio>

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++g_fail; } } while (0)
using namespace hsm;

int main() {
    LifeBook b;
    CHECK(b.Current(2) == 0);
    CHECK(b.Bump(2) == 1 && b.Bump(2) == 2 && b.Current(2) == 2 && b.Current(3) == 0);   // per entity
    for (int i = 0; i < 252; ++i) b.Bump(2);
    CHECK(b.Current(2) == 254);
    CHECK(b.Bump(2) == 255);
    CHECK(b.Bump(2) == 1);                                                              // wraps past 0, never 0
    for (int i = 0; i < 1000; ++i) CHECK(b.Bump(5) != 0);
    CHECK(b.Bump(0) == 0 && b.Bump(uint8_t(kMaxWillies + 1)) == 0 && b.Current(200) == 0);   // invalid entities
    b.Clear(); CHECK(b.Current(2) == 0 && b.Current(5) == 0 && b.Bump(2) == 1);

    CHECK(LifeCurrent(3, 3));
    CHECK(!LifeCurrent(2, 3));                  // a claim against the previous body
    CHECK(LifeCurrent(0, 3));                   // the attacker never saw a Spawn: unknown, accepted
    CHECK(LifeCurrent(0, 0) && LifeCurrent(7, 0));   // the host has no life yet: accepted
    // the late death report: body 1 died, respawned as body 2, the stale report for body 1 arrives
    LifeBook h; uint8_t l1 = h.Bump(4); h.Bump(4);
    CHECK(!LifeCurrent(l1, h.Current(4)));
    CHECK(LifeCurrent(h.Current(4), h.Current(4)));

    if (g_fail) { std::printf("%d failures\n", g_fail); return 1; }
    std::puts("ok");
    return 0;
}
