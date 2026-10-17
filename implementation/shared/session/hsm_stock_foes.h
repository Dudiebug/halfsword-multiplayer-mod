// Stock-spawn policy, independent of Unreal reflection and session timing.
#pragma once
namespace hsm {
inline bool AllowStockFoes(bool host, bool botFormat, bool progression) {
    return host && (botFormat || progression); // clients render announced bots; they never spawn their own foes
}
inline bool BlockStockSpawn(bool sessionMatch, bool allowFoes, bool playerFlagKnown, bool spawnPlayer) {
    return sessionMatch && !allowFoes && !(playerFlagKnown && spawnPlayer);
}
inline bool RemoveStockFighter(bool sessionMatch, bool allowFoes, bool localPlayer, bool sessionEntity) {
    return sessionMatch && !allowFoes && !localPlayer && !sessionEntity;
}
} // namespace hsm
