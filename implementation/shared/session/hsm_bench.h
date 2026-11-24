// A guest's BACK TO LOBBY (protocol rev 49): the guest leaves the match but keeps the Steam lobby and the host connection, and
// waits in the lobby for the host's next match. The host cannot treat it as a leaver (the connection stays) nor as a fighter in
// the menu (a frozen pose nobody can kill, so a last-one-standing round never resolves), so it benches the slot:
//   - the fighter falls as a leaver's does (LeaverFalls), its body goes from every machine (Despawn), its hand-off leases are
//     freed and its held input is let go;
//   - it gets no body again (SpawnDue, a rematch, a round reset, a map reload all skip it) and the round resolution leaves it out;
//   - the scoreboard shows it as in the lobby (FighterEntry bit 2), not as dead;
//   - the host's next Travel (a START, a rematch on a new map, BACK TO LOBBY, a map reload) un-benches it: the bench belongs to
//     the travel it was made in (SlotBench keys it by the host's travel seq), so nothing has to be cleaned up when it ends;
//   - leaving the lobby while benched frees the slot at once, once: the fall, the body and the leases are already done
//     (SlotBench::Leave), so only the roster, the score and the "left" line remain.
// The wire: BackToLobby 72, client -> host, reliable, 4 bytes after the header: the host's travel seq the guest is leaving. A
// stale one (the host travelled since) is ignored; the guest is in the menu level by then and the Travel pulls it back.
// Game-independent and unit-tested (session/test_hsm_bench.cpp); gameplay/session.cpp carries it out.
#pragma once
#include "protocol/hsm_protocol.h"
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace hsm {

struct BackToLobbyMsg { uint32_t seq = 0; };   // the host's travel seq the guest leaves; never 0 (0: no travel yet)

inline std::vector<uint8_t> Encode(const BackToLobbyMsg& m) {
    Writer w; w.header(MsgType::BackToLobby); w.u32(m.seq); return w.buf;
}
inline bool Decode(const uint8_t* d, size_t n, BackToLobbyMsg& m) {
    Reader r(d, n); MsgType t; if (!r.header(t) || t != MsgType::BackToLobby) return false;
    const uint32_t seq = r.u32();
    if (!r.done() || seq == 0) return false;
    m.seq = seq;
    return true;
}

// What the host does with a BackToLobby from `slot` (a client's, never 0).
enum class BenchVerdict : uint8_t { Bench, Duplicate, NotHost, NoMatch, HostSlot, Gather, Progression, Stale };
inline BenchVerdict BenchVerdictOf(bool isHost, bool matchLevel, uint8_t slot, bool gatherOn, bool progression, uint32_t msgSeq, uint32_t hostSeq,
                                   bool alreadyBenched) {
    if (!isHost) return BenchVerdict::NotHost;
    if (slot == 0) return BenchVerdict::HostSlot;          // the host's own BACK TO LOBBY is a Travel, not a bench
    if (!matchLevel) return BenchVerdict::NoMatch;         // the guest was in the menu already
    if (gatherOn) return BenchVerdict::Gather;             // a gather goes on to its battle
    if (progression) return BenchVerdict::Progression;     // the Progression co-op fight is one shared run
    if (msgSeq != hostSeq) return BenchVerdict::Stale;     // the host travelled since: the guest is pulled along already
    if (alreadyBenched) return BenchVerdict::Duplicate;
    return BenchVerdict::Bench;
}
inline const char* BenchVerdictText(BenchVerdict v) {
    switch (v) {
        case BenchVerdict::Bench: return "benched";
        case BenchVerdict::Duplicate: return "already benched";
        case BenchVerdict::NotHost: return "this machine is no host";
        case BenchVerdict::NoMatch: return "no match is on";
        case BenchVerdict::HostSlot: return "the host's own slot";
        case BenchVerdict::Gather: return "a Crypts run is on";
        case BenchVerdict::Progression: return "a Progression fight is on";
        default: return "from an earlier travel";
    }
}

// The benched slots (entity numbers), each with the host's travel seq of the bench. A slot is benched only while the host's
// travel seq is still that one: the next Travel un-benches it without anyone having to remember.
class SlotBench {
public:
    // False: already benched in this travel (nothing to do again).
    bool Bench(uint8_t entity, uint32_t seq) {
        auto [it, fresh] = seq_.try_emplace(entity, seq);
        if (fresh) return true;
        if (it->second == seq) return false;
        it->second = seq;
        return true;
    }
    bool Benched(uint8_t entity, uint32_t hostSeq) const {
        auto it = seq_.find(entity);
        return it != seq_.end() && it->second == hostSeq;
    }
    // The entities benched in the host's current travel.
    std::vector<uint8_t> Active(uint32_t hostSeq) const {
        std::vector<uint8_t> out;
        for (const auto& [e, s] : seq_) if (s == hostSeq) out.push_back(e);
        return out;
    }
    // The host travelled: the entries of an earlier travel are over. Returned (and forgotten) so the caller can put the match's
    // own record of them right (Match::Return).
    std::vector<uint8_t> Lifted(uint32_t hostSeq) {
        std::vector<uint8_t> out;
        for (auto it = seq_.begin(); it != seq_.end();) {
            if (it->second != hostSeq) { out.push_back(it->first); it = seq_.erase(it); }
            else ++it;
        }
        return out;
    }
    // The slot leaves the session. True: it was benched in this travel, so its fall, body, leases and held input are done
    // already and only the roster, score and "left" line remain. Either way the entry is gone.
    bool Leave(uint8_t entity, uint32_t hostSeq) {
        auto it = seq_.find(entity);
        if (it == seq_.end()) return false;
        const bool current = it->second == hostSeq;
        seq_.erase(it);
        return current;
    }
    void Clear() { seq_.clear(); }
    bool Any() const { return !seq_.empty(); }
private:
    std::map<uint8_t, uint32_t> seq_;
};

// May the host give this entity a body now (SpawnDue, RespawnEntity), and does the start wait for it (the idle match's allIn)?
// Not while it is benched in the host's current travel; the next travel gives it one again.
inline bool BodyAllowed(const SlotBench& b, uint8_t entity, uint32_t hostSeq) { return !b.Benched(entity, hostSeq); }

// The benched fighters the match does not count as gone yet: a rematch clears the match's own note of its leavers, and Match::Leave
// works only in a fought phase, so the host asks again every tick. Each one that is returned is to Leave: the round is resolved
// without it (a Duel round goes to whoever is left) and, with every other human gone too, the match ends as when they leave.
// `m` is the Match (its Gone(entity)).
template <class MatchT>
inline std::vector<uint8_t> BenchedToLeave(const SlotBench& b, uint32_t hostSeq, const MatchT& m) {
    std::vector<uint8_t> out;
    for (uint8_t e : b.Active(hostSeq)) if (!m.Gone(e)) out.push_back(e);
    return out;
}

// Why BACK TO LOBBY is dimmed on this machine, in the order the panel tells it. The host's button is a Travel to the menu level;
// a guest's is the bench.
//   NoLobby       no Steam lobby is kept (a direct connection): there is no lobby to wait in
//   Crypts        a Crypts gather is on (Apart or Together): it goes on to its battle, so no lobby comes of a click
//   Progression   a guest in a Progression co-op fight: the run is the host's, and a guest back in the menu cannot be benched there
enum class BackBlock : uint8_t { None, NoLobby, Crypts, Progression };
inline BackBlock BackToLobbyBlock(bool host, bool lobbyKept, bool cryptsRun, bool progression) {
    if (!lobbyKept) return BackBlock::NoLobby;
    if (cryptsRun) return BackBlock::Crypts;
    if (!host && progression) return BackBlock::Progression;
    return BackBlock::None;
}

// The fight level a new host opens again after a migration. A machine in a fight level reopens that level (never a Crypts run);
// one that went back to the lobby is in the menu level and reopens the level it left; any other menu: nothing.
inline std::string MigrationReopenLevel(bool inFight, bool soloLevel, const std::string& currentLevel, const std::string& benchedFrom) {
    if (inFight) return soloLevel ? std::string() : currentLevel;
    return benchedFrom;
}

}  // namespace hsm
