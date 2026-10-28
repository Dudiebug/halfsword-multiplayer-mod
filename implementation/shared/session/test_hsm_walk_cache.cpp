// The level walk's physics-flag cache (session/hsm_walk_cache.h).
#include "session/hsm_walk_cache.h"
#include <cstdio>

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++g_fail; } } while (0)
using namespace hsm;

static void TestTtl() {
    CHECK(SimTtlMs(true, true) == kSimKnownAsleepMs && SimTtlMs(true, false) == kSimKnownAsleepMs);
    CHECK(SimTtlMs(false, true) == kSimYesMs && SimTtlMs(false, false) == kSimNoMs);
    CHECK(kSimYesMs < kSimNoMs && kSimNoMs < kSimKnownAsleepMs);
}

static void TestLookup() {
    SimCache c;
    bool sim = true;
    CHECK(!c.Lookup(1, 10, false, 0.0, sim));                                // empty
    c.Store(1, 10, true, 1000.0);
    CHECK(c.Lookup(1, 10, false, 1000.0, sim) && sim);
    CHECK(c.Lookup(1, 10, false, 1000.0 + kSimYesMs - 1.0, sim) && sim);
    CHECK(!c.Lookup(1, 10, false, 1000.0 + kSimYesMs, sim));                  // a "yes" is re-asked soon
    CHECK(c.Lookup(1, 10, true, 1000.0 + kSimYesMs, sim) && sim);             // but kept longer for a known sleeper
    CHECK(!c.Lookup(1, 10, true, 1000.0 + kSimKnownAsleepMs, sim));
    c.Store(2, 20, false, 0.0);
    CHECK(c.Lookup(2, 20, false, kSimNoMs - 1.0, sim) && !sim);
    CHECK(!c.Lookup(2, 20, false, kSimNoMs, sim));
    // A reused pointer is another object: the index differs, so the entry does not answer.
    CHECK(!c.Lookup(1, 11, false, 1000.0, sim));
    bool st = false;
    CHECK(c.Stale(1, 10, st) && st && !c.Stale(1, 11, st) && !c.Stale(9, 10, st));
    CHECK(c.Stale(2, 20, st) && !st);                                         // stale answers regardless of age
    // A new answer replaces the old.
    c.Store(1, 10, false, 5000.0);
    CHECK(c.Lookup(1, 10, false, 5001.0, sim) && !sim && c.Size() == 2);
    c.Store(1, 12, true, 5002.0);
    CHECK(!c.Lookup(1, 10, false, 5003.0, sim) && c.Lookup(1, 12, false, 5003.0, sim) && sim);
}

static void TestPruneAndBound() {
    SimCache c;
    c.Store(1, 1, true, 0.0); c.Store(2, 2, false, 50000.0);
    CHECK(c.Prune(kSimKeepMs - 1.0) == 0 && c.Size() == 2);
    CHECK(c.Prune(kSimKeepMs) == 1 && c.Size() == 1);
    bool st = false;
    CHECK(!c.Stale(1, 1, st) && c.Stale(2, 2, st));
    c.Clear(); CHECK(c.Size() == 0);
    for (size_t i = 0; i < kMaxSimCacheEntries; ++i) c.Store(i + 1, int32_t(i), true, 0.0);
    CHECK(c.Size() == kMaxSimCacheEntries);
    c.Store(kMaxSimCacheEntries + 100, 1, true, 0.0);
    CHECK(c.Size() == kMaxSimCacheEntries && !c.Stale(kMaxSimCacheEntries + 100, 1, st));   // full: not stored
    c.Store(5, 4, false, 9.0);                                                // a known key still updates
    CHECK(c.Stale(5, 4, st) && !st);
}

int main() {
    TestTtl(); TestLookup(); TestPruneAndBound();
    if (g_fail) { std::printf("test_hsm_walk_cache: %d FAILED\n", g_fail); return 1; }
    std::printf("test_hsm_walk_cache: all passed\n");
    return 0;
}
