// World object sync: loose weapons and physics props. Wire format, host-side tracker and client-side view.
// Game-independent, unit-tested. The host owns every object; clients only render the latest state per id.
// Host:   WorldState st = tracker.Update(tick, live, players); auto bytes = Encode(st);   // send unreliable
// Client: WorldState st; if (Decode(data, size, st)) changes = view.Apply(st);
#pragma once
#include "hsm_protocol.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <map>
#include <set>
#include <vector>

namespace hsm {

// Per packet. Header (4 protocol + 4 tick + 2 count) + 128 * 38 bytes = 4874 bytes, well under kMaxPacket.
// Kept small so one packet fragments into few ENet fragments (an unreliable packet is lost if any fragment is).
constexpr size_t kMaxWorldObjects = 128;
constexpr size_t kWorldHeaderBytes = 4 + 4 + 2;
constexpr size_t kWorldObjectBytes = 2 + 1 + 2 + 12 + 8 + 12 + 1;   // id, kind, owner+slot, pos, quat, vel, flags
static_assert(kWorldHeaderBytes + kMaxWorldObjects * kWorldObjectBytes <= kMaxPacket,
              "a full WorldState must fit in kMaxPacket");

constexpr uint16_t kMaxWorldId = 0xFFFF;      // id 0 is reserved and never sent
constexpr uint32_t kSleepResendTicks = 10;    // unchanged sleeping objects are resent at this cadence
constexpr uint8_t kWorldTombstoneSends = 3;   // a removal is sent this many ticks so one lost packet does not hide it

enum class WorldKind : uint8_t { Weapon = 0, Prop = 1, Count = 2 };
enum WorldFlags : uint8_t { kWorldHeld = 1, kWorldSleeping = 2, kWorldRemoved = 4 };
constexpr uint8_t kWorldFlagMask = kWorldHeld | kWorldSleeping | kWorldRemoved;

struct WorldObject {
    uint16_t id = 0;
    uint8_t kind = 0;                 // WorldKind
    uint8_t owner = 0, slot = 0;      // weapon binding: last holder's entity and its SheathSlot; owner 0 = unbound
    float px = 0, py = 0, pz = 0;
    Quat16 rot;
    float vx = 0, vy = 0, vz = 0;
    uint8_t flags = 0;                // WorldFlags. kWorldRemoved marks a tombstone (no other fields meaningful)
};
struct WorldState {
    uint32_t tick = 0;
    std::vector<WorldObject> objects;
};

// ---------------------------------------------------------------- wire
inline std::vector<uint8_t> Encode(const WorldState& m) {
    Writer w; w.header(MsgType::WorldState); w.u32(m.tick);
    size_t n = m.objects.size() < kMaxWorldObjects ? m.objects.size() : kMaxWorldObjects; w.u16(uint16_t(n));
    for (size_t i = 0; i < n; ++i) {
        const WorldObject& o = m.objects[i];
        w.u16(o.id); w.u8(o.kind); w.u8(o.owner); w.u8(o.slot); w.f32(o.px); w.f32(o.py); w.f32(o.pz);
        w.i16(o.rot.x); w.i16(o.rot.y); w.i16(o.rot.z); w.i16(o.rot.w);
        w.f32(o.vx); w.f32(o.vy); w.f32(o.vz); w.u8(o.flags);
    }
    return w.buf;
}
inline bool Decode(const uint8_t* d, size_t n, WorldState& m) {
    if (n > kMaxPacket) return false;
    Reader r(d, n); MsgType t; if (!r.header(t) || t != MsgType::WorldState) return false;
    m.tick = r.u32(); size_t cnt = r.u16(); if (cnt > kMaxWorldObjects) return false;
    m.objects.resize(cnt);
    for (size_t i = 0; i < cnt; ++i) {
        WorldObject& o = m.objects[i];
        o.id = r.u16(); o.kind = r.u8(); o.owner = r.u8(); o.slot = r.u8(); o.px = r.f32(); o.py = r.f32(); o.pz = r.f32();
        o.rot.x = r.i16(); o.rot.y = r.i16(); o.rot.z = r.i16(); o.rot.w = r.i16();
        o.vx = r.f32(); o.vy = r.f32(); o.vz = r.f32(); o.flags = r.u8();
        if (!r.ok()) return false;
        if (o.id == 0 || o.kind >= uint8_t(WorldKind::Count) || (o.flags & ~kWorldFlagMask)) return false;
        if ((o.flags & kWorldRemoved) && (o.flags & (kWorldHeld | kWorldSleeping))) return false;   // tombstones carry nothing else
        for (size_t j = 0; j < i; ++j) if (m.objects[j].id == o.id) return false;                 // duplicate id
    }
    return r.done();
}

// ---------------------------------------------------------------- host side
struct WorldPos { float x = 0, y = 0, z = 0; };

// One simulated object as the host sees it this tick. key is the caller's stable identity (any uint64).
struct WorldSample {
    uint64_t key = 0;
    WorldKind kind = WorldKind::Prop;
    uint8_t owner = 0, slot = 0;
    float px = 0, py = 0, pz = 0;
    Quat16 rot;
    float vx = 0, vy = 0, vz = 0;
    bool held = false, sleeping = false;
};

// Assigns wire ids to object keys and selects, per tick, which objects go out in the WorldState.
// Rules: moving objects go every tick; sleeping objects go on change or every kSleepResendTicks; removals go as
// tombstones for kWorldTombstoneSends ticks, and their id is not reused until those sends are done. When more
// objects are due than kMaxWorldObjects, tombstones go first, then the rest by squared distance to the nearest player.
class WorldTracker {
public:
    // live: every object the host currently simulates. Objects absent from live are removed.
    // players: fighter positions used for distance priority (empty = no priority, ordered by id).
    WorldState Update(uint32_t tick, const std::vector<WorldSample>& live, const std::vector<WorldPos>& players) {
        std::set<uint64_t> present;
        for (const WorldSample& s : live) {
            if (!present.insert(s.key).second) continue;                    // duplicate key in one tick: first wins
            if (!Finite(s)) continue;                                       // never send what the decoder rejects
            auto it = entries_.find(s.key);
            if (it == entries_.end()) {
                uint16_t id = Allocate(); if (id == 0) continue;            // out of ids (not reachable in practice)
                Entry e; e.id = id; it = entries_.emplace(s.key, e).first;
            }
            WorldObject& c = it->second.current;
            c.id = it->second.id; c.kind = uint8_t(s.kind); c.owner = s.owner; c.slot = s.slot;
            c.px = s.px; c.py = s.py; c.pz = s.pz; c.rot = s.rot;
            c.vx = s.vx; c.vy = s.vy; c.vz = s.vz;
            c.flags = uint8_t((s.held ? kWorldHeld : 0) | (s.sleeping ? kWorldSleeping : 0));
        }
        for (auto it = entries_.begin(); it != entries_.end();) {
            if (present.count(it->first)) { ++it; continue; }
            tombs_.push_back({it->second.id, kWorldTombstoneSends});
            it = entries_.erase(it);
        }

        struct Cand { bool tomb = false; size_t index = 0; double dist = 0; uint16_t id = 0; WorldObject obj; Entry* entry = nullptr; };
        std::vector<Cand> cands;
        for (size_t i = 0; i < tombs_.size(); ++i) {
            Cand c; c.tomb = true; c.index = i; c.id = tombs_[i].id;
            c.obj.id = tombs_[i].id; c.obj.flags = kWorldRemoved;
            cands.push_back(c);
        }
        for (auto& kv : entries_) {
            Entry& e = kv.second;
            bool moving = !(e.current.flags & kWorldSleeping);
            bool due = moving || !e.sent || Changed(e.current, e.last) || uint32_t(tick - e.lastTick) >= kSleepResendTicks;
            if (!due) continue;
            Cand c; c.id = e.id; c.obj = e.current; c.entry = &e;
            c.dist = players.empty() ? 0.0 : NearestDist2(e.current, players);
            cands.push_back(c);
        }
        std::stable_sort(cands.begin(), cands.end(), [](const Cand& a, const Cand& b) {
            if (a.tomb != b.tomb) return a.tomb;
            if (a.dist != b.dist) return a.dist < b.dist;
            return a.id < b.id;
        });

        WorldState out; out.tick = tick;
        for (const Cand& c : cands) {
            if (out.objects.size() == kMaxWorldObjects) break;
            out.objects.push_back(c.obj);
            if (c.tomb) { --tombs_[c.index].sendsLeft; continue; }
            c.entry->last = c.entry->current; c.entry->sent = true; c.entry->lastTick = tick;
        }
        for (auto it = tombs_.begin(); it != tombs_.end();) {
            if (it->sendsLeft == 0) { used_[it->id] = false; it = tombs_.erase(it); }   // id reusable from the next tick
            else ++it;
        }
        return out;
    }

    size_t tracked() const { return entries_.size(); }
    size_t pendingTombstones() const { return tombs_.size(); }

private:
    struct Entry {
        uint16_t id = 0;
        WorldObject current;     // latest host state
        WorldObject last;        // state as last sent
        bool sent = false;
        uint32_t lastTick = 0;
    };
    struct Tomb { uint16_t id = 0; uint8_t sendsLeft = 0; };

    static bool Finite(const WorldSample& s) {
        return std::isfinite(s.px) && std::isfinite(s.py) && std::isfinite(s.pz) &&
               std::isfinite(s.vx) && std::isfinite(s.vy) && std::isfinite(s.vz) &&
               s.kind < WorldKind::Count;
    }
    static bool Changed(const WorldObject& a, const WorldObject& b) {
        return a.kind != b.kind || a.flags != b.flags || a.px != b.px || a.py != b.py || a.pz != b.pz ||
               a.rot.x != b.rot.x || a.rot.y != b.rot.y || a.rot.z != b.rot.z || a.rot.w != b.rot.w ||
               a.vx != b.vx || a.vy != b.vy || a.vz != b.vz;
    }
    static double NearestDist2(const WorldObject& o, const std::vector<WorldPos>& players) {
        double best = -1;
        for (const WorldPos& p : players) {
            double dx = double(o.px) - p.x, dy = double(o.py) - p.y, dz = double(o.pz) - p.z;
            double d = dx * dx + dy * dy + dz * dz;
            if (best < 0 || d < best) best = d;
        }
        return best < 0 ? 0.0 : best;
    }
    uint16_t Allocate() {
        for (size_t i = 1; i <= size_t(kMaxWorldId); ++i)
            if (!used_[i]) { used_[i] = true; return uint16_t(i); }
        return 0;
    }

    std::map<uint64_t, Entry> entries_;
    std::vector<Tomb> tombs_;
    std::vector<bool> used_ = std::vector<bool>(size_t(kMaxWorldId) + 1, false);
};

// ---------------------------------------------------------------- client side
// Latest state per id. Each id keeps the tick of its last accepted update; a packet only changes an id when its
// tick is newer (wrap-safe via SeqNewer), so reordered or duplicate unreliable packets are ignored per object.
class WorldView {
public:
    struct Changes { std::vector<uint16_t> created, removed; };

    // st must have passed Decode.
    Changes Apply(const WorldState& st) {
        Changes ch;
        for (const WorldObject& o : st.objects) {
            auto it = objs_.find(o.id);
            if (o.flags & kWorldRemoved) {
                if (it != objs_.end() && SeqNewer(st.tick, it->second.tick)) { objs_.erase(it); ch.removed.push_back(o.id); }
                continue;
            }
            if (it == objs_.end()) { objs_.emplace(o.id, Entry{o, st.tick}); ch.created.push_back(o.id); }
            else if (SeqNewer(st.tick, it->second.tick)) it->second = Entry{o, st.tick};
        }
        return ch;
    }
    const WorldObject* Find(uint16_t id) const {
        auto it = objs_.find(id);
        return it == objs_.end() ? nullptr : &it->second.obj;
    }
    size_t size() const { return objs_.size(); }

private:
    struct Entry { WorldObject obj; uint32_t tick = 0; };
    std::map<uint16_t, Entry> objs_;
};

}  // namespace hsm
