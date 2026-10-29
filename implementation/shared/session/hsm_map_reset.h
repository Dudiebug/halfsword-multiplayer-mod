// Map reset (HSMMod 0.8.0-pre6, protocol rev 28): at a Duel round reset and at a rematch the map goes back to how the round
// began -- level props to where they started, objects spawned during the round gone, the weapons the level itself placed
// back on their spots -- without a level reload. The host sends one small MapReset (reliable) when its setting is on (no
// message when off); every machine then applies it to its own copy once its own respawn is done, like the dropped-weapon
// clear. The prop sync (gameplay/object_sync.cpp) does the engine work; what is decided here is pure and unit-tested:
//
//   wire     MapReset 54: a sequence number (a repeat is ignored), the round, flags
//   plan     what happens to each object (PlanReset): restore, destroy, respawn or leave it alone
//   pacing   how many teleports a frame may do (ResetPerFrame), so a map with hundreds of props never spikes one frame
//   gate     when a queued reset may run (MapResetGate): after the dropped-weapon clear and the own respawn
#pragma once
#include "hsm_protocol.h"
#include <algorithm>
#include <cstdint>
#include <vector>

namespace hsm {

constexpr uint8_t kMapResetRematch = 1;                    // flags bit 0: a rematch (else a Duel round)
constexpr uint8_t kMapResetFlagMask = kMapResetRematch;

// Wire: 4 header, u16 seq, u16 round, u8 flags.
struct MapResetMsg {
    uint16_t seq = 0;       // the host's count of resets; a machine ignores the one it already applied
    uint16_t round = 0;     // the Duel round that begins (0 for a rematch of another format)
    uint8_t flags = 0;
};
inline std::vector<uint8_t> Encode(const MapResetMsg& m) {
    Writer w; w.header(MsgType::MapReset); w.u16(m.seq); w.u16(m.round); w.u8(m.flags); return w.buf;
}
inline bool Decode(const uint8_t* d, size_t n, MapResetMsg& m) {
    Reader r(d, n); MsgType t; if (!r.header(t) || t != MsgType::MapReset) return false;
    m.seq = r.u16(); m.round = r.u16(); m.flags = r.u8();
    return r.done() && (m.flags & ~kMapResetFlagMask) == 0;
}

// ---------------------------------------------------------------- what happens to each object
enum class ResetKind : uint8_t { LevelProp, Spawned, LevelWeapon };
enum class ResetAction : uint8_t {
    Leave,      // not part of the round: stays as it is (an object the level had at the match start, a held thing)
    Restore,    // back to its start transform, velocities zero
    Destroy,    // spawned during the round: gone
    Respawn,    // a level weapon that was destroyed: a new one of its class on its spot
    Skip,       // cannot be reset (no start recorded, broken beyond repair, class unknown); logged
};
struct ResetCandidate {
    ResetKind kind = ResetKind::LevelProp;
    bool alive = true;         // the actor exists here
    bool haveStart = false;    // its start transform was recorded (a level prop or weapon)
    bool broken = false;       // a geometry collection that has shattered: the pieces cannot be put back together
    bool held = false;         // a hand holds it (a weapon with a Parent Actor)
    bool bornInRound = false;  // spawned objects: appeared after the match went live
    bool detached = false;     // a body that came off its object: it belongs to that object, not a thing of its own
    bool classKnown = false;   // level weapons: a loaded class that can be spawned again
};
inline ResetAction PlanReset(const ResetCandidate& c) {
    switch (c.kind) {
        case ResetKind::LevelProp:
            if (!c.alive) return ResetAction::Leave;           // the census drops it; nothing to move
            if (c.held) return ResetAction::Leave;
            if (!c.haveStart || c.broken || c.detached) return ResetAction::Skip;
            return ResetAction::Restore;
        case ResetKind::Spawned:
            if (!c.bornInRound || !c.alive || c.held) return ResetAction::Leave;
            if (c.detached) return ResetAction::Skip;          // destroying it would take the old actor with it
            return ResetAction::Destroy;
        case ResetKind::LevelWeapon:
            if (!c.haveStart) return ResetAction::Skip;
            if (c.alive) return c.held ? ResetAction::Leave : ResetAction::Restore;
            return c.classKnown ? ResetAction::Respawn : ResetAction::Skip;
    }
    return ResetAction::Leave;
}

struct ResetTally {
    uint32_t restored = 0, destroyed = 0, respawned = 0, skipped = 0, left = 0;
    void Count(ResetAction a) {
        switch (a) {
            case ResetAction::Restore: ++restored; break;
            case ResetAction::Destroy: ++destroyed; break;
            case ResetAction::Respawn: ++respawned; break;
            case ResetAction::Skip: ++skipped; break;
            case ResetAction::Leave: ++left; break;
        }
    }
};

// ---------------------------------------------------------------- pacing
constexpr size_t kResetMaxFrames = 3;      // a reset of any size is done within this many frames
constexpr size_t kResetMinPerFrame = 16;   // a small map is done in one frame
inline size_t ResetPerFrame(size_t total) {
    return std::max(kResetMinPerFrame, (total + kResetMaxFrames - 1) / kResetMaxFrames);
}

// ---------------------------------------------------------------- when it runs
// A queued reset waits kMapResetDelayMs (the MatchInfo that carries the new round arrives, the dropped weapons are
// cleared, the new bodies' armour attaches), then until nothing is `busy` (the own respawn, the drop clear, the host's
// respawn queue), at most kMapResetGiveUpMs. A reset with the seq of the last one queued is a repeat.
constexpr uint32_t kMapResetDelayMs = 1500, kMapResetGiveUpMs = 5000;
class MapResetGate {
public:
    void Queue(const MapResetMsg& m, uint32_t nowMs) {
        if (any_ && m.seq == lastSeq_) return;
        any_ = true; lastSeq_ = m.seq;
        pending_ = true; msg_ = m; due_ = nowMs + kMapResetDelayMs; giveUp_ = nowMs + kMapResetGiveUpMs;
    }
    bool pending() const { return pending_; }
    // True once, with the reset to apply, when it is time.
    bool Poll(uint32_t nowMs, bool busy, MapResetMsg& out) {
        if (!pending_ || !Due(nowMs, due_) || (busy && !Due(nowMs, giveUp_))) return false;
        pending_ = false; out = msg_;
        return true;
    }
    void Reset() { *this = MapResetGate{}; }       // a new level or session: nothing queued, nothing remembered
    void Cancel() { pending_ = false; }
private:
    static bool Due(uint32_t now, uint32_t at) { return int32_t(now - at) >= 0; }
    bool any_ = false, pending_ = false;
    uint16_t lastSeq_ = 0;
    MapResetMsg msg_;
    uint32_t due_ = 0, giveUp_ = 0;
};

}  // namespace hsm
