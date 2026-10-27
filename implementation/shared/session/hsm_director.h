// N-fighter session bookkeeping. Game-independent, unit-tested; the mod only executes what these decide.
//   HostDirector - host: which roster slots need a fighter spawned/removed on the current level, and the
//                  Spawn records late-ready clients must be sent.
//   ClientWorld  - client: which announced entities need a local puppet, and which puppets to destroy.
//   BlobStore    - both: latest loadout blob per slot, addressed by host-assigned blob ids.
#pragma once
#include "hsm_protocol.h"
#include "session/hsm_roster.h"
#include <cstdint>
#include <map>
#include <vector>

namespace hsm {

// Blob ids are assigned by the host: high byte = slot + 1 (entity - 1 for bots), low 24 bits = per-slot version (>= 1). Never 0.
inline uint32_t MakeBlobId(uint8_t slot, uint32_t version) { return (uint32_t(slot + 1) << 24) | ((version & 0xFFFFFFu) ? (version & 0xFFFFFFu) : 1u); }
inline int BlobSlot(uint32_t blobId) { uint32_t s = blobId >> 24; return s >= 1 && s <= uint32_t(kMaxWillies) ? int(s - 1) : -1; }   // fighter slots 0..8, bots 9..18

class BlobStore {
public:
    // Keeps only the newest version per slot. Returns false for ids that do not map to a slot or are stale.
    bool Put(uint32_t blobId, std::vector<uint8_t> bytes) {
        int slot = BlobSlot(blobId);
        if (slot < 0 || bytes.empty()) return false;
        auto it = bySlot_.find(uint8_t(slot));
        if (it != bySlot_.end() && (it->second.id & 0xFFFFFFu) > (blobId & 0xFFFFFFu)) return false;
        bySlot_[uint8_t(slot)] = {blobId, std::move(bytes)};
        return true;
    }
    const std::vector<uint8_t>* Get(uint32_t blobId) const {
        int slot = BlobSlot(blobId);
        if (slot < 0) return nullptr;
        auto it = bySlot_.find(uint8_t(slot));
        return it != bySlot_.end() && it->second.id == blobId ? &it->second.bytes : nullptr;
    }
    // The slot's newest blob if it is the requested version or newer (a Spawn may name a superseded version).
    const std::vector<uint8_t>* AtLeast(uint32_t blobId) const {
        int slot = BlobSlot(blobId);
        if (slot < 0) return nullptr;
        auto it = bySlot_.find(uint8_t(slot));
        return it != bySlot_.end() && (it->second.id & 0xFFFFFFu) >= (blobId & 0xFFFFFFu) ? &it->second.bytes : nullptr;
    }
    uint32_t LatestFor(uint8_t slot) const { auto it = bySlot_.find(slot); return it == bySlot_.end() ? 0 : it->second.id; }
    void DropSlot(uint8_t slot) { bySlot_.erase(slot); }
    void Clear() { bySlot_.clear(); }
    size_t size() const { return bySlot_.size(); }

private:
    struct Entry { uint32_t id = 0; std::vector<uint8_t> bytes; };
    std::map<uint8_t, Entry> bySlot_;
};

class HostDirector {
public:
    static constexpr uint32_t kReadyTimeoutMs = 20000;   // a client that never reports Ready still gets a fighter
    static constexpr uint32_t kBlobGraceMs = 5000;       // after Ready, wait this long for a loadout before using default

    // Host loaded the level of travel `travelSeq` (0 = not from a broadcast travel). Actors from the previous level
    // are gone, so nothing is spawned any more. A client that already reported Ready for this very travel (it
    // loaded faster than the host) stays ready.
    void OnLevel(bool fight, uint32_t nowMs, uint32_t travelSeq) {
        fight_ = fight; live_.clear();
        for (auto& [slot, s] : slots_) {
            s.ready = slot == 0 || (travelSeq != 0 && s.readySeq == travelSeq);
            s.readyAt = nowMs; s.since = nowMs;
        }
    }
    void OnJoin(uint8_t slot, uint32_t nowMs) { Slot& s = slots_[slot]; s = Slot{}; s.ready = slot == 0; s.since = nowMs; s.readyAt = nowMs; }
    // Returns the entity to despawn (0 = none was live).
    uint8_t OnLeave(uint8_t slot) {
        slots_.erase(slot);
        uint8_t e = Roster::EntityForSlot(slot);
        return live_.erase(e) ? e : 0;
    }
    // currentSeq: the host's present travel (0 = not checked). A Ready for an older travel (a client that finished loading
    // the previous level late) is noted but does not make the slot ready for the level now loading.
    void OnReady(uint8_t slot, uint32_t travelSeq, uint32_t nowMs, uint32_t currentSeq = 0) {
        auto it = slots_.find(slot);
        if (it == slots_.end()) return;
        it->second.readySeq = travelSeq;
        if (currentSeq && travelSeq != currentSeq) return;
        if (!it->second.ready) { it->second.ready = true; it->second.readyAt = nowMs; }
    }
    // The travel the slot last reported Ready for (0: none yet, or not a client).
    uint32_t ReadySeq(uint8_t slot) const { auto it = slots_.find(slot); return it == slots_.end() ? 0 : it->second.readySeq; }
    void OnBlob(uint8_t slot, uint32_t blobId) { auto it = slots_.find(slot); if (it != slots_.end()) it->second.blobId = blobId; }

    // Slots whose fighter should be spawned now, lowest slot first. Slot 0 is the host's own stock fighter:
    // the mod only announces it, but it goes through the same decision.
    std::vector<uint8_t> Due(uint32_t nowMs) const {
        std::vector<uint8_t> out;
        if (!fight_) return out;
        for (const auto& [slot, s] : slots_) {
            if (live_.count(Roster::EntityForSlot(slot))) continue;
            bool ready = s.ready || nowMs - s.since >= kReadyTimeoutMs;
            uint32_t readyAt = s.ready ? s.readyAt : s.since + kReadyTimeoutMs;
            bool loadout = s.blobId != 0 || slot == 0 || nowMs - readyAt >= kBlobGraceMs;
            if (ready && loadout) out.push_back(slot);
        }
        return out;
    }
    uint32_t BlobFor(uint8_t slot) const { auto it = slots_.find(slot); return it == slots_.end() ? 0 : it->second.blobId; }
    void MarkSpawned(const Spawn& sp) { live_[sp.entity] = sp; }
    void Forget(uint8_t entity) { live_.erase(entity); }   // a bot removed for good (wave cleared), or a rejoiner's old fighter
    bool Live(uint8_t entity) const { return live_.count(entity) != 0; }
    // Everything a (re)joining or newly ready client must be told about, in entity order.
    std::vector<Spawn> LiveSpawns() const { std::vector<Spawn> v; for (const auto& [e, sp] : live_) v.push_back(sp); return v; }
    bool fight() const { return fight_; }

private:
    struct Slot { bool ready = false; uint32_t since = 0, readyAt = 0, blobId = 0, readySeq = 0; };
    bool fight_ = false;
    std::map<uint8_t, Slot> slots_;
    std::map<uint8_t, Spawn> live_;
};

class ClientWorld {
public:
    void SetMyEntity(uint8_t e) { mine_ = e; }
    uint8_t myEntity() const { return mine_; }
    // Local actors died with the level; the host re-announces everything after this client's Ready.
    void OnLevel() { entries_.clear(); }
    // Returns true when an existing puppet must be destroyed first (re-announce with a different loadout).
    bool OnSpawn(const Spawn& sp) {
        auto it = entries_.find(sp.entity);
        bool replace = it != entries_.end() && it->second.created && it->second.spawn.blobId != sp.blobId;
        Entry e; e.spawn = sp; e.created = it != entries_.end() && it->second.created && !replace;
        entries_[sp.entity] = e;
        return replace;
    }
    // Returns true when a local puppet existed and must be destroyed.
    bool OnDespawn(uint8_t entity) {
        auto it = entries_.find(entity);
        if (it == entries_.end()) return false;
        bool created = it->second.created; entries_.erase(it); return created;
    }
    // Puppets that can be created now: announced, not this client's own fighter, not yet created, and the
    // loadout (if any) has arrived. haveBlob(id) should accept newer versions too (BlobStore::AtLeast).
    template <class HaveBlob> std::vector<Spawn> PendingPuppets(HaveBlob haveBlob) const {
        std::vector<Spawn> out;
        for (const auto& [e, en] : entries_)
            if (e != mine_ && !en.created && (en.spawn.blobId == 0 || haveBlob(en.spawn.blobId))) out.push_back(en.spawn);
        return out;
    }
    void MarkCreated(uint8_t entity) { auto it = entries_.find(entity); if (it != entries_.end()) it->second.created = true; }
    // A created puppet the game destroyed while its entity is still announced: pending again. False when there is
    // nothing to re-create (unknown, never created, or this client's own fighter).
    bool MarkLost(uint8_t entity) {
        auto it = entries_.find(entity);
        if (it == entries_.end() || entity == mine_ || !it->second.created) return false;
        it->second.created = false;
        return true;
    }
    bool Known(uint8_t entity) const { return entries_.count(entity) != 0; }
    const Spawn* Find(uint8_t entity) const { auto it = entries_.find(entity); return it == entries_.end() ? nullptr : &it->second.spawn; }
    size_t size() const { return entries_.size(); }

private:
    struct Entry { Spawn spawn; bool created = false; };
    uint8_t mine_ = 0;
    std::map<uint8_t, Entry> entries_;
};

}  // namespace hsm
