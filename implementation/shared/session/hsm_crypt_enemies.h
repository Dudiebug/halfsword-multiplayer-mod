// The host's director enemies in the shared Crypts (docs/PLAN_CRYPTS_COOP.md, PR C). Pure rules for two things the game layer needs:
//   - EnemyVerdictOf: what the host does with a fighter it finds that no player or bot owns. The game's director spawns the
//     Crypts' enemies on the host alone; each living one is adopted as a host bot (the Players vs Bots path: a Spawn with its
//     passport, the pose stream, hit claims, kills), so every client shows it as a stand-in. One that is already dead is left
//     where it lies (it may be the level's own decor; nothing is synced for it), and one that finds no free bot entity waits.
//   - PruneList: the director keeps its live fighters in "Spawned Willies" and spawns only while that list holds 3 or 4 of
//     them. The game empties it by destroying fighters (Destroy Single Willie, Destroy Actors in Area); in the shared run
//     the second is blocked and the mod removes a bot itself, so the mod takes a removed or dead enemy out of the list, or
//     the cap would stay full and no enemy would spawn again.
// Game-independent and unit-tested (session/test_hsm_crypt_enemies.cpp).
#pragma once
#include <cstdint>

namespace hsm::crypt {

enum class EnemyVerdict : uint8_t { Adopt = 0, Wait = 1, Leave = 2 };

// `dead`: the fighter is dead already. `entityFree`: a bot entity is free for it.
inline EnemyVerdict EnemyVerdictOf(bool dead, bool entityFree) {
    if (dead) return EnemyVerdict::Leave;
    return entityFree ? EnemyVerdict::Adopt : EnemyVerdict::Wait;
}
inline const char* EnemyVerdictText(EnemyVerdict v) {
    switch (v) {
        case EnemyVerdict::Adopt: return "adopted";
        case EnemyVerdict::Wait: return "waiting for a free bot entity";
        case EnemyVerdict::Leave: return "already dead; left alone";
    }
    return "?";
}

// Takes out of data[0..num) every entry for which `drop(entry)` is true, keeping the order of the rest; num shrinks by that
// many and the count is returned. The entries after the new end are left as they were (a game array's Num is what counts).
template <class T, class F> int PruneList(T* data, int32_t& num, F drop) {
    if (!data || num <= 0) return 0;
    int32_t kept = 0;
    for (int32_t i = 0; i < num; ++i) {
        if (drop(data[i])) continue;
        if (kept != i) data[kept] = data[i];
        ++kept;
    }
    const int removed = int(num - kept);
    num = kept;
    return removed;
}

}  // namespace hsm::crypt
