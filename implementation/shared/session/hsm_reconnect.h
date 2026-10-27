// Reconnect and host-leave decisions. Game-independent, unit-tested; the mod only executes what these decide.
//   ReservationTable - host: a member that disconnects mid-match keeps its slot (roster entry, entity, score)
//                      for kReserveMs. Only the same key can claim it back. Lapsed slots are reported once by Expire.
//   ReconnectResend  - host: what a (re)joining member must be sent, in the order the client can apply it.
//   HostLossMonitor  - client: a silent or gone host becomes "reconnecting", then "return to menu" (exactly once).
// All times are millisecond clocks truncated to uint32. Arithmetic is modular, so they must only move forward.
#pragma once
#include "session/hsm_roster.h"
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <map>
#include <vector>

namespace hsm {

constexpr uint32_t kReserveMs = 60000;   // how long a disconnected member's slot, entity and score are held
constexpr size_t kMaxReservations = 5;   // clients only (slots 1..5); the host (slot 0) is never reserved

// True once nowMs is at least `window` ms after sinceMs. Wrap-safe: uint32 modular subtraction.
inline bool ElapsedAtLeast(uint32_t sinceMs, uint32_t nowMs, uint32_t window) { return uint32_t(nowMs - sinceMs) >= window; }
// K9: `nowMs` has reached the deadline `dueMs` on the 32-bit ms clock (modular: still right after the 49.7-day wrap).
inline bool DueAt(uint32_t nowMs, uint32_t dueMs) { return int32_t(nowMs - dueMs) >= 0; }

class ReservationTable {
public:
    // Holds `slot` for `key` until kReserveMs after nowMs. A repeat for the same key refreshes the timer (and
    // cancels a lapsed-but-unreported reservation of that key). Returns false, storing nothing, for slot 0 or
    // out of range, a slot held by another key, or a table already full of live reservations.
    // Lapsed entries are not dropped silently: they move to a pending list that the next Expire() returns.
    bool Reserve(uint64_t key, uint8_t slot, uint32_t nowMs);
    // The reserved slot when `key` holds a live reservation; the reservation is consumed. Otherwise nullopt.
    // A lapsed reservation is not claimable and stays for Expire() to report.
    std::optional<uint8_t> Claim(uint64_t key, uint32_t nowMs);
    // Slots whose reservations lapsed by nowMs, ascending. Each is reported once and then removed.
    std::vector<uint8_t> Expire(uint32_t nowMs);
    void Clear() { entries_.clear(); lapsed_.clear(); }
    size_t size() const { return entries_.size(); }

private:
    struct Entry { uint64_t key = 0; uint8_t slot = 0; uint32_t sinceMs = 0; };
    std::vector<Entry> entries_;   // live reservations
    std::vector<Entry> lapsed_;    // lapsed during Reserve(), waiting for Expire()
};

inline bool ReservationTable::Reserve(uint64_t key, uint8_t slot, uint32_t nowMs) {
    if (slot == 0 || slot >= kMaxFighters) return false;
    for (size_t i = 0; i < entries_.size();) {
        if (!ElapsedAtLeast(entries_[i].sinceMs, nowMs, kReserveMs)) { ++i; continue; }
        lapsed_.push_back(entries_[i]);
        entries_.erase(entries_.begin() + long(i));
    }
    std::erase_if(lapsed_, [&](const Entry& e) { return e.key == key; });   // this key is reserved again
    for (const Entry& e : entries_) if (e.slot == slot && e.key != key) return false;
    for (const Entry& e : lapsed_) if (e.slot == slot) return false;        // not yet reported: slot still busy
    for (Entry& e : entries_) if (e.key == key) { e.slot = slot; e.sinceMs = nowMs; return true; }
    if (entries_.size() >= kMaxReservations) return false;
    entries_.push_back({key, slot, nowMs});
    return true;
}

inline std::optional<uint8_t> ReservationTable::Claim(uint64_t key, uint32_t nowMs) {
    for (size_t i = 0; i < entries_.size(); ++i) {
        if (entries_[i].key != key) continue;
        if (ElapsedAtLeast(entries_[i].sinceMs, nowMs, kReserveMs)) return std::nullopt;
        uint8_t slot = entries_[i].slot;
        entries_.erase(entries_.begin() + long(i));
        return slot;
    }
    return std::nullopt;
}

inline std::vector<uint8_t> ReservationTable::Expire(uint32_t nowMs) {
    std::vector<uint8_t> out;
    for (const Entry& e : lapsed_) out.push_back(e.slot);
    lapsed_.clear();
    for (size_t i = 0; i < entries_.size();) {
        if (!ElapsedAtLeast(entries_[i].sinceMs, nowMs, kReserveMs)) { ++i; continue; }
        out.push_back(entries_[i].slot);
        entries_.erase(entries_.begin() + long(i));
    }
    std::sort(out.begin(), out.end());
    return out;
}

// What a (re)joining member is sent, in order. Welcome first (it assigns slot and entity). Loadouts and Travel
// precede Spawns because the client drops announced actors when it loads a level (ClientWorld::OnLevel).
// MatchInfo last. Spawns are normally also re-sent on the member's Ready.
enum class ResendItem : uint8_t { Welcome, LoadoutBlobs, CurrentTravel, LiveSpawns, MatchInfo };

struct ResendFacts {
    bool inFightLevel = false;    // host is in a fight level; Travel and Spawns only apply there
    bool hasLiveSpawns = false;   // HostDirector::LiveSpawns() is non-empty
    bool hasLoadouts = false;     // BlobStore holds at least one loadout
};

inline std::vector<ResendItem> ReconnectResend(const ResendFacts& f) {
    std::vector<ResendItem> out{ResendItem::Welcome};
    if (f.hasLoadouts) out.push_back(ResendItem::LoadoutBlobs);
    if (f.inFightLevel) out.push_back(ResendItem::CurrentTravel);
    if (f.inFightLevel && f.hasLiveSpawns) out.push_back(ResendItem::LiveSpawns);
    out.push_back(ResendItem::MatchInfo);
    return out;
}

// Client side. Silence is measured from the last packet from the host. Once HostLost, the monitor stays there
// until Reset(), so ReturnToMenu is emitted exactly once.
enum class HostLink : uint8_t { Connected, HostSilent, HostLost };
enum class HostLossAction : uint8_t { None, ShowReconnecting, ReturnToMenu };

class HostLossMonitor {
public:
    static constexpr uint32_t kSilentMs = 3000;    // no packet this long: show "reconnecting"
    static constexpr uint32_t kLostMs = 10000;     // no packet this long: the host is lost

    explicit HostLossMonitor(uint32_t nowMs) : lastPacketMs_(nowMs) {}
    void Reset(uint32_t nowMs) { link_ = HostLink::Connected; lastPacketMs_ = nowMs; }

    // Any packet from the host proves it is alive. HostSilent recovers to Connected with no action.
    // HostLost is terminal until Reset().
    void OnPacket(uint32_t nowMs) {
        if (link_ == HostLink::HostLost) return;
        link_ = HostLink::Connected; lastPacketMs_ = nowMs;
    }
    // An explicit disconnect from the host is lost immediately.
    HostLossAction OnDisconnect([[maybe_unused]] uint32_t nowMs) { return Lose(); }
    // Call once per tick. Silence past kLostMs goes straight to ReturnToMenu, even from Connected.
    HostLossAction Tick(uint32_t nowMs) {
        if (link_ == HostLink::HostLost) return HostLossAction::None;
        if (ElapsedAtLeast(lastPacketMs_, nowMs, kLostMs)) return Lose();
        if (link_ == HostLink::Connected && ElapsedAtLeast(lastPacketMs_, nowMs, kSilentMs)) {
            link_ = HostLink::HostSilent;
            return HostLossAction::ShowReconnecting;
        }
        return HostLossAction::None;
    }
    HostLink link() const { return link_; }

private:
    HostLossAction Lose() {
        if (link_ == HostLink::HostLost) return HostLossAction::None;
        link_ = HostLink::HostLost;
        return HostLossAction::ReturnToMenu;
    }
    HostLink link_ = HostLink::Connected;
    uint32_t lastPacketMs_ = 0;
};

// Host side, Steam peers: the Steam transport has no timeout of its own, so a peer that stops sending (a crashed or frozen
// client, a dead route) would keep its slot forever. Every message from a peer is Seen; Expired returns the peers silent for
// kSilentMs and forgets them, once each. The limit is generous: a client loading a level sends nothing for a while.
class PeerSilenceMonitor {
public:
    static constexpr uint32_t kSilentMs = 15000;
    void Seen(uint32_t peer, uint32_t nowMs) { last_[peer] = nowMs; }
    void Forget(uint32_t peer) { last_.erase(peer); }
    void Clear() { last_.clear(); }
    std::vector<uint32_t> Expired(uint32_t nowMs) {
        std::vector<uint32_t> out;
        for (auto it = last_.begin(); it != last_.end();) {
            if (ElapsedAtLeast(it->second, nowMs, kSilentMs)) { out.push_back(it->first); it = last_.erase(it); }
            else ++it;
        }
        return out;
    }
private:
    std::map<uint32_t, uint32_t> last_;
};

}  // namespace hsm
