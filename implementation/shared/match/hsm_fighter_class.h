// The actor class a fighter of a format is spawned as (the look half of the stand-in's shape, #561, #298). The Zombies
// format's bots are the game's zombie Willie (Willie_BP_Zombie_C, a child of Willie_BP_C with the rotten look baked
// into its component templates and no new variables), so a stand-in spawned as it is pose-sync-safe; everything else
// is the stock class. Infection's infected players are zombies too (pre24 tester feedback): their next body is one.
// The host decides and says so in Spawn.flags kSpawnZombie (rev 38), so a client never guesses from a MatchInfo that
// may not have arrived yet (Abyss clients saw human bots).
#pragma once

namespace hsm {

inline const char* FighterClassFor(bool zombiesFormat, bool botTeam, bool infectedPlayer = false) {
    return ((zombiesFormat && botTeam) || infectedPlayer) ? "Willie_BP_Zombie_C" : "Willie_BP_C";
}
inline bool IsZombieClass(const char* cls) {
    const char* z = "Willie_BP_Zombie_C";
    for (; *cls && *cls == *z; ++cls, ++z) {}
    return *cls == 0 && *z == 0;
}

}  // namespace hsm
