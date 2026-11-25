// Class hash -> readable class name, for log lines. Sync messages carry only sync::WeaponClassHash of a class name, so a
// line that prints the hash alone ("class 3a9f01c2") tells nobody what the object is. Each machine remembers the names of
// the classes it has seen locally (a level actor, a loaded weapon class); a hash it has never seen reads "unknown class".
// No protocol change: the name is only ever filled from this machine's own knowledge.
#pragma once
#include "hsm_pose_codec.h"
#include <cstdint>
#include <cstdio>
#include <string>
#include <unordered_map>

namespace hsm::sync {

// The short form of a class name or path: "/Game/Props/BP_Barrel.BP_Barrel_C" -> "BP_Barrel_C",
// "Class /Script/Engine.Actor" -> "Actor".
inline std::string ShortClassName(const std::string& full) {
    size_t cut = full.find_last_of("./: ");
    return cut == std::string::npos ? full : full.substr(cut + 1);
}

class ClassNames {
public:
    // Remember a class by its full name (hash computed here). Returns the hash; 0 for an empty name.
    uint32_t Note(const std::string& fullName) {
        if (fullName.empty()) return 0;
        const uint32_t h = WeaponClassHash(fullName.c_str());
        if (h != 0 && !names_.count(h) && names_.size() < kMaxNames) names_.emplace(h, ShortClassName(fullName));
        return h;
    }
    bool Known(uint32_t hash) const { return names_.count(hash) != 0; }
    // "BP_Barrel_C (3a9f01c2)", or "unknown class (3a9f01c2)".
    std::string Describe(uint32_t hash) const {
        char b[16];
        std::snprintf(b, sizeof b, "%08x", unsigned(hash));
        auto it = names_.find(hash);
        return (it == names_.end() ? std::string("unknown class") : it->second) + " (" + b + ")";
    }
    size_t Size() const { return names_.size(); }

private:
    static constexpr size_t kMaxNames = 8192;
    std::unordered_map<uint32_t, std::string> names_;
};

}  // namespace hsm::sync
