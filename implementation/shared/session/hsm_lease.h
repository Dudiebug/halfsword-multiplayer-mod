// Dropped weapons and loose props in owner sync (protocol rev 9): FREE/TOUCH/HOLD leases, host-arbitrated.
// A prop starts FREE (the host simulates and streams it, exactly like a WorldState object used to). Touching it
// grants a TOUCH lease; holding it (a hand's grab) grants a HOLD lease; HOLD beats TOUCH, and between two requests
// at the same level the first one standing keeps it (ties lose). The lessee streams PropState itself; everyone
// else drives the prop toward that stream (PropStateView: only the granted streamer's states apply). A lease
// lapses kLeaseLapseMs after the last report -- a Request renewing the touch/hold, sent every kClaimRenewMs while
// it lasts (LeaseClaim). A PropState never renews: the lessee streams every tick for as long as it is the lessee,
// so a lessee that stopped renewing (crashed hand logic, lost link) must still lose the prop. The prop then reverts
// to FREE; leaving the session releases every lease a slot held. Weapons actively held/equipped are carried by
// Pose instead (protocol/hsm_owner.h) and never enter this table -- only loose/dropped weapons and level props do.
//
// The claim lifecycle (touch or hold -> request; let go -> keep a touch while the throw/push is still moving ->
// release after kRestReleaseMs at rest), the rest threshold and the stream of bodies nobody leases (as fast as a leased one while moving) follow
// Cyrex0/HalfSword-MP's HSMPWorld (docs/development/subsystems/world-replication.md), Copyright (c) 2026 Cyrex0 and
// the HalfSword-MP contributors, MIT (see THIRD_PARTY_NOTICES.md).
//
// Identity: a 64-bit propId every machine agrees on.
//   - A loose weapon: MakeWeaponPropId(lastOwnerEntity, lowByte), bit 63 clear. Owner sync (gameplay/prop_sync.cpp)
//     fills the low byte with MakeDropPropId's (seq << 1) | hand and bits 16-47 with a drop epoch, minted once by the
//     dropping fighter's machine: SheathSlot is a sheath type in this game, not unique per fighter (see "dropped
//     weapons" below). Bits 48-62 of a weapon id are always clear.
//   - A level prop: MakeLevelPropId(index) -- its index in the host's census (session/hsm_prop_census.h), which
//     every other machine matches to its own actor by class and place. Not a name: the level manager destroys the
//     actors tagged 'Random Delete' on a per-machine coin flip and re-spawns the ones tagged 'Respawn' under new
//     names (BP_LevelManager), so the two machines' names differ. Bit 63 set (kPropLevelBit), so the two id spaces
//     can never collide; bits 16-62 clear (IsCanonicalLevelPropId).
//   propId 0 is reserved ("no identity yet", e.g. a weapon nobody has ever held) and every decoder rejects it.
#pragma once
#include "hsm_protocol.h"
#include "session/hsm_authority.h"
#include <cmath>
#include <cstdint>
#include <map>
#include <vector>

namespace hsm {

enum class LeaseLevel : uint8_t { Free = 0, Touch = 1, Hold = 2 };
enum class LeaseAction : uint8_t { Request = 0, Release = 1, Grant = 2 };

constexpr double kLeaseLapseMs = 3000.0;      // no report for this long: the lease lapses, prop reverts to FREE
constexpr float kMaxPropCoord = 1.0e7f;       // cm; a level is far smaller, anything beyond is garbage
constexpr float kMaxPropSpeed = 5.0e4f;       // cm/s (500 m/s); generous, catches garbage rather than real motion
constexpr float kMaxPropSpin = 1.0e5f;        // deg/s
// A claim is renewed this often while it lasts: a third of the lapse, so two renewals can stall on a congested
// reliable channel before the lease lapses under a fighter still holding the prop.
constexpr double kClaimRenewMs = 1000.0;
// A contact (hit event) counts as touching for this long: hit events fire per physics contact, not per frame, so a
// fighter leaning on a prop reports it in bursts.
constexpr double kContactKeepMs = 250.0;
constexpr double kRestReleaseMs = 1500.0;     // a prop let go of and at rest this long is released (HSMPWorld)
constexpr float kRestSpeed = 8.0f;            // cm/s; slower (and turning slower than kRestSpin) is at rest (HSMPWorld)
constexpr float kRestSpin = 10.0f;            // deg/s
constexpr double kFreeStreamMs = 16.0;        // the host streams a moving FREE prop as fast as a leased one (about 60 Hz)...
constexpr double kFreeIdleMs = 1000.0;        // ... and one at rest once a second (a lost state, a late joiner)

// ---------------------------------------------------------------- identity
constexpr uint64_t kPropLevelBit = uint64_t(1) << 63;
constexpr int kDropEpochShift = 16;           // a dropped weapon's epoch: bits 16-47 ...
constexpr int kWeaponIdBits = 48;             // ... and nothing above them in a weapon id

// lastOwnerEntity 0 (no known holder yet) or out of range deliberately yields 0, the reserved/invalid id.
inline uint64_t MakeWeaponPropId(uint8_t lastOwnerEntity, uint8_t sheathSlot) {
    if (lastOwnerEntity == 0 || lastOwnerEntity > kMaxWillies) return 0;
    return (uint64_t(lastOwnerEntity) << 8) | sheathSlot;
}
inline uint64_t MakeLevelPropId(uint16_t hostIndex) { return kPropLevelBit | hostIndex; }

// An id some machine could have made: any level-prop hash, or a weapon id (any epoch) whose owner byte is a valid
// entity. Every decoder rejects anything else (0 included), so garbage never reaches the lease table or an actor lookup.
inline bool ValidPropId(uint64_t propId) {
    if (propId & kPropLevelBit) return true;
    if (propId >> kWeaponIdBits) return false;
    const uint64_t owner = (propId >> 8) & 0xFF;
    return owner >= 1 && owner <= kMaxWillies;
}
// What a receiver needs to find a weapon prop's actor: its holder entity and sheath slot (both 0 for a level prop).
inline bool IsLevelProp(uint64_t propId) { return (propId & kPropLevelBit) != 0; }
inline uint8_t PropOwnerEntity(uint64_t propId) { return IsLevelProp(propId) ? 0 : uint8_t(propId >> 8); }
inline uint8_t PropSheathSlot(uint64_t propId) { return IsLevelProp(propId) ? 0 : uint8_t(propId & 0xFF); }
// A level prop's census index (0 for a weapon id); IsCanonicalLevelPropId: a level id with nothing above the index,
// the only kind a census names (ValidPropId lets the rest through the decoders; prop_sync drops them).
inline uint16_t LevelPropIndex(uint64_t propId) { return IsLevelProp(propId) ? uint16_t(propId & 0xFFFF) : 0; }
inline bool IsCanonicalLevelPropId(uint64_t propId) { return IsLevelProp(propId) && (propId & ~(kPropLevelBit | 0xFFFF)) == 0; }

// ---------------------------------------------------------------- dropped weapons (owner sync)
// In the game a weapon's SheathSlot is its sheath *type* (Willie_BP Assign_R_Weapon_Slot: 0-4), not an index: two
// weapons of one fighter can share it. A weapon is therefore keyed when it is first dropped, by the one machine that
// saw it happen natively -- the owner of the fighter that let go: the weapon id's low byte is (seq << 1) | hand, the
// dropping hand (0 right, 1 left) and a 7-bit per-owner sequence, and bits 16-47 carry the owner's drop epoch: the
// minting machine starts it at a per-process value and advances it on every wrap of the sequence, so no id is minted
// twice -- not after 128 drops, not by a machine that reconnected -- whether or not an older prop with it still lies in
// the level (a machine only knows the ids it tracks or heard of lately). Every other machine has that fighter as a stand-in
// that normally still holds its copy in the same hand (its tick is blocked, so its hands change only when the mod changes
// them), which is how it finds the copy (DropSignature, ResolveDropNow); a copy that stand-in let go of already (the
// hand matching, sync/hsm_handmatch.h, or by itself) is the weapon it let go of from that hand most recently (prop_sync's
// Adopt). A weapon keeps its id for good once minted: picked up and dropped again, by anyone, it is the same prop.
inline uint64_t MakeDropPropId(uint8_t ownerEntity, uint8_t hand, uint8_t seq, uint32_t epoch = 0) {
    if (hand > 1) return 0;
    const uint64_t id = MakeWeaponPropId(ownerEntity, uint8_t(((seq & 0x7F) << 1) | hand));
    return id ? id | (uint64_t(epoch) << kDropEpochShift) : 0;
}
inline uint8_t PropHand(uint64_t propId) { return uint8_t(PropSheathSlot(propId) & 1); }
inline uint8_t PropDropSeq(uint64_t propId) { return uint8_t(PropSheathSlot(propId) >> 1); }
inline uint32_t PropDropEpoch(uint64_t propId) { return IsLevelProp(propId) ? 0 : uint32_t(propId >> kDropEpochShift); }

// The owner's machine, on a drop: the next id of ownerEntity's sequence and epoch that inUse(id) does not report live
// (an older drop still lying in the level keeps its id). Advances seq past what it returns, and the epoch with every
// wrap of seq; 0 when there is none.
template <class InUse>
uint64_t MintDropPropId(uint8_t ownerEntity, uint8_t hand, uint8_t& seq, uint32_t& epoch, InUse inUse) {
    for (int i = 0; i < 128; ++i) {
        const uint64_t id = MakeDropPropId(ownerEntity, hand, seq, epoch);
        if (!id) return 0;
        seq = uint8_t((seq + 1) & 0x7F);
        if (seq == 0) ++epoch;
        if (!inUse(id)) return id;
    }
    return 0;
}

// A drop as the rest of the session first hears of it: the owner's machine asking for (or the host granting it) a
// Touch on a weapon id it minted -- the thrown or dropped weapon is still that machine's simulation. Only this makes
// a machine that does not know propId look for the copy in the owner's stand-in's hand; a Hold (a pick-up), any
// other slot or a level prop never does.
inline bool DropSignature(uint64_t propId, LeaseLevel level, uint8_t slot) {
    if (IsLevelProp(propId) || !ValidPropId(propId) || level != LeaseLevel::Touch) return false;
    return slot == OwnerSlotOf(PropOwnerEntity(propId));
}

// May the copy in the stand-in's hand be released for that drop now? Only once the owner's newest pose shows that
// hand empty or holding a weapon of another class (poseHandClass: WeaponSample::classHash, 0 = none): a pose still
// showing the same class means the drop is not visible yet (or the owner holds an identical weapon again) -- wait.
// A copy of unknown class (0) is never released.
inline bool ResolveDropNow(bool havePose, uint32_t poseHandClass, uint32_t copyClass) {
    return havePose && copyClass != 0 && poseHandClass != copyClass;
}

// A lease change from the owner is a drop. A weapon in the hand of a fighter's stand-in here is that fighter's weapon
// in its hand there, which its machine leases HOLD for as long as the game's "Parent Actor" names the fighter; the moment
// it lets go (a drop, a throw, a disarm) the claim falls to TOUCH -- the throw is still its simulation -- or the lease
// ends. The host's Grant for that (it answers changes only, a renewal is no Grant) comes well before the owner's pose
// shows the hand empty, and before the hand matching (sync/hsm_handmatch.h) lets the copy go: it holds the last shown
// class kReattachMinMs, and until then the copy sits in the stand-in's hand and the owner's stream does not drive it.
// prev/prevSlot: the lease as this machine's view had it before the Grant (the streamer, kHostSlot when FREE);
// now/nowSlot: the Grant. ownerSlot: the roster slot whose machine simulates the fighter whose stand-in holds the copy.
// True only when standInHoldsIt (a prop this machine tracks sits in that stand-in's hand; an id it does not track is the
// PendingDrop path of DropSignature) and either the owner's HOLD ended (a TOUCH or FREE follows, or another slot's lease)
// or the owner is granted a TOUCH it did not have. A HOLD grant to the owner (a pick-up, its renewal), a TOUCH renewal,
// a TOUCH ending (a rest release) and every grant to another slot release nothing.
inline bool DropNoticed(LeaseLevel prev, uint8_t prevSlot, LeaseLevel now, uint8_t nowSlot, uint8_t ownerSlot, bool standInHoldsIt) {
    if (!standInHoldsIt) return false;
    const bool ownerHeld = prev == LeaseLevel::Hold && prevSlot == ownerSlot;
    const bool ownerHolds = now == LeaseLevel::Hold && nowSlot == ownerSlot;
    if (ownerHeld) return !ownerHolds;
    const bool ownerTouchedBefore = prev == LeaseLevel::Touch && prevSlot == ownerSlot;
    return now == LeaseLevel::Touch && nowSlot == ownerSlot && !ownerTouchedBefore;
}

// ---------------------------------------------------------------- PropState (unreliable)
// Sent every tick by whoever currently streams the prop: the lessee when leased, otherwise the host (a FREE prop
// is the host's to simulate, exactly as a WorldState object used to be). Everyone else applies it to the actor.
struct PropStateMsg {
    uint64_t propId = 0;
    uint8_t lessee = kHostSlot;               // the roster slot currently streaming it (kHostSlot when FREE)
    uint32_t senderTimeMs = 0;                // sender's own clock, for SeqNewer staleness on the unreliable channel
    float px = 0, py = 0, pz = 0;
    Quat16 rot;
    float vx = 0, vy = 0, vz = 0;              // linear velocity, cm/s
    float wx = 0, wy = 0, wz = 0;              // angular velocity, deg/s
};

inline std::vector<uint8_t> Encode(const PropStateMsg& m) {
    Writer w; w.header(MsgType::PropState);
    w.u64(m.propId); w.u8(m.lessee); w.u32(m.senderTimeMs);
    w.f32(m.px); w.f32(m.py); w.f32(m.pz);
    w.i16(m.rot.x); w.i16(m.rot.y); w.i16(m.rot.z); w.i16(m.rot.w);
    w.f32(m.vx); w.f32(m.vy); w.f32(m.vz);
    w.f32(m.wx); w.f32(m.wy); w.f32(m.wz);
    return w.buf;
}
inline bool Decode(const uint8_t* d, size_t n, PropStateMsg& m) {
    Reader r(d, n); MsgType t; if (!r.header(t) || t != MsgType::PropState) return false;
    m.propId = r.u64(); m.lessee = r.u8(); m.senderTimeMs = r.u32();
    m.px = r.f32(); m.py = r.f32(); m.pz = r.f32();
    m.rot.x = r.i16(); m.rot.y = r.i16(); m.rot.z = r.i16(); m.rot.w = r.i16();
    m.vx = r.f32(); m.vy = r.f32(); m.vz = r.f32();
    m.wx = r.f32(); m.wy = r.f32(); m.wz = r.f32();
    if (!r.done() || !ValidPropId(m.propId) || m.lessee >= kMaxFighters) return false;
    if (std::fabs(m.px) > kMaxPropCoord || std::fabs(m.py) > kMaxPropCoord || std::fabs(m.pz) > kMaxPropCoord) return false;
    if (std::fabs(m.vx) > kMaxPropSpeed || std::fabs(m.vy) > kMaxPropSpeed || std::fabs(m.vz) > kMaxPropSpeed) return false;
    if (std::fabs(m.wx) > kMaxPropSpin || std::fabs(m.wy) > kMaxPropSpin || std::fabs(m.wz) > kMaxPropSpin) return false;
    return true;
}

// At rest: slower than kRestSpeed and turning slower than kRestSpin (whole vectors, not per axis).
inline bool PropAtRest(const PropStateMsg& m) {
    return m.vx * m.vx + m.vy * m.vy + m.vz * m.vz < kRestSpeed * kRestSpeed &&
           m.wx * m.wx + m.wy * m.wy + m.wz * m.wz < kRestSpin * kRestSpin;
}

// The host streams a FREE prop (nobody leases it) when this is true; lastSentMs far in the past means never sent.
inline bool FreeStreamDue(double lastSentMs, bool atRest, double nowMs) {
    return nowMs - lastSentMs >= (atRest ? kFreeIdleMs : kFreeStreamMs);
}

// The streamer of a prop sends its state when this is true. A lessee streams at the pose rate while the prop is
// held (a held weapon swings even when its speed reads low) or moving (a throw, a push); a touched prop at rest once a
// second; a FREE prop on the host's schedule (FreeStreamDue).
constexpr double kLeaseStreamMs = 16.0;
inline bool PropStreamDue(LeaseLevel level, double lastSentMs, bool atRest, double nowMs) {
    if (level == LeaseLevel::Free) return FreeStreamDue(lastSentMs, atRest, nowMs);
    const bool fast = level == LeaseLevel::Hold || !atRest;
    return nowMs - lastSentMs >= (fast ? kLeaseStreamMs : kFreeIdleMs);
}

// ---------------------------------------------------------------- PropLease (reliable)
// Request: a slot asking for Touch or Hold (level must be one of those two). Release: a slot giving up whatever it
// holds (level sent as Free by convention; the host ignores it). Grant: the host's answer -- the resulting level
// and who holds it now (Free/kHostSlot when nobody does). Clients send Request/Release; only the host sends Grant.
struct PropLeaseMsg {
    uint64_t propId = 0;
    LeaseAction action = LeaseAction::Request;
    LeaseLevel level = LeaseLevel::Touch;
    uint8_t slot = kHostSlot;
};

inline std::vector<uint8_t> Encode(const PropLeaseMsg& m) {
    Writer w; w.header(MsgType::PropLease);
    w.u64(m.propId); w.u8(uint8_t(m.action)); w.u8(uint8_t(m.level)); w.u8(m.slot);
    return w.buf;
}
inline bool Decode(const uint8_t* d, size_t n, PropLeaseMsg& m) {
    Reader r(d, n); MsgType t; if (!r.header(t) || t != MsgType::PropLease) return false;
    m.propId = r.u64(); const uint8_t ac = r.u8(); const uint8_t lv = r.u8(); m.slot = r.u8();
    if (!r.done() || !ValidPropId(m.propId) || ac > 2 || lv > 2 || m.slot >= kMaxFighters) return false;
    m.action = LeaseAction(ac); m.level = LeaseLevel(lv);
    if (m.action == LeaseAction::Request && m.level == LeaseLevel::Free) return false;   // a request always names a level
    if (m.action == LeaseAction::Grant && m.level == LeaseLevel::Free && m.slot != kHostSlot) return false;   // FREE = the host's
    return true;
}

// The host, on a PropLease from the machine in fromSlot: a client only asks (Request) or lets go (Release), and only
// for itself. Grants are the host's alone.
inline bool LeaseFromPeerOk(const PropLeaseMsg& m, uint8_t fromSlot) {
    return m.action != LeaseAction::Grant && m.slot == fromSlot;
}

// ---------------------------------------------------------------- host arbitration
struct LeaseResult {
    bool granted = false;         // the request takes effect (immediately, or by evicting a weaker lease)
    uint8_t slot = kHostSlot;     // who holds it afterwards (the winner, win or lose)
    LeaseLevel level = LeaseLevel::Free;
    bool changed = false;         // the (slot, level) pair actually changed -- not a bare renewal
};

// Host-side table of who leases what. Pure state machine: the caller (host session code) decides when to call
// Request/Release from incoming PropLease messages, broadcasts a Grant per LeaseResult, and calls Tick once a
// frame to age leases out. No entry is ever stored at LeaseLevel::Free; its absence from the table means FREE.
class LeaseTable {
public:
    // slot reports touching/holding propId at nowMs. FREE -> granted. The same slot renewing/upgrading its own
    // lease always wins. A different slot only wins by requesting a strictly higher level; on a tie the holder
    // keeps it (first wins). Asking for Free is a no-op (that is Release); it neither stores nor drops anything.
    LeaseResult Request(uint64_t propId, uint8_t slot, LeaseLevel level, double nowMs) {
        if (level == LeaseLevel::Free) {
            LeaseResult r;
            Lessee(propId, r.slot, r.level);
            return r;
        }
        Entry& e = table_[propId];
        const LeaseLevel prevLevel = e.level;
        const uint8_t prevSlot = e.slot;
        const bool free = prevLevel == LeaseLevel::Free;
        const bool mine = !free && prevSlot == slot;
        const bool wins = free || mine || uint8_t(level) > uint8_t(prevLevel);
        LeaseResult r;
        if (wins) {
            e.slot = slot; e.level = level; e.lastMs = nowMs;
            r.granted = true;
            r.changed = free || prevSlot != slot || prevLevel != level;
        }
        r.slot = e.slot; r.level = e.level;
        return r;
    }

    // slot gives up its lease on propId. A no-op (not granted) if slot does not hold it, or it is already FREE.
    LeaseResult Release(uint64_t propId, uint8_t slot) {
        LeaseResult r;
        auto it = table_.find(propId);
        if (it == table_.end() || it->second.level == LeaseLevel::Free || it->second.slot != slot) {
            if (it != table_.end()) { r.slot = it->second.slot; r.level = it->second.level; }
            return r;
        }
        table_.erase(it);
        r.granted = true; r.changed = true;      // slot/level already default to kHostSlot/Free
        return r;
    }

    // slot disconnected (or its fighter despawned): release every lease it held. Returns the freed propIds.
    std::vector<uint64_t> EntityLeft(uint8_t slot) {
        std::vector<uint64_t> freed;
        for (auto it = table_.begin(); it != table_.end();) {
            if (it->second.slot == slot) { freed.push_back(it->first); it = table_.erase(it); }
            else ++it;
        }
        return freed;
    }

    // Called once per host tick. Lapses any lease not reported on in kLeaseLapseMs; returns the propIds that
    // lapsed (the host broadcasts a Grant(Free, kHostSlot) for each, and resumes streaming it itself).
    std::vector<uint64_t> Tick(double nowMs) {
        std::vector<uint64_t> lapsed;
        for (auto it = table_.begin(); it != table_.end();) {
            if (nowMs - it->second.lastMs >= kLeaseLapseMs) { lapsed.push_back(it->first); it = table_.erase(it); }
            else ++it;
        }
        return lapsed;
    }

    bool Lessee(uint64_t propId, uint8_t& slot, LeaseLevel& level) const {
        auto it = table_.find(propId);
        if (it == table_.end()) return false;
        slot = it->second.slot; level = it->second.level;
        return true;
    }

    // The one slot that may stream propId now: its lessee, or the host while it is FREE.
    uint8_t Streamer(uint64_t propId) const {
        auto it = table_.find(propId);
        return it == table_.end() ? kHostSlot : it->second.slot;
    }

    // A PropState the host received from fromSlot: only the current streamer's, and only sent as itself. Anything
    // else (a lessee that lost the prop and has not heard yet, a forged lessee) is dropped, not relayed.
    bool Accepts(const PropStateMsg& m, uint8_t fromSlot) const { return m.lessee == fromSlot && Streamer(m.propId) == fromSlot; }

    // Every current lease as the Grant a late joiner needs (a prop missing from it is FREE).
    std::vector<PropLeaseMsg> Grants() const {
        std::vector<PropLeaseMsg> out;
        for (const auto& [id, e] : table_) {
            PropLeaseMsg g; g.propId = id; g.action = LeaseAction::Grant; g.level = e.level; g.slot = e.slot;
            out.push_back(g);
        }
        return out;
    }

private:
    struct Entry { uint8_t slot = kHostSlot; LeaseLevel level = LeaseLevel::Free; double lastMs = 0.0; };
    std::map<uint64_t, Entry> table_;
};

// ---------------------------------------------------------------- every machine's view
// Who streams each prop (from the host's Grants) and its latest PropState. senderTimeMs is the streamer's own clock,
// so staleness is judged per streamer: the unreliable channel can reorder or duplicate (only a strictly newer time
// from the same streamer applies, as in WorldView, session/hsm_world.h), and a new streamer's first state applies
// whatever its clock says -- the host's clock may be hours ahead of the new lessee's. A state from anyone but the
// granted streamer is dropped (a former lessee's late packet, or a new lessee whose Grant has not arrived yet: the
// Grant is reliable, the state is not, so at most a round trip of its states is lost). Unknown props are FREE.
class PropStateView {
public:
    // The host's Grant (the host applies its own too): from now on only slot streams propId, the host when FREE.
    void OnGrant(uint64_t propId, uint8_t slot, LeaseLevel level) {
        Entry& e = entries_[propId];
        const uint8_t streamer = level == LeaseLevel::Free ? kHostSlot : slot;
        if (streamer != e.streamer) { e.streamer = streamer; e.fresh = false; }
        e.level = level;
    }
    // True if m came from the streamer and was newer: it is now the current state (drive the prop's actor to it).
    bool Apply(const PropStateMsg& m) {
        auto it = entries_.find(m.propId);
        if (m.lessee != (it == entries_.end() ? kHostSlot : it->second.streamer)) return false;
        if (it == entries_.end()) it = entries_.emplace(m.propId, Entry{}).first;
        Entry& e = it->second;
        if (e.fresh && !SeqNewer(m.senderTimeMs, e.state.senderTimeMs)) return false;
        e.state = m; e.fresh = true; e.have = true;
        return true;
    }
    // The latest state applied, kept across a streamer change until the new streamer's first one.
    const PropStateMsg* Find(uint64_t propId) const {
        auto it = entries_.find(propId);
        return it == entries_.end() || !it->second.have ? nullptr : &it->second.state;
    }
    uint8_t Streamer(uint64_t propId) const {
        auto it = entries_.find(propId);
        return it == entries_.end() ? kHostSlot : it->second.streamer;
    }
    LeaseLevel Level(uint64_t propId) const {
        auto it = entries_.find(propId);
        return it == entries_.end() ? LeaseLevel::Free : it->second.level;
    }
    void Clear() { entries_.clear(); }

private:
    struct Entry {
        uint8_t streamer = kHostSlot;
        LeaseLevel level = LeaseLevel::Free;
        bool fresh = false;           // state came from the current streamer (its time is the staleness baseline)
        bool have = false;            // state holds anything at all
        PropStateMsg state;
    };
    std::map<uint64_t, Entry> entries_;
};

// ---------------------------------------------------------------- each machine's claim on one prop
// What a machine asks the host for, from what its own fighters do to the prop: holding (a hand's grab on it) ->
// Hold; touching (a contact within kContactKeepMs) -> Touch; let go of, or no longer in contact, while it was mine
// -> keep a Touch while it still moves (the throw or push is this machine's simulation) and release it once it has
// been at rest for kRestReleaseMs. A claim is re-sent when its level changes and renewed every kClaimRenewMs; a
// denied claim is simply renewed (the host answers only changes), and the machine streams only once a Grant names
// it (PropStateView::Streamer).
enum class ClaimSend : uint8_t { None = 0, Request = 1, Release = 2 };

struct LeaseClaim {
    LeaseLevel sent = LeaseLevel::Free;   // what this machine last asked for (Free: nothing, or released)
    double sentMs = 0.0;
    double restSinceMs = -1.0;            // when the prop came to rest with nothing of mine on it (-1: not at rest)
};

// One step per tick. lastContactMs: the last contact of a fighter simulated here (or its weapon) with the prop, far
// in the past if none. moving: !PropAtRest of the prop as read here. level: the level wanted now (Free = none).
inline ClaimSend StepClaim(LeaseClaim& c, bool holding, double lastContactMs, bool moving, double nowMs, LeaseLevel& level) {
    const bool touching = nowMs - lastContactMs < kContactKeepMs;
    LeaseLevel want = LeaseLevel::Free;
    if (holding) want = LeaseLevel::Hold;
    else if (touching) want = LeaseLevel::Touch;
    else if (c.sent != LeaseLevel::Free) {
        if (moving) c.restSinceMs = -1.0;
        else if (c.restSinceMs < 0.0) c.restSinceMs = nowMs;
        want = c.restSinceMs >= 0.0 && nowMs - c.restSinceMs >= kRestReleaseMs ? LeaseLevel::Free : LeaseLevel::Touch;
    }
    if (holding || touching) c.restSinceMs = -1.0;
    level = want;
    if (want == LeaseLevel::Free) {
        if (c.sent == LeaseLevel::Free) return ClaimSend::None;
        c.sent = LeaseLevel::Free; c.restSinceMs = -1.0;
        return ClaimSend::Release;
    }
    if (want == c.sent && nowMs - c.sentMs < kClaimRenewMs) return ClaimSend::None;
    c.sent = want; c.sentMs = nowMs;
    return ClaimSend::Request;
}

// Contact islands. A contact between a prop and another object counts as touching for this machine (its claim above)
// when the other object is this machine's: a fighter simulated here or a weapon in its hand (the first behaviour), or a
// prop this machine streams (its own TOUCH / HOLD lease, or FREE while it is the host), so a prop pushed by a prop it
// simulates is simulated here too and the pile one hit disturbed has one simulator (two-player test 6: the barrel the
// touched stick held up stayed the host's, and the two copies moved differently). A prop streamed elsewhere is that
// streamer's island; a stand-in, and the weapon in its hand, is another machine's fighter and claims nothing.
// myEntity: the fighter of this machine the contact is credited to (for a fighter or its weapon its own, for a prop the one
// that touched that prop last, else this machine's), 0 when there is none. Returns the entity to claim for, 0 for none.
enum class OtherKind : uint8_t { OwnedFighter = 0, OwnedWeapon = 1, StandIn = 2, Prop = 3 };
inline uint8_t ContactClaimant(uint8_t myEntity, OtherKind kind, bool otherStreamedHere) {
    switch (kind) {
    case OtherKind::OwnedFighter: return myEntity;
    case OtherKind::OwnedWeapon: return myEntity;
    case OtherKind::Prop: return otherStreamedHere ? myEntity : 0;
    case OtherKind::StandIn: return 0;
    }
    return 0;
}

}  // namespace hsm
