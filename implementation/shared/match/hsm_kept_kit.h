// The Loadouts row (#2112): a Tournament with random loadouts either rolls a new loadout for every fight (the default) or
// keeps each player's first one for the whole bracket. The host keeps the rolls (KeptKits) and gives the kept one again
// at every fight; a new tournament (the match start, a rematch) rolls new ones. Never a wire field: the loadouts go out
// as before.
#pragma once
#include "match/hsm_match.h"
#include <cstdint>
#include <map>
#include <vector>

namespace hsm {

// The row is in force: a Tournament whose gear is a random loadout each (the Mirror roll is shared by both fighters).
inline bool KeepsKit(const MatchConfig& c) { return c.format == Format::Tournament && c.gear == Gear::RandomTier && c.keepKit; }

class KeptKits {
public:
    // The loadout for `entity`: the one kept for it, else roll() (kept from now on). An empty roll is not kept.
    template <class Roll> std::vector<uint8_t> For(uint8_t entity, Roll&& roll, bool& reused) {
        if (auto it = kits_.find(entity); it != kits_.end()) { reused = true; return it->second; }
        reused = false;
        std::vector<uint8_t> blob = roll();
        if (!blob.empty()) kits_[entity] = blob;
        return blob;
    }
    void Clear() { kits_.clear(); }
    size_t size() const { return kits_.size(); }
private:
    std::map<uint8_t, std::vector<uint8_t>> kits_;
};

}  // namespace hsm
