// Weapons drawn and sheathed in owner sync (protocol rev 10, MsgType::Equip).
//
// A stand-in is spawned from its owner's loadout, so it starts with the same weapons in hand and in the same sheath
// slots (Weapon Slot R 1, R 2, Back, L 1, L 2; SheathSlots_Enum 0..4). Its tick is blocked, so it never draws or
// sheathes by itself: when the owner draws a sheathed weapon, the stand-in's copy stayed on its belt and its hand stayed
// empty, so the drawn weapon was invisible on the other screen and a later drop of it found nothing in the stand-in's
// hand (prop_sync).
//
// The owner reports, per fighter, what each hand holds in terms of those slots, and which slots hold a sheathed weapon.
// In Willie_BP a sheathed weapon is in its slot variable with that slot's sheath constraint (Sheathe on Spawn,
// Willie_BP.cpp:6994; Create_Sheath_Constraint_R, 2874). A draw grabs it while still sheathed (the hand and the slot
// hold the same actor), then pulling it out breaks the constraint and nulls the slot variable (Check Scabbard Unsheath
// State R, 2324). Which slot a drawn weapon came from is therefore remembered by EquipTracker, which reads a fighter
// every poll:
//   hand token  k+1 when the hand's actor was last seen sheathed in slot k (or still sits in slot k's variable without
//               its constraint) and is sheathed nowhere now; 0 (other) for anything else: empty, fists, a weapon it
//               started with in hand, a pickup (a prop, streamed by prop_sync), a weapon mid-draw or mid-sheathe;
//   sheathed    bit k when slot k holds an actor with its constraint.
// The stand-in's own state is read the same way (its own tracker), and PlanEquip lists what it must do to match: put
// back (Sheathe) a weapon the owner sheathed, then draw (Draw) what the owner drew. Pure types, no engine headers.
//
// Wire layout, in order: header(Equip) entity:u8 seq:u16 handR:u8 handL:u8 sheathed:u8.
#pragma once
#include "hsm_protocol.h"
#include <array>
#include <cstdint>
#include <map>
#include <vector>

namespace hsm::combat {

constexpr int kEquipSlots = 5;              // R 1, R 2, Back, L 1, L 2
constexpr uint8_t kEquipOther = 0;          // hand token: not drawn from a slot
constexpr double kEquipResendMs = 2000;     // the state is sent again this often (a late stand-in catches up)

struct EquipStateMsg {
    uint8_t entity = 0;
    uint16_t seq = 0;                       // per sender, for logs
    std::array<uint8_t, 2> hand{};          // 0 right, 1 left: kEquipOther or slot + 1
    uint8_t sheathed = 0;                   // bit k: slot k holds a sheathed weapon
};

inline std::vector<uint8_t> Encode(const EquipStateMsg& m) {
    Writer w;
    w.header(MsgType::Equip);
    w.u8(m.entity);
    w.u16(m.seq);
    w.u8(m.hand[0]);
    w.u8(m.hand[1]);
    w.u8(m.sheathed);
    return w.buf;
}

inline bool Decode(const uint8_t* d, size_t n, EquipStateMsg& m) {
    if (n > kMaxPacket) return false;
    Reader r(d, n);
    MsgType t;
    if (!r.header(t) || t != MsgType::Equip) return false;
    EquipStateMsg o;
    o.entity = r.u8();
    o.seq = r.u16();
    o.hand[0] = r.u8();
    o.hand[1] = r.u8();
    o.sheathed = r.u8();
    if (!r.done()) return false;
    if (o.entity < 1 || o.entity > kMaxWillies) return false;
    if (o.sheathed >= (1u << kEquipSlots)) return false;
    for (uint8_t h : o.hand) {
        if (h > kEquipSlots) return false;
        if (h != kEquipOther && (o.sheathed & (1u << (h - 1)))) return false;   // drawn from a slot it is sheathed in
    }
    if (o.hand[0] != kEquipOther && o.hand[0] == o.hand[1]) return false;
    m = o;
    return true;
}

// What a fighter carries, as actor identities: its two hands ("Weapon R", "Weapon L"), its five slot variables and
// whether each slot has its sheath constraint.
struct EquipSnapshot {
    std::array<const void*, 2> hands{};
    std::array<const void*, kEquipSlots> slots{};
    std::array<bool, kEquipSlots> constrained{};
};

// One fighter's reader. Read gives the snapshot in tokens (entity and seq left 0) and remembers, per actor, the slot it
// was last seen sheathed in:
//   - an actor sheathed in two slots counts as the lower one;
//   - a slot that now holds another sheathed actor is no one else's origin any more;
//   - an actor in neither hand nor any slot variable is forgotten (a dropped weapon picked up again is "other");
//   - a weapon in both hands counts for the right hand only, and no two hands get the same token.
class EquipTracker {
public:
    EquipStateMsg Read(const EquipSnapshot& s) {
        EquipStateMsg m;
        for (int k = kEquipSlots - 1; k >= 0; --k)   // downwards: the lower slot wins
            if (s.slots[k] && s.constrained[k]) {
                m.sheathed = uint8_t(m.sheathed | (1u << k));
                origin_[s.slots[k]] = k;
            }
        for (auto it = origin_.begin(); it != origin_.end();) {
            const bool refilled = (m.sheathed & (1u << it->second)) && s.slots[it->second] != it->first;
            it = refilled || !Carried(s, it->first) ? origin_.erase(it) : std::next(it);
        }
        for (int h = 0; h < 2; ++h) {
            const void* a = s.hands[h];
            if (!a || SheathedNow(s, a)) continue;
            if (auto it = origin_.find(a); it != origin_.end()) { m.hand[h] = uint8_t(it->second + 1); continue; }
            for (int k = 0; k < kEquipSlots; ++k)
                if (s.slots[k] == a) { m.hand[h] = uint8_t(k + 1); break; }   // its slot variable kept, constraint gone
        }
        if (m.hand[1] != kEquipOther && m.hand[1] == m.hand[0]) m.hand[1] = kEquipOther;   // also one weapon in both hands
        return m;
    }
    void Clear() { origin_.clear(); }

private:
    static bool SheathedNow(const EquipSnapshot& s, const void* a) {
        for (int k = 0; k < kEquipSlots; ++k) if (s.slots[k] == a && s.constrained[k]) return true;
        return false;
    }
    static bool Carried(const EquipSnapshot& s, const void* a) {
        if (a == s.hands[0] || a == s.hands[1]) return true;
        for (const void* slot : s.slots) if (slot == a) return true;
        return false;
    }
    std::map<const void*, int> origin_;   // at most 7 entries after each Read (2 hands, 5 slots)
};

// What the stand-in's hands show that the owner's do not, read from the owner's pose rather than its Equip state:
// staleHeld[h] when the stand-in's hand h holds a real weapon (not fists; for the left, not the right's weapon again)
// and the owner's newest pose shows that hand empty or holding another class. A weapon the owner never drew from a slot
// (one it started with in hand, a pickup) has token other on both sides, so the Equip state alone cannot tell that the
// owner put it away.
struct EquipHints {
    std::array<bool, 2> staleHeld{};
};

// Whether hand h (0 right, 1 left) can sheathe into slot k: Create Sheath Constraint R takes R 1, R 2 and Back, L takes
// L 1, L 2 and Back (Weapon_Slots_Enum, Willie_BP.cpp:2874).
constexpr bool HandReachesSlot(int hand, int slot) {
    if (slot < 0 || slot >= kEquipSlots) return false;
    if (hand == 0) return slot <= 2;
    if (hand == 1) return slot >= 2;
    return false;
}

// The owner's pose shows a weapon of class cls in an empty hand of the stand-in while its Equip token for that hand is
// other: it may be a draw whose state is still on its way (mid-draw the owner's hand and slot hold the same actor) when a
// slot the owner shows sheathed holds a weapon of that class on the stand-in (slotClass[k], 0 for an empty slot). Any
// slot, whichever hand: a hand draws from every slot (PlanEquip's Draw has no reach; HandReachesSlot is for sheathing).
// Test 5: the right hand drew from L 1, a reach rule took that for a pickup, and the hand went for a loose weapon.
inline bool MayBeDrawFrom(uint8_t ownerSheathed, const std::array<uint32_t, kEquipSlots>& slotClass, uint32_t cls) {
    if (cls == 0) return false;
    for (int k = 0; k < kEquipSlots; ++k)
        if ((ownerSheathed & (1u << k)) && slotClass[size_t(k)] == cls) return true;
    return false;
}

struct EquipStep {
    enum class Kind : uint8_t { Sheathe, Draw };
    Kind kind = Kind::Draw;
    int hand = 0;
    int slot = 0;
};

// What the stand-in (its own state standIn, hasSlot: which of its slots hold an actor) does to match owner: every
// Sheathe first, then every Draw.
//   Sheathe(h, k)  its hand h holds slot k's weapon and the owner has slot k sheathed (a drawn slot never is: Decode);
//   Sheathe(h, k)  also for a stale hand h (EquipHints) with token other whose weapon has exactly one place to go: k is
//                  the only slot hand h reaches that the owner has sheathed, the stand-in has empty and neither of its
//                  hands drew from, and no other stale hand has k as a candidate (otherwise nothing: which weapon went
//                  where is not known). These come after the plain Sheathes;
//   Draw(h, k)     the owner's hand h holds slot k's weapon, the stand-in's does not, the stand-in has an actor in slot
//                  k, and that actor is not in its other hand once the Sheathes are done.
inline std::vector<EquipStep> PlanEquip(const EquipStateMsg& owner, const EquipStateMsg& standIn,
                                        const std::array<bool, kEquipSlots>& hasSlot,
                                        const EquipHints& hints = {}) {
    std::vector<EquipStep> steps;
    for (int h = 0; h < 2; ++h) {
        const uint8_t held = standIn.hand[h];
        if (held != kEquipOther && (owner.sheathed & (1u << (held - 1))))   // so the owner's hand h is not drawn from it
            steps.push_back({EquipStep::Kind::Sheathe, h, held - 1});
    }
    std::array<uint8_t, 2> candidates{};   // per stale hand: bit k for each slot its weapon could go to
    for (int h = 0; h < 2; ++h) {
        if (!hints.staleHeld[h] || standIn.hand[h] != kEquipOther) continue;
        for (int k = 0; k < kEquipSlots; ++k)
            if (HandReachesSlot(h, k) && (owner.sheathed & (1u << k)) && !hasSlot[k] && standIn.hand[0] != k + 1 &&
                standIn.hand[1] != k + 1)
                candidates[h] |= uint8_t(1u << k);
    }
    for (int h = 0; h < 2; ++h) {
        const uint8_t c = candidates[h];
        if (c == 0 || (c & (c - 1)) != 0 || (c & candidates[1 - h]) != 0) continue;   // none, several, or shared
        int k = 0;
        while (!(c & (1u << k))) ++k;
        steps.push_back({EquipStep::Kind::Sheathe, h, k});
    }
    for (int h = 0; h < 2; ++h) {
        const uint8_t want = owner.hand[h];
        if (want == kEquipOther || standIn.hand[h] == want || !hasSlot[want - 1]) continue;
        if (standIn.hand[1 - h] == want) continue;   // its other hand holds it (the owner never sheathes a drawn one)
        steps.push_back({EquipStep::Kind::Draw, h, want - 1});
    }
    return steps;
}

// Which sheath slots of a stand-in the client anchors to its body (standin_slots.cpp): the game spawns each slot weapon
// under the actor and constrains its BaseMesh to a body component the mod then switches off and moves (BoneCore for the
// waist slots), so a sheathed weapon is left behind at the spawn point. A slot is anchored when it holds an actor, its
// sheath constraint is live (a dead one is left alone: it is logged) and its actor is in neither hand (a draw grabs the
// weapon while the slot variable still holds it: the hand owns it then). slotActors / constraintLive as in
// EquipSnapshot (slots / constrained); heldActors are the stand-in's "Weapon R" and "Weapon L".
inline std::array<bool, kEquipSlots> SlotAnchorPlan(const std::array<const void*, kEquipSlots>& slotActors,
                                                    const std::array<bool, kEquipSlots>& constraintLive,
                                                    const std::array<const void*, 2>& heldActors) {
    std::array<bool, kEquipSlots> out{};
    for (int k = 0; k < kEquipSlots; ++k) {
        const void* a = slotActors[size_t(k)];
        out[size_t(k)] = a && constraintLive[size_t(k)] && a != heldActors[0] && a != heldActors[1];
    }
    return out;
}

// Sender side, per fighter: a state is due when it is the first, differs from the last one sent, or the last one is
// kEquipResendMs old.
class EquipSender {
public:
    bool Due(const EquipStateMsg& now, double nowMs) {
        auto it = last_.find(now.entity);
        if (it != last_.end() && it->second.state.hand == now.hand && it->second.state.sheathed == now.sheathed &&
            nowMs - it->second.atMs < kEquipResendMs)
            return false;
        last_[now.entity] = Last{now, nowMs};
        return true;
    }
    void Forget(uint8_t entity) { last_.erase(entity); }

private:
    struct Last { EquipStateMsg state; double atMs = 0; };
    std::map<uint8_t, Last> last_;
};

}  // namespace hsm::combat
