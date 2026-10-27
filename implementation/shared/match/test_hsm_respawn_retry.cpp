// Own-respawn retry policy: first attempt at once, then every second, five attempts, give up; wrap-safe.
#include "match/hsm_respawn_retry.h"
#include <cstdio>

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++g_fail; } } while (0)
using namespace hsm;

static void TestSchedule() {
    RespawnRetry r;
    CHECK(!r.Active() && !r.Due(0));
    r.Begin();
    CHECK(r.Due(5000));                                    // the first attempt is immediate
    CHECK(r.Failed(5000));
    CHECK(!r.Due(5999) && r.Due(6000));
    for (int i = 2; i <= 4; ++i) { CHECK(r.Failed(5000 + 1000u * unsigned(i - 1))); }
    CHECK(r.attempts() == 4 && r.Active());
    CHECK(!r.Failed(9000));                                // the fifth failure is the last
    CHECK(!r.Active() && !r.Due(100000) && r.attempts() == 5);
}

static void TestBeginIdempotent() {
    RespawnRetry r;
    r.Begin(); r.Failed(10);
    r.Begin();                                             // does not restart the count or make it due early
    CHECK(r.attempts() == 1 && !r.Due(500));
    r.Reset();
    CHECK(!r.Active());
    r.Begin(); CHECK(r.Due(0) && r.attempts() == 0);
}

static void TestWrap() {
    RespawnRetry r;
    r.Begin(); r.Failed(0xFFFFFF00u);                      // next attempt falls after the wrap
    CHECK(!r.Due(0xFFFFFFFFu) && !r.Due(0x2E7u) && r.Due(0x2E8u));
}

int main() {
    TestSchedule(); TestBeginIdempotent(); TestWrap();
    if (g_fail) { std::printf("%d failure(s)\n", g_fail); return 1; }
    std::puts("ok");
    return 0;
}
