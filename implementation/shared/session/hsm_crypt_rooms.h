// The shared Crypts run in the host's own dungeon (protocol rev 50, docs/PLAN_CRYPTS_COOP.md). The host's director places
// the rooms (random classes, dice, doors); a client builds exactly those, by streaming each room's levels at the host's
// Origin Point. Three messages carry it:
//   CryptRoom 73       host -> client, reliable: one room the host's dungeon built (seq, class, Origin Point, generation, parent,
//                      tier, which of its entrances joins its parent). The host numbers them 1, 2, 3 in the order it saw them; a
//                      joiner gets the whole list. A client builds in the order of seq.
//   CryptDoor 74       host -> client, reliable: a door, gate or barricade of the host's rooms and its state (closed, open, gone);
//                      sent when the host first sees it and again at each change (PR D; session/hsm_crypt_loot.h DoorBook).
//   CryptRoomsBuilt 75 client -> host, reliable: "my rooms 1..count are all built" (a prefix: the count only grows).
// The host holds a client's fighter spawn and regear until that client's CryptRoomsBuilt covers the entrance rooms (#3846: a
// client that spawned before its rooms existed fell through the floor and crashed on the next streaming change).
// Game-independent and unit-tested (session/test_hsm_crypt_rooms.cpp); gameplay/crypts_probe.cpp and crypts_layout.cpp carry
// it out.
#pragma once
#include "match/hsm_crypts_layout.h"
#include "protocol/hsm_protocol.h"
#include <cmath>
#include <cstdint>
#include <map>
#include <vector>

namespace hsm::crypt {

constexpr size_t kMaxRooms = 96;          // a run's rooms; the game's dungeon is a few dozen at most
constexpr size_t kMaxDoors = 192;
constexpr uint8_t kMaxGeneration = 64;
constexpr uint8_t kMaxTier = 16;
constexpr float kMaxCoordCm = 5.0e6f;
constexpr uint8_t kNoEntrance = 0xFF;     // the root room: no parent joint
constexpr uint32_t kReadyTimeoutMs = 45000;   // a client that never reports its rooms spawns anyway after this

// CryptRoomMsg::flags
constexpr uint8_t kRoomEntrance = 1;      // the game's Is Entrance? room (where the players start)
constexpr uint8_t kRoomBoss = 2;          // Is Boss Arena?
constexpr uint8_t kRoomExit = 4;          // Is Exit? / Leads To Exit
constexpr uint8_t kRoomKnownFlags = 7;

struct CryptRoomMsg {
    uint16_t seq = 0;            // 1.. in the order the host saw the rooms; never 0
    uint8_t catalog = 0;         // index in crypts::kRooms
    float x = 0, y = 0, z = 0, yaw = 0;   // the room's Origin Point (world; yaw in degrees)
    uint8_t generation = 0;      // the game's Generation: 0 the first room, 1 its children
    uint16_t parentSeq = 0;      // the parent's seq; 0 for the root
    uint8_t tier = 0;            // Room Tier
    uint8_t startEntrance = kNoEntrance;   // which of this room's entrances joins its parent (kNoEntrance: root)
    uint8_t flags = 0;
};
// CryptDoorMsg::kind: which of the game's door actors this is
constexpr uint8_t kDoorGate = 0;          // BP_Door_Gate_A_001 (a physical gate; locked until a key sits in its collider)
constexpr uint8_t kDoorTearDown = 1;      // BP_TearDown_Barricade_Door (planks a fighter breaks)
constexpr uint8_t kDoorUnbreakable = 2;   // BP_Door_Barricade_Unbreakable
// CryptDoorMsg::state
constexpr uint8_t kDoorClosed = 0;        // a gate locked / a barricade standing
constexpr uint8_t kDoorOpen = 1;          // a gate unlocked (a key opened it)
constexpr uint8_t kDoorGone = 2;          // torn down or destroyed
struct CryptDoorMsg {
    uint16_t seq = 0;            // 1.. never 0; the host numbers its doors in the order it saw them, a state change repeats the seq
    uint8_t kind = 0;            // kDoor*
    float x = 0, y = 0, z = 0, yaw = 0;
    uint8_t state = kDoorClosed; // kDoor*, trailing in rev 50 (PR D)
};
struct CryptRoomsBuiltMsg { uint16_t count = 0; };   // rooms 1..count built on the sender

inline bool CoordOk(float v) { return std::isfinite(v) && std::fabs(v) <= kMaxCoordCm; }

inline std::vector<uint8_t> Encode(const CryptRoomMsg& m) {
    Writer w; w.header(MsgType::CryptRoom);
    w.u16(m.seq); w.u8(m.catalog); w.f32(m.x); w.f32(m.y); w.f32(m.z); w.f32(m.yaw);
    w.u8(m.generation); w.u16(m.parentSeq); w.u8(m.tier); w.u8(m.startEntrance); w.u8(m.flags);
    return w.buf;
}
inline bool Decode(const uint8_t* d, size_t n, CryptRoomMsg& m) {
    Reader r(d, n); MsgType t; if (!r.header(t) || t != MsgType::CryptRoom) return false;
    CryptRoomMsg v;
    v.seq = r.u16(); v.catalog = r.u8(); v.x = r.f32(); v.y = r.f32(); v.z = r.f32(); v.yaw = r.f32();
    v.generation = r.u8(); v.parentSeq = r.u16(); v.tier = r.u8(); v.startEntrance = r.u8(); v.flags = r.u8();
    if (!r.done()) return false;
    if (v.seq == 0 || v.seq > kMaxRooms || v.catalog >= crypts::kRoomCount) return false;
    if (!CoordOk(v.x) || !CoordOk(v.y) || !CoordOk(v.z) || std::fabs(v.yaw) > 360.0f) return false;
    if (v.generation > kMaxGeneration || v.tier > kMaxTier || (v.flags & ~kRoomKnownFlags)) return false;
    if (v.parentSeq >= v.seq) return false;                                  // a parent comes first
    if (v.parentSeq == 0) { if (v.startEntrance != kNoEntrance) return false; }
    else if (v.startEntrance >= crypts::kRooms[v.catalog].entranceCount) return false;
    m = v;
    return true;
}

inline std::vector<uint8_t> Encode(const CryptDoorMsg& m) {
    Writer w; w.header(MsgType::CryptDoor);
    w.u16(m.seq); w.u8(m.kind); w.f32(m.x); w.f32(m.y); w.f32(m.z); w.f32(m.yaw); w.u8(m.state);
    return w.buf;
}
inline bool Decode(const uint8_t* d, size_t n, CryptDoorMsg& m) {
    Reader r(d, n); MsgType t; if (!r.header(t) || t != MsgType::CryptDoor) return false;
    CryptDoorMsg v;
    v.seq = r.u16(); v.kind = r.u8(); v.x = r.f32(); v.y = r.f32(); v.z = r.f32(); v.yaw = r.f32(); v.state = r.u8();
    if (!r.done()) return false;
    if (v.seq == 0 || v.seq > kMaxDoors || v.kind > kDoorUnbreakable || v.state > kDoorGone) return false;
    if (!CoordOk(v.x) || !CoordOk(v.y) || !CoordOk(v.z) || std::fabs(v.yaw) > 360.0f) return false;
    m = v;
    return true;
}

inline std::vector<uint8_t> Encode(const CryptRoomsBuiltMsg& m) {
    Writer w; w.header(MsgType::CryptRoomsBuilt); w.u16(m.count); return w.buf;
}
inline bool Decode(const uint8_t* d, size_t n, CryptRoomsBuiltMsg& m) {
    Reader r(d, n); MsgType t; if (!r.header(t) || t != MsgType::CryptRoomsBuilt) return false;
    const uint16_t c = r.u16();
    if (!r.done() || c > kMaxRooms) return false;
    m.count = c;
    return true;
}

// The rooms a machine knows, by seq. The host adds each room it reads (Add assigns the next seq); a client stores what it
// receives (Receive) and builds the contiguous prefix 1..Known() in order.
class RoomList {
public:
    // Host: a room just read. `msg.seq` is set here. Returns the stored message, or nullptr when the list is full.
    const CryptRoomMsg* Add(CryptRoomMsg msg) {
        if (rooms_.size() >= kMaxRooms) return nullptr;
        msg.seq = uint16_t(rooms_.size() + 1);
        return &rooms_.emplace(msg.seq, msg).first->second;
    }
    // Client: a room from the host. A repeat of a seq is ignored (false).
    bool Receive(const CryptRoomMsg& msg) {
        if (msg.seq == 0 || msg.seq > kMaxRooms) return false;
        return rooms_.emplace(msg.seq, msg).second;
    }
    const CryptRoomMsg* Find(uint16_t seq) const { auto it = rooms_.find(seq); return it == rooms_.end() ? nullptr : &it->second; }
    size_t Size() const { return rooms_.size(); }
    // The largest n with rooms 1..n all known (what a client can build now).
    uint16_t Known() const { uint16_t n = 0; while (rooms_.count(uint16_t(n + 1))) ++n; return n; }
    // The rooms the host sends a joiner, in seq order.
    std::vector<CryptRoomMsg> All() const { std::vector<CryptRoomMsg> v; for (const auto& kv : rooms_) v.push_back(kv.second); return v; }
    // The seq a client must have built before it may spawn: the highest seq among the entrance rooms (generation <= 1: the
    // placed room and its children, all loaded by the time the level starts). 0: no rooms known yet, nothing is covered.
    uint16_t EntranceCover() const { uint16_t hi = 0; for (const auto& kv : rooms_) if (kv.second.generation <= 1 && kv.first > hi) hi = kv.first; return hi; }
    void Clear() { rooms_.clear(); }
private:
    std::map<uint16_t, CryptRoomMsg> rooms_;
};

// Which of a client's rooms are built. Built(seq) marks one; Prefix() is what CryptRoomsBuilt reports.
class BuiltSet {
public:
    void Mark(uint16_t seq) { if (seq) done_[seq] = true; }
    uint16_t Prefix() const { uint16_t n = 0; while (done_.count(uint16_t(n + 1))) ++n; return n; }
    bool Has(uint16_t seq) const { return done_.count(seq) != 0; }
    void Clear() { done_.clear(); }
private:
    std::map<uint16_t, bool> done_;
};

// What the host does with a client's fighter spawn (#3846).
enum class Hold : uint8_t { Spawn, Wait, TimedOut };

// The host's view of every client's progress. Report(slot, count) takes a CryptRoomsBuilt (the count only grows). Decide() says
// whether `slot` may get its fighter: Spawn once its count covers the entrance rooms (latched: later rooms never take a body
// away), Wait until then, TimedOut once kReadyTimeoutMs has passed since Arm() with no cover (spawn anyway, logged by the caller).
class ReadyTracker {
public:
    void Arm(uint32_t nowMs) { armedMs_ = nowMs; armed_ = true; }
    void Disarm() { armed_ = false; reported_.clear(); ready_.clear(); }
    bool Armed() const { return armed_; }
    void Report(uint8_t slot, uint16_t count) { uint16_t& c = reported_[slot]; if (count > c) c = count; }
    uint16_t Reported(uint8_t slot) const { auto it = reported_.find(slot); return it == reported_.end() ? 0 : it->second; }
    Hold Decide(uint8_t slot, uint16_t cover, uint32_t nowMs) {
        if (!armed_) return Hold::Spawn;                       // not a shared run: nothing to wait for
        if (ready_.count(slot)) return Hold::Spawn;
        if (cover != 0 && Reported(slot) >= cover) { ready_[slot] = 1; return Hold::Spawn; }
        if (nowMs - armedMs_ >= kReadyTimeoutMs) { ready_[slot] = 2; return Hold::TimedOut; }
        return Hold::Wait;
    }
    bool IsReady(uint8_t slot) const { return ready_.count(slot) != 0; }
    void Forget(uint8_t slot) { reported_.erase(slot); ready_.erase(slot); }   // a leaver
private:
    bool armed_ = false;
    uint32_t armedMs_ = 0;
    std::map<uint8_t, uint16_t> reported_;
    std::map<uint8_t, uint8_t> ready_;
};

// Where a joint is: the world pose of a child room's start entrance (what the client clears the gate at).
inline crypts::Pose JointOf(const CryptRoomMsg& m) {
    const crypts::RoomClass& rc = crypts::kRooms[m.catalog];
    if (m.startEntrance >= rc.entranceCount) return {m.x, m.y, m.z, m.yaw};
    return crypts::ToWorld({m.x, m.y, m.z, m.yaw}, rc.entrances[m.startEntrance]);
}

}  // namespace hsm::crypt
