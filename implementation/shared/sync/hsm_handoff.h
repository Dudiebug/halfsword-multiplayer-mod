// Hand-off (protocol rev 32): owner sync everywhere, except while one fighter's body is coupled to another's (a grab, a
// blade stuck in it). Then the machine of the fighter that grabbed or stabbed takes the other over: it simulates both
// bodies, the taken one from its player's input, so the two are in one simulation. It gives it back the moment they part
// (the blade is out, the hand lets go). Peer to peer: the host only relays these messages (docs/PLAN_HANDOFF.md).
//   HandOffAsk  asker -> host -> the victim's owner, reliable: my fighter `by` is coupled to your fighter `victim`.
//   HandOff     writer -> host -> everyone else, reliable: lease entries (the host sends a joiner the full table).
// A lease per fighter entity: an epoch, and the slot holding it (kNoSlot: its owner simulates it). Its owner grants it
// and may take it back; its holder gives it back. Each write adds one to the epoch, and a machine keeps only a newer
// one. Pose and Vitals carry the sender's epoch for the entity, so a stream from the side that no longer has it is dropped.
// Bots are never handed: a client fighter coupled to a bot is handed to the host, which has both bodies then.
#pragma once
#include "hsm_protocol.h"
#include "session/hsm_authority.h"
#include <array>
#include <cmath>
#include <cstdint>
#include <vector>

namespace hsm::sync {

// Why two fighters are coupled (bits).
enum Couple : uint8_t { kCoupleGrab = 1, kCoupleStuck = 2 };
constexpr uint8_t kCoupleMask = 3;

constexpr double kAskEveryMs = 250.0;     // an ask nobody answered is sent again this often while the coupling lasts
constexpr double kGrabReachUu = 45.0;     // a hand held to grab this close to another fighter's body: a grab (or one coming)
constexpr double kGrabKeepUu = 70.0;      // ... and the hold lasts while the hand stays this close (no flapping at the edge)
constexpr double kCoupleNearUu = 200.0;   // pelvises further apart than this: no grab checked

// Serial order: a is newer than b (epochs wrap at 256; a switch takes a frame at least, so 128 never pass unseen).
inline bool EpochNewer(uint8_t a, uint8_t b) { return a != b && uint8_t(a - b) < 128; }
// Only fighters are handed (entities 1..kMaxFighters); bots stay the host's.
inline bool LeaseEntity(uint8_t e) { return e >= 1 && e <= kMaxFighters; }
inline uint8_t LeaseOwner(uint8_t e) { return uint8_t(e - 1); }   // the fighter's own slot

struct Lease {
    uint8_t epoch = 0;
    uint8_t holder = kNoSlot;   // the slot simulating it now; kNoSlot: its owner
    uint8_t by = 0;             // the fighter it is coupled to (the holder's, or a bot of the host's)
    uint8_t why = 0;            // Couple bits at the grant
    bool Active() const { return holder != kNoSlot; }
};
struct LeaseEntry { uint8_t entity = 0; Lease lease; };

struct HandOffMsg {
    bool full = false;                 // the whole table (the host to a joiner): entries not listed are free at epoch 0
    std::vector<LeaseEntry> entries;
};
inline std::vector<uint8_t> Encode(const HandOffMsg& m) {
    Writer w; w.header(MsgType::HandOff);
    w.u8(m.full ? 1 : 0); w.u8(uint8_t(m.entries.size()));
    for (const LeaseEntry& e : m.entries) { w.u8(e.entity); w.u8(e.lease.epoch); w.u8(e.lease.holder); w.u8(e.lease.by); w.u8(e.lease.why); }
    return w.buf;
}
inline bool Decode(const uint8_t* d, size_t n, HandOffMsg& m) {
    Reader r(d, n); MsgType t; if (!r.header(t) || t != MsgType::HandOff) return false;
    const uint8_t flags = r.u8(), count = r.u8();
    if (flags > 1 || count > kMaxFighters) return false;
    m.full = flags != 0;
    m.entries.resize(count);
    for (LeaseEntry& e : m.entries) {
        e.entity = r.u8(); e.lease.epoch = r.u8(); e.lease.holder = r.u8(); e.lease.by = r.u8(); e.lease.why = r.u8();
        if (!LeaseEntity(e.entity) || (e.lease.why & ~kCoupleMask)) return false;
        if (e.lease.Active() && (e.lease.holder >= kMaxFighters || e.lease.holder == LeaseOwner(e.entity) || !ValidEntity(e.lease.by) || e.lease.by == e.entity))
            return false;
    }
    return r.done();
}

struct HandOffAskMsg { uint8_t victim = 0, by = 0, why = 0; };
inline std::vector<uint8_t> Encode(const HandOffAskMsg& m) {
    Writer w; w.header(MsgType::HandOffAsk);
    w.u8(m.victim); w.u8(m.by); w.u8(m.why);
    return w.buf;
}
inline bool Decode(const uint8_t* d, size_t n, HandOffAskMsg& m) {
    Reader r(d, n); MsgType t; if (!r.header(t) || t != MsgType::HandOffAsk) return false;
    m.victim = r.u8(); m.by = r.u8(); m.why = r.u8();
    if (!LeaseEntity(m.victim) || !ValidEntity(m.by) || m.by == m.victim || !m.why || (m.why & ~kCoupleMask)) return false;
    return r.done();
}

// Every machine's view of the leases.
class LeaseTable {
public:
    const Lease& Of(uint8_t e) const { static const Lease none; return LeaseEntity(e) ? leases_[e] : none; }
    uint8_t Epoch(uint8_t e) const { return Of(e).epoch; }
    uint8_t Holder(uint8_t e) const { return Of(e).holder; }
    bool Active(uint8_t e) const { return Of(e).Active(); }

    // An entry another machine wrote: kept when newer than the one known. *was: the lease it replaced.
    bool Apply(const LeaseEntry& in, Lease* was = nullptr) {
        if (!LeaseEntity(in.entity) || !EpochNewer(in.lease.epoch, leases_[in.entity].epoch)) return false;
        if (was) *was = leases_[in.entity];
        leases_[in.entity] = in.lease;
        return true;
    }
    // The host's full table, as it is (a joiner, a new host). True when the entity's lease changed.
    bool Set(const LeaseEntry& in, Lease* was = nullptr) {
        if (!LeaseEntity(in.entity)) return false;
        Lease& l = leases_[in.entity];
        if (l.epoch == in.lease.epoch && l.holder == in.lease.holder && l.by == in.lease.by) return false;
        if (was) *was = l;
        l = in.lease;
        return true;
    }
    // This machine writes: the owner grants (holder) or takes back (kNoSlot), the holder gives back. Applied here at once.
    LeaseEntry Write(uint8_t e, uint8_t holder, uint8_t by = 0, uint8_t why = 0) {
        LeaseEntry out; out.entity = e;
        if (!LeaseEntity(e)) return out;
        Lease& l = leases_[e];
        l.epoch = uint8_t(l.epoch + 1);
        l.holder = holder;
        l.by = holder == kNoSlot ? 0 : by;
        l.why = holder == kNoSlot ? 0 : uint8_t(why & kCoupleMask);
        out.lease = l;
        return out;
    }

    Holders holders() const {
        Holders h;
        for (uint8_t e = 1; e <= kMaxFighters; ++e) h.slot[e] = leases_[e].holder;
        return h;
    }
    bool HoldsAny(uint8_t slot) const {
        for (uint8_t e = 1; e <= kMaxFighters; ++e) if (leases_[e].Active() && leases_[e].holder == slot) return true;
        return false;
    }
    bool IsBy(uint8_t e) const {   // a lease is coupled to e
        for (uint8_t v = 1; v <= kMaxFighters; ++v) if (leases_[v].Active() && leases_[v].by == e) return true;
        return false;
    }
    bool Any() const { for (uint8_t e = 1; e <= kMaxFighters; ++e) if (leases_[e].Active()) return true; return false; }
    HandOffMsg Snapshot() const {
        HandOffMsg m; m.full = true;
        for (uint8_t e = 1; e <= kMaxFighters; ++e) if (leases_[e].epoch || leases_[e].Active()) m.entries.push_back({e, leases_[e]});
        return m;
    }
    void Clear() { leases_ = {}; }   // a new level, a new session: every lease free at epoch 0

private:
    std::array<Lease, kMaxFighters + 1> leases_{};
};

// The host's check of an entry a client wrote, against the lease it replaces: the owner grants its fighter to another
// machine (only a free one) or takes it back; the holder gives it back.
inline bool LeaseWriteOk(uint8_t fromSlot, const LeaseEntry& in, const Lease& cur) {
    if (!LeaseEntity(in.entity) || !EpochNewer(in.lease.epoch, cur.epoch) || uint8_t(in.lease.epoch - cur.epoch) != 1) return false;
    const uint8_t owner = LeaseOwner(in.entity);
    if (in.lease.Active()) return fromSlot == owner && in.lease.holder != owner && !cur.Active();
    return fromSlot == owner || (cur.Active() && fromSlot == cur.holder);
}

// The victim's owner (mySlot) on an ask from askerSlot, whose fighter `by` is coupled to the victim. No chains: a fighter
// is handed once, by a machine that holds nothing, to a machine whose own fighter is its own (it may hold others: one
// fighter may grab or stab two). When both ask for each other's
// fighter at once (askingBack: I am asking the asker for `by`), the lower slot takes the pair.
enum class AskVerdict : uint8_t { Grant, Wrong, Leased, Chain, Mine };
inline AskVerdict JudgeAsk(const LeaseTable& t, const HandOffAskMsg& a, uint8_t askerSlot, uint8_t mySlot, bool askingBack) {
    if (!LeaseEntity(a.victim) || LeaseOwner(a.victim) != mySlot || askerSlot == mySlot || OwnerSlotOf(a.by) != askerSlot) return AskVerdict::Wrong;
    if (t.Active(a.victim)) return AskVerdict::Leased;
    if (t.HoldsAny(mySlot) || t.IsBy(a.victim) || t.Active(a.by)) return AskVerdict::Chain;
    if (askingBack && mySlot < askerSlot) return AskVerdict::Mine;
    return AskVerdict::Grant;
}

// Geometry the coupling checks share.
struct HandOffPoint { double x = 0, y = 0, z = 0; };
inline double HandOffDist(const HandOffPoint& a, const HandOffPoint& b) {
    const double dx = a.x - b.x, dy = a.y - b.y, dz = a.z - b.z;
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}
// A hand within reach of any of the other fighter's body points.
inline bool HandOn(const HandOffPoint& hand, const std::vector<HandOffPoint>& otherBody, double reach) {
    for (const HandOffPoint& p : otherBody) if (HandOffDist(hand, p) <= reach) return true;
    return false;
}

inline const char* CoupleName(uint8_t why) {
    static const char* names[4] = {"none", "grab", "stuck blade", "grab+stuck"};
    return names[why & kCoupleMask];
}

}  // namespace hsm::sync
