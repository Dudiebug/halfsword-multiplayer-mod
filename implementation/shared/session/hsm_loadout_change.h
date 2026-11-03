// CHANGE LOADOUT in the Esc menu (0.9.0, protocol rev 36): a player who edited their kit in the game's own menu brings it
// into the match without leaving it. Own gear only, and only out of the fight; the fighter comes back in the new kit with no
// score or death change.
//
// Rule (CanChangeLoadout), decided by the host and shown to the player by the same function:
//   gear is not Own (Random by tier, Mirror, Fists)        refused: those loadouts are dealt by the host
//   a fought phase, the fighter alive, not in a round gap  refused: "Change loadout when you are out of the fight"
//   a fought phase, the fighter down for the round         accepted; the kit is used when it comes back (its respawn)
//   a round gap / wave break                               accepted; the fighter respawns now
//   countdown, results, no match yet                       accepted; the fighter respawns now
//
// Wire: LoadoutChange 61 (client -> host, reliable): the new passport as BlobChunks, wrapped the way BlobChunk 13 is (the same
// body after the header, the same validation, the same 256 KiB and 8 KiB chunk caps) but kept apart from the upload at join
// so that nothing is stored before the rule has been checked. The host assembles the chunks per peer, checks the passport
// like any upload (CheckPassportSave), checks the rule, and stores and relays it as the slot's loadout. One change is
// accepted per kLoadoutChangeGapMs per player. The host answers with a LoadoutChangeResult (the same type, host -> client, one
// verdict byte) before it respawns the fighter, so the client's own body respawns in the new kit only when it was accepted.
// Game-independent and unit-tested.
#pragma once
#include "hsm_protocol.h"
#include "match/hsm_match.h"
#include <cstdint>
#include <vector>

namespace hsm {

constexpr uint32_t kLoadoutChangeGapMs = 5000;

// The first four come from the rule; TooSoon (the rate limit) and BadBlob (the passport failed its check) from the host's other checks.
enum class LoadoutVerdict : uint8_t { RespawnNow, AtNextRespawn, NotOwnGear, InFight, TooSoon, BadBlob };
constexpr uint8_t kLoadoutVerdictCount = 6;
inline bool LoadoutAllowed(LoadoutVerdict v) { return v == LoadoutVerdict::RespawnNow || v == LoadoutVerdict::AtNextRespawn; }

// alive: the fighter has a living body in the match (a fighter not in the match yet counts alive); roundGap: a Duel / Last Man
// Standing round gap or a bot formats' wave break (nobody fights); awaitsRespawn: the match holds the fighter down (it left, or
// was benched), so it comes back at the match's own respawn.
inline LoadoutVerdict CanChangeLoadout(Gear gear, MatchPhase phase, bool alive, bool roundGap, bool awaitsRespawn = false) {
    if (gear != Gear::Own) return LoadoutVerdict::NotOwnGear;
    if (!alive || awaitsRespawn) return LoadoutVerdict::AtNextRespawn;
    if (CombatPhase(phase) && !roundGap) return LoadoutVerdict::InFight;
    return LoadoutVerdict::RespawnNow;
}

inline const char* LoadoutVerdictText(LoadoutVerdict v) {
    switch (v) {
        case LoadoutVerdict::NotOwnGear: return "Own loadouts only: the host deals the gear in this match";
        case LoadoutVerdict::InFight: return "Change loadout when you are out of the fight";
        case LoadoutVerdict::TooSoon: return "Wait a few seconds before changing your loadout again";
        case LoadoutVerdict::BadBlob: return "The host could not use that loadout";
        case LoadoutVerdict::AtNextRespawn: return "Loadout changed: you come back in it";
        default: return "Loadout changed";
    }
}
inline const char* LoadoutVerdictLog(LoadoutVerdict v) {
    switch (v) {
        case LoadoutVerdict::NotOwnGear: return "gear is not own";
        case LoadoutVerdict::InFight: return "in the fight";
        case LoadoutVerdict::TooSoon: return "too soon after the last change";
        case LoadoutVerdict::BadBlob: return "the passport failed its check";
        case LoadoutVerdict::AtNextRespawn: return "at the next respawn";
        default: return "respawn now";
    }
}

// One change per kLoadoutChangeGapMs (wrap-safe tick counts). A refused change does not start the wait.
class LoadoutChangeLimiter {
public:
    bool Allow(uint32_t nowMs) const { return !started_ || uint32_t(nowMs - last_) >= kLoadoutChangeGapMs; }
    void Used(uint32_t nowMs) { started_ = true; last_ = nowMs; }
    uint32_t WaitMs(uint32_t nowMs) const { return Allow(nowMs) ? 0 : kLoadoutChangeGapMs - uint32_t(nowMs - last_); }
private:
    bool started_ = false;
    uint32_t last_ = 0;
};

struct LoadoutChangeMsg { BlobChunk chunk; };
// The host's answer, host -> client under the same type: what became of the change. Accepted (RespawnNow, AtNextRespawn) means
// the host stored the passport and relayed it; the client then keeps it as the loadout its own body respawns in.
struct LoadoutChangeResult { LoadoutVerdict verdict = LoadoutVerdict::InFight; };

// The BlobChunk body under the LoadoutChange type byte (header: magic u16, revision u8, type u8).
inline std::vector<uint8_t> Encode(const LoadoutChangeMsg& m) {
    std::vector<uint8_t> b = Encode(m.chunk);
    b[3] = uint8_t(MsgType::LoadoutChange);
    return b;
}
inline std::vector<uint8_t> Encode(const LoadoutChangeResult& m) {
    Writer w; w.header(MsgType::LoadoutChange); w.u8(uint8_t(m.verdict)); return w.buf;
}
inline bool Decode(const uint8_t* d, size_t n, LoadoutChangeResult& m) {
    Reader r(d, n); MsgType t; if (!r.header(t) || t != MsgType::LoadoutChange) return false;
    const uint8_t v = r.u8(); if (v >= kLoadoutVerdictCount) return false;
    m.verdict = LoadoutVerdict(v); return r.done();
}
inline bool Decode(const uint8_t* d, size_t n, LoadoutChangeMsg& m) {
    if (n < 4 || d[3] != uint8_t(MsgType::LoadoutChange)) return false;
    std::vector<uint8_t> b(d, d + n);
    b[3] = uint8_t(MsgType::BlobChunk);
    return Decode(b.data(), b.size(), m.chunk);
}

}  // namespace hsm
