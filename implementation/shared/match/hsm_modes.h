// Wave 3 modes: Tag, Infection and Crossbow TDM. Game-independent rules; the host's session executes what these decide
// (gameplay/session.cpp) and the clients' HUD reads the MatchInfo they end up in (hsm_hud_text.h, hsm_hud_scene.h).
//
//   Tag          Duel's rounds for any number of fighters. One living fighter is "it" and carries a fuse (the Fuse time row).
//                A landed hit by "it" on a living fighter passes the fuse on: it keeps burning (a hot potato), but never has less
//                than kTagPassFloorMs left after a pass. The fighter who just passed cannot be tagged back for kTagBackMs.
//                When the fuse runs out "it" is out for the round; a new "it" is picked among the living with a full fuse
//                (also when "it" is killed or leaves). The last one standing takes the round (Match, IsRoundFormat).
//   Infection    Survivors (team 0) against the infected (team 1). 1 or 2 patient zeros start infected, rotating each rematch.
//                A survivor killed by an infected fighter is infected and respawns (the Respawn row); a survivor killed by anything
//                else is out for the match; the infected win when no survivor is left, the survivors when the time limit finds
//                one standing (Match). A fighter joining a running match is infected.
//   Crossbow TDM Teams where everyone has a crossbow. Without the crossbow classes it plays as plain Teams.
#pragma once
#include "match/hsm_match.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace hsm {

namespace modes_detail {
inline uint32_t Mix(uint32_t x) {                        // murmur3 finaliser
    x ^= x >> 16; x *= 0x85EBCA6Bu; x ^= x >> 13; x *= 0xC2B2AE35u; x ^= x >> 16;
    return x;
}
// Sorted, each once.
inline std::vector<uint8_t> Ids(std::vector<uint8_t> v) {
    std::sort(v.begin(), v.end());
    v.erase(std::unique(v.begin(), v.end()), v.end());
    return v;
}
}  // namespace modes_detail

// ---------------------------------------------------------------- Tag
constexpr uint32_t kTagBackMs = 1500;        // the fighter who just passed the fuse cannot be tagged back for this long
constexpr uint32_t kTagPassFloorMs = 3000;   // a pass never leaves less than this on the fuse
constexpr uint32_t kTagMinFighters = 2;      // "it" needs somebody to chase

inline uint32_t FuseMs(uint8_t fuseS) { return uint32_t(std::clamp<int>(fuseS, kFuseMinS, kFuseMaxS)) * 1000u; }

// The fighter picked as "it" among `living`: the same ids, seed and step always give the same one, and over n consecutive steps
// with the same n fighters everybody has a turn (0 when there is nobody).
inline uint8_t PickIt(std::vector<uint8_t> living, uint32_t seed, uint32_t step) {
    living = modes_detail::Ids(std::move(living));
    if (living.empty()) return 0;
    return living[(size_t(modes_detail::Mix(seed)) + step) % living.size()];
}

class TagGame {
public:
    struct Step { uint8_t burned = 0, picked = 0; };    // Update: whose fuse ran out (0: none), and the new "it" picked (0: none)

    // A new match: forget everything; the seed starts the rotation.
    void Reset(uint32_t seed, uint8_t fuseS) { seed_ = seed; step_ = 0; fuseMs_ = FuseMs(fuseS); EndRound(); }
    // A round starts (the match going live, a round reset): "it" is picked among the living with a full fuse.
    void BeginRound(const std::vector<uint8_t>& living, uint32_t nowMs) {
        active_ = true; it_ = prev_ = 0; living_ = Set(living);
        PickAmongLiving(nowMs);
    }
    // The round is over (won, drawn, or the gap): nobody is "it".
    void EndRound() { active_ = false; it_ = prev_ = 0; living_.clear(); }
    bool active() const { return active_; }
    uint8_t it() const { return it_; }
    uint32_t FuseLeftMs(uint32_t nowMs) const {
        if (!it_) return 0;
        const int32_t left = int32_t(fuseEnd_ - nowMs);
        return left > 0 ? uint32_t(left) : 0;
    }

    // Each tick of a round in play, with every fighter standing. "it" gone (killed, left) is replaced with a full fuse; the fuse
    // running out burns "it" (the caller eliminates it) and picks the next among the rest. One fighter standing is nobody's chase.
    Step Update(const std::vector<uint8_t>& living, uint32_t nowMs) {
        Step s;
        if (!active_) return s;
        living_ = Set(living);
        if (living_.size() < kTagMinFighters) { it_ = 0; return s; }
        if (!it_ || !living_.count(it_)) { PickAmongLiving(nowMs); s.picked = it_; return s; }
        if (int32_t(nowMs - fuseEnd_) >= 0) {
            s.burned = it_;
            living_.erase(it_); it_ = prev_ = 0;
            if (living_.size() >= kTagMinFighters) { PickAmongLiving(nowMs); s.picked = it_; }
        }
        return s;
    }

    // A landed hit of `attacker` on `victim`. True when the fuse was passed: only "it" passes, only to a living fighter other than
    // itself, and not back to the fighter who just passed it on (kTagBackMs).
    bool OnHit(uint8_t attacker, uint8_t victim, uint32_t nowMs) {
        if (!active_ || !it_ || attacker != it_ || victim == attacker || !living_.count(victim)) return false;
        if (victim == prev_ && nowMs - passedAt_ < kTagBackMs) return false;
        prev_ = attacker; it_ = victim; passedAt_ = nowMs;
        if (int32_t(fuseEnd_ - (nowMs + kTagPassFloorMs)) < 0) fuseEnd_ = nowMs + kTagPassFloorMs;
        return true;
    }

private:
    static std::set<uint8_t> Set(const std::vector<uint8_t>& v) { return {v.begin(), v.end()}; }
    void PickAmongLiving(uint32_t nowMs) {
        prev_ = 0;
        if (living_.size() < kTagMinFighters) { it_ = 0; return; }
        it_ = PickIt(std::vector<uint8_t>(living_.begin(), living_.end()), seed_, step_++);
        fuseEnd_ = nowMs + fuseMs_;
    }
    bool active_ = false;
    uint32_t seed_ = 0, step_ = 0, fuseMs_ = 30000, fuseEnd_ = 0, passedAt_ = 0;
    uint8_t it_ = 0, prev_ = 0;
    std::set<uint8_t> living_;
};

// ---------------------------------------------------------------- Infection
// Patient zeros for `players` fighters: 2..5 one, 6 or more two; never all of them, none for a lone fighter.
inline int PatientZeroCount(int players) {
    if (players < 2) return 0;
    return std::min(players >= 6 ? 2 : 1, players - 1);
}

// The teams for an Infection match's fighters (each id once): the patient zeros are infected (kInfectedTeam), the rest survive.
// The zeros are PatientZeroCount fighters in a row by id from a seed-picked place, moving on that many at every rematch so
// everybody gets a turn. The same arguments always deal the same teams.
inline std::map<uint8_t, int8_t> AssignInfection(const std::vector<uint8_t>& fighters, uint32_t seed, uint32_t rematch = 0) {
    const std::vector<uint8_t> ids = modes_detail::Ids(fighters);
    std::map<uint8_t, int8_t> out;
    const size_t n = ids.size(), zeros = size_t(PatientZeroCount(int(n)));
    const size_t first = n ? (modes_detail::Mix(seed) + size_t(rematch) * std::max<size_t>(zeros, 1)) % n : 0;
    for (size_t i = 0; i < n; ++i) out[ids[i]] = (i + n - first) % n < zeros ? kInfectedTeam : kSurvivorTeam;
    return out;
}
// A fighter joining a running Infection match is infected: the survivors are a shrinking pool, and a newcomer is never one of them.
inline int8_t InfectionLateJoinerTeam() { return kInfectedTeam; }

// Who becomes infected: a survivor killed by an infected fighter (not by a survivor, itself or the world).
inline bool InfectsOnKill(int8_t killerTeam, int8_t victimTeam, bool killerIsVictim) {
    return !killerIsVictim && killerTeam == kInfectedTeam && victimTeam == kSurvivorTeam;
}

// What an infected player fighter is made of (unverified in game: the names come from the reflection index). The session sets the
// zombie flag, zeroes the pain fields every tick (as the stand-ins' are, gameplay/standin_rig.cpp) and keeps Consciousness from
// falling under half of what it was at the spawn. Every field is optional: what the Willie lacks is skipped and logged.
inline constexpr const char* kInfectedZombieFlag = "Is Zombie?";
inline constexpr const char* kInfectedPainDoubles[] = {"Pain Shock Rate", "Pain Upper Body", "Pain Lower Body", "Pain Neck", "Pain Head", "Pain Arm R",
                                                       "Pain Arm L", "Pain Leg R", "Pain Leg L", "Pain Grab Rate", "Pain L Arm Alpha", "Pain R Arm Alpha"};
inline constexpr const char* kInfectedPainBools[] = {"Pain Shock"};   // forced false
inline constexpr const char* kInfectedConsciousnessFields[] = {"Consciousness"};
inline constexpr double kInfectedConsciousnessKeep = 0.5;
// The lowest Consciousness an infected fighter keeps, from the value read at its spawn; a baseline that is no number or not above 0
// gives no floor (NaN: nothing to hold).
inline double InfectedConsciousnessFloor(double baseline) {
    return std::isfinite(baseline) && baseline > 0.0 ? baseline * kInfectedConsciousnessKeep : std::nan("");
}
// `current` raised to the floor; a current that is no number, or no floor, leaves it as it is.
inline double InfectedConsciousness(double current, double floor) {
    return std::isfinite(current) && std::isfinite(floor) && current < floor ? floor : current;
}
// The capability line the session logs once per infected fighter. consciousness: the field found, empty when none.
inline std::string InfectedTraitLine(const std::string& name, bool zombieFlag, int painFields, const std::string& consciousness) {
    return "infection: zombie traits on " + name + ": Is Zombie? " + (zombieFlag ? "yes" : "missing") + ", pain fields " + std::to_string(painFields) +
           ", consciousness " + (consciousness.empty() ? std::string("missing") : consciousness);
}

// ---------------------------------------------------------------- Crossbow TDM
// Crossbow TDM hands everyone a crossbow when the game has the classes; without them it is Teams, and the host says so once.
inline constexpr const char* kCrossbowTdmMissingLine = "crossbow: Crossbow TDM without crossbows (class missing)";
inline constexpr const char* kCrossbowRowMissingLine = "crossbow: the Crossbows row has no effect in this match (the crossbow or its bolt class is missing)";
inline uint8_t EffectiveCrossbows(const MatchConfig& c, bool classesPresent) {
    if (IsCrossbowTdm(c.format)) return classesPresent ? uint8_t(Crossbows::All) : uint8_t(Crossbows::Off);
    return c.crossbows;
}
inline const char* CrossbowMissingLine(Format f) { return IsCrossbowTdm(f) ? kCrossbowTdmMissingLine : kCrossbowRowMissingLine; }
// A fighter that joins mid-match (or comes back from a fall) in Crossbow TDM still gets one.
inline bool CrossbowForEveryone(Format f, bool classesPresent) { return IsCrossbowTdm(f) && classesPresent; }

}  // namespace hsm
