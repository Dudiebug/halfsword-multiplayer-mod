// Sync audit item 43. A chest's BeginPlay turns off the collision between its Box and the limbs of GetPlayerCharacter(0)
// only (BP_Container_Chest_001.cpp:87, 002-005 :58), so a stand-in reaching into a chest snags on it and shoves it on
// every other machine. The same call, for every stand-in and every chest, once per pair. Game thread only.
#pragma once
#include "ue/engine.h"
#include <set>
#include <tuple>
#include <utility>
#include <vector>

namespace hsm::game {

class ChestLids {
public:
    // Every tick: the Willies that are no player 0 here (stand-ins). Looks for chests every few seconds.
    void Tick(double nowMs, const std::vector<ue::UObject*>& fighters);
    void Clear();   // level change: the chests and meshes are gone
private:
    std::vector<std::pair<ue::UObject*, int32_t>> chests_;   // each Box, with its object index
    std::set<std::tuple<ue::UObject*, ue::UObject*, int32_t>> done_;   // (Box, Mesh, its object index)
    ue::UObject* lib_ = nullptr;
    double nextScanMs_ = 0, nextPairMs_ = 0;
    uint64_t disabled_ = 0;
};

}  // namespace hsm::game
