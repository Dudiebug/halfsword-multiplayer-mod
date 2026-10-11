// Session roster: who occupies which slot/entity. Game-independent, unit-tested.
// Slot 0 is the host; slots 1..5 are clients in join order; freed slots are reused (lowest first).
// Entity ids are slot + 1 for fighters, so they stay stable for a member's whole session.
#pragma once
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace hsm {

constexpr int kMaxFighters = 6;

struct RosterMember {
    uint64_t key = 0;      // stable identity (SteamID, or transport peer id for direct-IP)
    uint8_t slot = 0;
    uint8_t entity = 0;
    std::string name;
};

class Roster {
public:
    static uint8_t EntityForSlot(uint8_t slot) { return uint8_t(slot + 1); }

    // Adds a member or returns the existing slot for a known key. Fails when all slots are taken.
    std::optional<uint8_t> Add(uint64_t key, const std::string& name) {
        if (const RosterMember* m = Find(key)) return m->slot;
        for (uint8_t slot = 0; slot < kMaxFighters; ++slot) {
            if (BySlot(slot)) continue;
            members_.push_back({key, slot, EntityForSlot(slot), name});
            return slot;
        }
        return std::nullopt;
    }

    bool Remove(uint64_t key) {
        for (size_t i = 0; i < members_.size(); ++i)
            if (members_[i].key == key) { members_.erase(members_.begin() + long(i)); return true; }
        return false;
    }

    const RosterMember* Find(uint64_t key) const {
        for (const RosterMember& m : members_) if (m.key == key) return &m;
        return nullptr;
    }
    const RosterMember* BySlot(uint8_t slot) const {
        for (const RosterMember& m : members_) if (m.slot == slot) return &m;
        return nullptr;
    }
    const RosterMember* ByEntity(uint8_t entity) const {
        for (const RosterMember& m : members_) if (m.entity == entity) return &m;
        return nullptr;
    }
    const std::vector<RosterMember>& members() const { return members_; }
    size_t size() const { return members_.size(); }
    void Clear() { members_.clear(); }

private:
    std::vector<RosterMember> members_;
};

}  // namespace hsm
