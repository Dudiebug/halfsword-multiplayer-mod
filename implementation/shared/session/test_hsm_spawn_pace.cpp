// Fighter spawns kept apart on one machine (session/hsm_spawn_pace.h).
#include "session/hsm_spawn_pace.h"
#include <cstdio>

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++g_fail; } } while (0)
using namespace hsm;

int main() {
    // Crash family J: the own respawn at 1000 ms, then a stand-in each frame (1016, 1033) was the burst. With the gap
    // each stand-in waits kSpawnGapMsDefault after the spawn before it.
    {
        SpawnPace p;
        CHECK(p.gap() == kSpawnGapMsDefault);
        CHECK(p.Ready(1000));                                   // nothing spawned yet
        p.Note(1000);                                           // the own respawn
        CHECK(!p.Ready(1016));
        CHECK(!p.Ready(1000 + kSpawnGapMsDefault - 1));
        CHECK(p.Ready(1000 + kSpawnGapMsDefault));
        p.Note(1000 + kSpawnGapMsDefault);                      // the first stand-in
        CHECK(!p.Ready(1000 + kSpawnGapMsDefault + 16));
        CHECK(p.Ready(1000 + 2 * kSpawnGapMsDefault));
    }
    // spawn_gap_ms=0: as in 0.8.2-beta1 (one a frame, nothing held).
    {
        SpawnPace p;
        p.SetGap(0);
        p.Note(500);
        CHECK(p.Ready(500));
        CHECK(p.Ready(516));
    }
    // A gap above the cap counts as the cap; a small one as itself.
    {
        SpawnPace p;
        p.SetGap(kSpawnGapMsMax + 1000);
        CHECK(p.gap() == kSpawnGapMsMax);
        p.Note(0);
        CHECK(!p.Ready(kSpawnGapMsMax - 1));
        CHECK(p.Ready(kSpawnGapMsMax));
        p.SetGap(kSpawnGapMsMax);
        CHECK(p.gap() == kSpawnGapMsMax);
        p.SetGap(30);
        CHECK(p.gap() == 30);
        CHECK(!p.Ready(29));
        CHECK(p.Ready(30));
    }
    // Before any spawn nothing is held, at any clock (the low 32 bits of the tick count start anywhere).
    {
        SpawnPace p;
        CHECK(p.Ready(0));
        CHECK(p.Ready(5));
    }
    // The default covers the J faults' p90 (about 100 ms after a spawn).
    CHECK(kSpawnGapMsDefault >= 100);
    // A now read before a later Note in the same tick (a rematch's RespawnAll, then the queue) is not ready.
    {
        SpawnPace p;
        p.Note(1000);
        CHECK(!p.Ready(990));
        CHECK(!p.Ready(999));
        CHECK(p.Ready(1000 + kSpawnGapMsDefault));
    }
    // The steady clock's millisecond count wraps: the gap still holds across it.
    {
        SpawnPace p;
        p.Note(0xFFFFFFF0u);
        CHECK(!p.Ready(0x00000010u));                           // 32 ms later
        CHECK(p.Ready(uint32_t(0xFFFFFFF0u + kSpawnGapMsDefault)));
    }

    if (g_fail) { std::printf("%d spawn pace check(s) failed\n", g_fail); return 1; }
    std::printf("spawn pace tests passed\n");
    return 0;
}
