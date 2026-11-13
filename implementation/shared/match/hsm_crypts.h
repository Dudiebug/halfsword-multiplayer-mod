// Crypts (#3199, match/hsm_match.h Gear::Crypts): the gather before the battle. When the host starts a Crypts match, every
// machine, the host's included, opens a fresh Crypts run of its own (the game's free run: one player, a new random character of the tier)
// and its player loots a kit there for the Gather time. Each machine reads its own fighter's kit (its live passport) every
// kKitSnapshotMs while it is alive and sends it to the host when it changed. A player who dies keeps the last kit read
// while alive; one who reaches an exit keeps the kit they came out with. Either way they wait in the main menu. When the
// clock runs out (or nobody is still in the Crypts) the host asks for the last kits (Collecting), waits at most
// kGatherCollectMs for them, then travels everyone to the battle map, where the format is fought in the gathered kits
// (RegearFor: dealt per slot when everybody starts, kept for every life). A rematch is a new gather.
//
// The dungeons are not shared: the game draws them from unseeded random numbers on each machine, so every player walks a
// different one. CryptsGather is the host's record of the gather, GatherClock every machine's view of its clock, and
// GatherSelf what this machine's own player is doing; the DLL keeps the kits' bytes and opens the levels.
#pragma once
#include "match/hsm_match.h"
#include <algorithm>
#include <cctype>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace hsm {

inline const std::vector<int> kGatherOptions = {300, 600, 900, 1200, 1500, 1800};   // the Gather time row, seconds
// The Gather time row's arrows: the next step above (or below) the time, wrapping, so a typed 12 min steps to 15 (or 10).
inline uint16_t StepGather(uint16_t cur, int dir) {
    if (dir > 0) { for (int o : kGatherOptions) if (o > cur) return uint16_t(o); return uint16_t(kGatherOptions.front()); }
    for (auto it = kGatherOptions.rbegin(); it != kGatherOptions.rend(); ++it) if (*it < cur) return uint16_t(*it);
    return uint16_t(kGatherOptions.back());
}
// A Gather time the host typed: whole minutes, an optional "min" or "m" and spaces around, kGatherMinS..kGatherMaxS.
// Returns seconds; nothing else parses (no sign, no decimals, no seconds).
inline std::optional<uint16_t> ParseGatherInput(const std::string& text) {
    size_t a = 0, b = text.size();
    auto space = [&](size_t i) { return std::isspace(static_cast<unsigned char>(text[i])) != 0; };
    while (a < b && space(a)) ++a;
    while (b > a && space(b - 1)) --b;
    auto endsWith = [&](const char* s) {
        const size_t n = std::char_traits<char>::length(s);
        if (b - a < n) return false;
        for (size_t i = 0; i < n; ++i) if (std::tolower(static_cast<unsigned char>(text[b - n + i])) != s[i]) return false;
        return true;
    };
    if (endsWith("min")) b -= 3; else if (endsWith("m")) b -= 1;
    while (b > a && space(b - 1)) --b;
    if (b - a > 2) return std::nullopt;                  // nothing left is 0 min: below the range
    int v = 0;
    for (size_t i = a; i < b; ++i) {
        if (!std::isdigit(static_cast<unsigned char>(text[i]))) return std::nullopt;
        v = v * 10 + (text[i] - '0');
    }
    if (v * 60 < kGatherMinS || v * 60 > kGatherMaxS) return std::nullopt;
    return uint16_t(v * 60);
}

// The Crypts' tier during a gather (Enum_Ranks: 0 Beggar, 1 Peasant, ... 8 Lord). A fresh run starts at Beggar, and the
// game raises the tier room by room (each room is its parent's tier plus its door's modifier, and the dungeon's director
// takes the tier of each room entered) and when it proceeds to the next rank. The higher tiers' enemies outrun what their
// loot gives a kit, so until the loot tables and the enemies' ranks can be changed a gather is held at Peasant: the
// director's tier and each room's are capped (`[mod] crypts_max_tier`, 0 to 8; 8 = the game's own tiers).
constexpr uint8_t kCryptsTierCap = 1;             // Peasant
constexpr uint8_t kCryptsTierTop = 8;             // Lord: no cap
inline uint8_t CryptsTierCapFrom(int ini) { return uint8_t(std::clamp(ini, 0, int(kCryptsTierTop))); }
// The tier to write back: `tier` held at `cap` (the game's own value when it is not above the cap).
inline uint8_t CappedCryptsTier(uint8_t tier, uint8_t cap) { return tier > cap ? cap : tier; }
constexpr uint32_t kGatherCollectMs = 10000;      // after the clock: the last kits come in for at most this long
constexpr uint32_t kGatherJoinMinMs = 60000;      // a player who joins during the gather opens a run only with a minute left
constexpr uint32_t kGatherClockEveryMs = 10000;   // the host sends the clock again this often (and to every joiner)
constexpr uint32_t kKitSnapshotMs = 5000;         // a gathering machine reads its fighter's kit this often while it is alive

enum class GatherPhase : uint8_t { Off = 0, Gathering = 1, Collecting = 2 };
inline constexpr uint8_t kGatherPhaseCount = 3;
// A player's word to the host about their own run (the Gather message, client -> host).
//   Loading    the run is opening: the level load takes a while, so the host waits longer before calling the player gone
//   InCrypts   the run is open and the player is looting
//   Died       the fighter died in the Crypts: the last kit sent is the one they keep
//   Extracted  the player reached an exit: the kit they came out with is sent first
//   Waiting    no run (joined too late, or the Crypts would not open): they wait for the battle in the menu
enum class GatherNote : uint8_t { Loading = 1, InCrypts = 2, Died = 3, Extracted = 4, Waiting = 5 };
inline bool ValidGatherNote(uint8_t n) { return n >= uint8_t(GatherNote::Loading) && n <= uint8_t(GatherNote::Waiting); }
// A note that ends the player's gather: nothing more is coming from their run.
inline bool GatherNoteEnds(GatherNote n) { return n == GatherNote::Died || n == GatherNote::Extracted || n == GatherNote::Waiting; }
inline const char* GatherNoteLog(GatherNote n) {
    switch (n) {
        case GatherNote::Loading: return "opening the Crypts";
        case GatherNote::InCrypts: return "in the Crypts";
        case GatherNote::Died: return "died - kit kept";
        case GatherNote::Extracted: return "exit reached - kit locked";
        case GatherNote::Waiting: return "waiting in the menu";
    }
    return "?";
}

enum class GatherStep : uint8_t { None, Collect, Battle };

// The host's record of a gather: who is in it, who is done, whose kit came in.
class CryptsGather {
public:
    struct Slot { bool kit = false, done = false, finalKit = false, inRun = false; };   // inRun: InCrypts seen

    void Start(uint32_t now, uint16_t gatherS, const std::vector<uint8_t>& slots) {
        phase_ = GatherPhase::Gathering;
        startMs_ = now; collectMs_ = 0;
        gatherMs_ = uint32_t(std::clamp(gatherS, kGatherMinS, kGatherMaxS)) * 1000u;
        if (++seq_ == 0) seq_ = 1;
        slots_.clear();
        for (uint8_t s : slots) slots_[s] = Slot{};
    }
    void Stop() { phase_ = GatherPhase::Off; slots_.clear(); }
    GatherPhase phase() const { return phase_; }
    bool Active() const { return phase_ != GatherPhase::Off; }
    uint16_t seq() const { return seq_; }

    // Gathering: the clock's time left; Collecting: the time left for the last kits; 0 otherwise.
    uint32_t LeftMs(uint32_t now) const {
        uint32_t end = 0;
        if (phase_ == GatherPhase::Gathering) end = startMs_ + gatherMs_;
        else if (phase_ == GatherPhase::Collecting) end = collectMs_ + kGatherCollectMs;
        else return 0;
        return now >= end ? 0 : end - now;
    }

    // A player joins during the gather. True: they open a run for the time left. False: too late (under kGatherJoinMinMs,
    // or the last kits are being collected): they wait in the menu and fight as a fresh character the host deals.
    bool Join(uint8_t slot, uint32_t now) {
        if (phase_ == GatherPhase::Off) return false;
        Slot& s = slots_[slot];
        const bool gathers = phase_ == GatherPhase::Gathering && LeftMs(now) >= kGatherJoinMinMs && !s.done;
        if (!gathers) s.done = true;
        return gathers;
    }
    void Leave(uint8_t slot) { slots_.erase(slot); }

    // Whether a kit from the slot is a gather kit: from a player in their Crypts run (InCrypts seen) whose run is not over.
    // Not one sent before the run (a CHANGE LOADOUT on the old level, the player's own save) or after it (a death, an exit, a
    // late join): an exit's last kit goes out before its note.
    bool TakesKit(uint8_t slot) const {
        auto it = slots_.find(slot);
        return phase_ != GatherPhase::Off && it != slots_.end() && it->second.inRun && !it->second.done;
    }
    // A kit stored for the slot (the DLL keeps its bytes; TakesKit first). One that comes while the last kits are collected is
    // the final one.
    void OnKit(uint8_t slot) {
        if (!TakesKit(slot)) return;
        auto it = slots_.find(slot);
        it->second.kit = true;
        if (phase_ == GatherPhase::Collecting) it->second.finalKit = true;
    }
    void OnNote(uint8_t slot, GatherNote n) {
        auto it = slots_.find(slot);
        if (it == slots_.end() || phase_ == GatherPhase::Off) return;
        if (n == GatherNote::InCrypts) it->second.inRun = true;
        if (GatherNoteEnds(n)) it->second.done = true;
    }

    bool In(uint8_t slot) const { return slots_.count(slot) != 0; }
    bool HasKit(uint8_t slot) const { auto it = slots_.find(slot); return it != slots_.end() && it->second.kit; }
    bool Done(uint8_t slot) const { auto it = slots_.find(slot); return it != slots_.end() && it->second.done; }
    int Players() const { return int(slots_.size()); }
    int Kits() const { int n = 0; for (const auto& [s, v] : slots_) n += v.kit ? 1 : 0; return n; }
    int StillGathering() const { int n = 0; for (const auto& [s, v] : slots_) n += v.done ? 0 : 1; return n; }

    // Collect: the clock ran out, or nobody is still in the Crypts (ask every machine for its last kit). Battle: the last
    // kits are in (everyone done or sent one since Collect), or their time is up: travel to the battle map. The gather is
    // over then (Off); the DLL keeps the kits for the match start.
    GatherStep Update(uint32_t now) {
        if (phase_ == GatherPhase::Gathering) {
            if (LeftMs(now) > 0 && StillGathering() > 0) return GatherStep::None;
            phase_ = GatherPhase::Collecting; collectMs_ = now;
            return GatherStep::Collect;
        }
        if (phase_ == GatherPhase::Collecting) {
            bool allIn = true;
            for (const auto& [s, v] : slots_) allIn = allIn && (v.done || v.finalKit);
            if (!allIn && LeftMs(now) > 0) return GatherStep::None;
            phase_ = GatherPhase::Off;
            return GatherStep::Battle;
        }
        return GatherStep::None;
    }

private:
    GatherPhase phase_ = GatherPhase::Off;
    uint32_t startMs_ = 0, gatherMs_ = 0, collectMs_ = 0;
    uint16_t seq_ = 0;
    std::map<uint8_t, Slot> slots_;
};

// Every machine's view of the host's clock, from its Gather messages; it runs on between them.
class GatherClock {
public:
    void Set(uint32_t now, GatherPhase p, uint32_t leftMs, uint16_t seq) { phase_ = p; leftMs_ = leftMs; atMs_ = now; seq_ = seq; }
    void Clear() { phase_ = GatherPhase::Off; leftMs_ = 0; }
    GatherPhase phase() const { return phase_; }
    bool Active() const { return phase_ != GatherPhase::Off; }
    uint16_t seq() const { return seq_; }
    uint32_t LeftMs(uint32_t now) const {
        if (phase_ == GatherPhase::Off) return 0;
        const uint32_t ran = now - atMs_;
        return ran >= leftMs_ ? 0 : leftMs_ - ran;
    }

private:
    GatherPhase phase_ = GatherPhase::Off;
    uint32_t leftMs_ = 0, atMs_ = 0;
    uint16_t seq_ = 0;
};

// What this machine's own player is doing in the gather.
enum class GatherSelf : uint8_t { None, Opening, Gathering, Died, Extracted, Waiting };
// Whether the machine reads its fighter's kit now: only while it loots, alive, and kKitSnapshotMs after the last read.
inline bool KitSnapshotDue(GatherSelf self, bool alive, uint32_t now, uint32_t lastMs, bool never) {
    return self == GatherSelf::Gathering && alive && (never || now - lastMs >= kKitSnapshotMs);
}
// A read kit goes to the host when it differs from the last one sent.
inline bool KitUploadDue(const std::vector<uint8_t>& kit, const std::vector<uint8_t>& lastSent) { return !kit.empty() && kit != lastSent; }

}  // namespace hsm
