// Owner authority (protocol rev 9): who simulates which entity, who may stream it, and where the host routes what an
// owner sends. Fighter entity e (1..kMaxFighters, 9) is owned by the machine holding roster slot e-1 (slot 0 = the
// host); every other entity (bots, adopted stock NPCs) is owned by the host. A machine only ever streams what it
// owns, and the host drops anything else, so one player can never move or damage through another's fighter.
#pragma once
#include "hsm_protocol.h"
#include "session/hsm_roster.h"
#include <array>
#include <cstdint>
#include <vector>

namespace hsm {

constexpr uint8_t kHostSlot = 0;

enum class SyncMode : uint8_t {
    Host = 0,    // host physics (0.9.0, rev 29): the host simulates every fighter from its player's inputs; every client shows
                 // the host's stream for every fighter, its own included (rev 8 had this with a soft copy)
    Owner = 1,   // every machine simulates its own fighter and streams it; others are stand-ins
};

inline bool ValidEntity(uint8_t entity) { return entity >= 1 && entity <= kMaxWillies; }

// Hand-offs (sync/hsm_handoff.h): the slot whose machine simulates each entity for now, kNoSlot where its owner does.
constexpr uint8_t kNoSlot = 0xFF;
struct Holders {
    std::array<uint8_t, kMaxWillies + 1> slot;
    Holders() { slot.fill(kNoSlot); }
    uint8_t Of(uint8_t entity) const { return entity <= kMaxWillies ? slot[entity] : kNoSlot; }
};

// The roster slot whose machine simulates entity (kHostSlot for bots, the holder while it is handed off); 0xFF for an
// invalid entity.
inline uint8_t OwnerSlotOf(uint8_t entity, const Holders& h = Holders()) {
    if (!ValidEntity(entity)) return 0xFF;
    if (h.Of(entity) < kMaxFighters) return h.Of(entity);
    return entity <= kMaxFighters ? uint8_t(entity - 1) : kHostSlot;
}

// May the machine in senderSlot stream (Pose, Vitals) or claim hits with (HitClaim attacker) this entity?
inline bool MayStream(uint8_t senderSlot, uint8_t entity, const Holders& h = Holders()) {
    return senderSlot < kMaxFighters && OwnerSlotOf(entity, h) == senderSlot;
}

// Where the host sends a Pose or Vitals that the machine in fromSlot streamed: every other connected slot.
inline std::vector<uint8_t> RelayTargets(uint8_t fromSlot, const std::vector<uint8_t>& connectedSlots) {
    std::vector<uint8_t> out;
    for (uint8_t s : connectedSlots)
        if (s != fromSlot && s != kHostSlot) out.push_back(s);
    return out;
}

static_assert(size_t(kMaxFighters) == kMaxPeerMap, "PeerMap carries one entry per fighter slot");

// Body lives (rev 19). The host counts every body it spawns for an entity (Spawn.life, 1..255, wrapping past 0);
// a claim or death report names the life it was made against, and one that names an older body is dropped, so a late
// report can never kill the next body. 0 is "unknown" on either side (a missing Spawn must not block play) and passes.
class LifeBook {
public:
    uint8_t Bump(uint8_t entity) {
        if (!ValidEntity(entity)) return 0;
        uint8_t& v = life_[entity];
        v = (v == 255) ? uint8_t(1) : uint8_t(v + 1);
        return v;
    }
    uint8_t Current(uint8_t entity) const { return ValidEntity(entity) ? life_[entity] : uint8_t(0); }
    void Clear() { for (uint8_t& v : life_) v = 0; }
private:
    uint8_t life_[kMaxWillies + 1] = {};
};
inline bool LifeCurrent(uint8_t claimed, uint8_t current) { return current == 0 || claimed == 0 || claimed == current; }

// A HitClaim the host received from senderSlot (or made itself, senderSlot = kHostSlot).
struct ClaimRoute {
    bool accept = false;
    uint8_t replaySlot = 0xFF;            // the slot that replays it on its own fighter (kHostSlot: the host itself)
    std::vector<uint8_t> fxSlots;         // connected slots that get it as HitFx (cosmetic), host excluded
    bool hostFx = false;                  // the host shows it cosmetically (it is neither attacker nor victim owner)
};
inline ClaimRoute RouteClaim(uint8_t senderSlot, uint8_t attacker, uint8_t victim, const std::vector<uint8_t>& connectedSlots,
                             const Holders& h = Holders()) {
    ClaimRoute r;
    if (attacker == victim || !ValidEntity(victim) || !MayStream(senderSlot, attacker, h)) return r;
    const uint8_t victimSlot = OwnerSlotOf(victim, h);
    bool victimHere = victimSlot == kHostSlot;
    for (uint8_t s : connectedSlots) victimHere = victimHere || s == victimSlot;
    if (!victimHere) return r;            // nobody owns that fighter any more
    r.accept = true;
    r.replaySlot = victimSlot;
    for (uint8_t s : connectedSlots)
        if (s != kHostSlot && s != senderSlot && s != victimSlot) r.fxSlots.push_back(s);
    r.hostFx = senderSlot != kHostSlot && victimSlot != kHostSlot;
    return r;
}

}  // namespace hsm
