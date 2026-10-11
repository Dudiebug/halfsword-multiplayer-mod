// Spectating while dead: which living fighter the camera follows, and how that choice moves as fighters die or
// leave. Game-independent and unit-tested; the client's gameplay layer points the camera at the result.
#pragma once
#include "match/hsm_match.h"   // kNoTeam
#include <algorithm>
#include <cstdint>
#include <optional>
#include <utility>
#include <vector>

namespace hsm {

class SpectateCycler {
public:
    struct Candidate { uint8_t entity = 0; int8_t team = kNoTeam; bool alive = true; };

    // Rebuilds the cycle from the current roster. The spectator (`self`) is never a candidate, and neither are dead
    // fighters. Living teammates come first, then every other living fighter; each group is ordered by entity id.
    // Teammates are only teammates when both sides have a team, so with no team everyone else is in the second group.
    // The target stays while it lives. When it dies or leaves the roster the camera moves to the entry that followed
    // it in the cycle, wrapping to the first; with nobody left alive the target is cleared.
    void Refresh(uint8_t self, int8_t selfTeam, const std::vector<Candidate>& roster);

    std::optional<uint8_t> Target() const;
    std::optional<uint8_t> Next();   // forward through the cycle, wrapping
    std::optional<uint8_t> Prev();   // backward through the cycle, wrapping

private:
    struct Entry { int group = 1; uint8_t entity = 0; };   // group 0: living teammate, 1: other living fighter
    static bool Before(const Entry& a, const Entry& b) { return a.group != b.group ? a.group < b.group : a.entity < b.entity; }
    std::optional<uint8_t> Step(bool forward);

    std::vector<Entry> order_;                     // sorted by Before, one entry per entity
    std::optional<Entry> target_;                  // always an element of order_ when order_ is not empty
};

inline void SpectateCycler::Refresh(uint8_t self, int8_t selfTeam, const std::vector<Candidate>& roster) {
    std::vector<Entry> live;
    for (const Candidate& c : roster) {
        if (!c.alive || c.entity == self) continue;
        bool mate = selfTeam != kNoTeam && c.team == selfTeam;
        live.push_back({mate ? 0 : 1, c.entity});
    }
    // One entry per entity: a duplicate keeps the lower group (the teammate reading) so the choice does not depend on order.
    std::sort(live.begin(), live.end(), [](const Entry& a, const Entry& b) {
        return a.entity != b.entity ? a.entity < b.entity : a.group < b.group;
    });
    live.erase(std::unique(live.begin(), live.end(), [](const Entry& a, const Entry& b) { return a.entity == b.entity; }), live.end());
    std::sort(live.begin(), live.end(), Before);
    order_ = std::move(live);

    if (order_.empty()) { target_.reset(); return; }
    if (target_) {
        auto same = std::find_if(order_.begin(), order_.end(), [&](const Entry& e) { return e.entity == target_->entity; });
        if (same != order_.end()) { target_ = *same; return; }   // still here: keep following it, whatever moved
        // Gone: the first entry after where it sat, wrapping to the front.
        auto after = std::upper_bound(order_.begin(), order_.end(), *target_, Before);
        target_ = after != order_.end() ? *after : order_.front();
        return;
    }
    target_ = order_.front();
}

inline std::optional<uint8_t> SpectateCycler::Target() const {
    if (!target_) return std::nullopt;
    return target_->entity;
}

inline std::optional<uint8_t> SpectateCycler::Step(bool forward) {
    if (order_.empty()) { target_.reset(); return std::nullopt; }
    size_t n = order_.size(), at = 0;
    if (target_) {
        for (size_t i = 0; i < n; ++i) if (order_[i].entity == target_->entity) { at = i; break; }
    }
    at = forward ? (at + 1) % n : (at + n - 1) % n;
    target_ = order_[at];
    return target_->entity;
}

inline std::optional<uint8_t> SpectateCycler::Next() { return Step(true); }
inline std::optional<uint8_t> SpectateCycler::Prev() { return Step(false); }

}  // namespace hsm
