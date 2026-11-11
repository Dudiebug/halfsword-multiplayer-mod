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

// The Tagger speed row (#1408): the multiplier on a fighter's run and walk speed in a Tag match. Whoever is "it" in a round in play
// (`it` is that fighter's entity, 0 when no round runs) gets the row's percent; everyone else, "it" with the row on 100, and every
// other format get 1.0, the game's own speed. Passing the fuse moves `it`, so the bonus goes with it. The machine that runs the
// tagger's body applies it (gameplay/session.cpp, through the class stats).
inline double TaggerSpeedMultiplier(Format f, uint8_t it, uint8_t entity, uint8_t speedPct) {
    return IsTag(f) && it != 0 && entity == it && ValidTaggerSpeedPct(speedPct) ? double(speedPct) / 100.0 : 1.0;
}

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
inline constexpr const char* kOneCrossbowMissingLine = "crossbow: One Crossbow without its crossbow (class missing): the rounds are fist fights";
inline uint8_t EffectiveCrossbows(const MatchConfig& c, bool classesPresent) {
    if (IsCrossbowTdm(c.format)) return classesPresent ? uint8_t(Crossbows::All) : uint8_t(Crossbows::Off);
    if (IsOneCrossbow(c.format)) return classesPresent ? uint8_t(Crossbows::One) : uint8_t(Crossbows::Off);
    return c.crossbows;
}
inline const char* CrossbowMissingLine(Format f) { return IsCrossbowTdm(f) ? kCrossbowTdmMissingLine : IsOneCrossbow(f) ? kOneCrossbowMissingLine : kCrossbowRowMissingLine; }

// ---------------------------------------------------------------- One Crossbow
// Endless bolts: in One Crossbow whoever holds the crossbow wears a quiver that never runs out. Each machine keeps its own
// fighter's: a quiver under kEndlessBoltFloor is filled back up to `full` (so a reload always finds a bolt), any other
// count is left alone. Nothing happens in another format or in a hand without the crossbow.
inline constexpr int kEndlessBoltFloor = 2;
inline bool EndlessBoltsFor(Format f, bool matchRuns, bool alive, bool holdsCrossbow) { return IsOneCrossbow(f) && matchRuns && alive && holdsCrossbow; }
inline int EndlessBolts(int inQuiver, int full) { return inQuiver < kEndlessBoltFloor && full > inQuiver ? full : inQuiver; }

// ---------------------------------------------------------------- Roulette
// Roulette (#836): the host hands one crossbow from fighter to fighter. It is a six-chamber cylinder with one live
// chamber: every pull moves on to the next chamber, so the odds rise from 1 in 6 until it fires, and then it is spun
// again. The holder pulls on themself or on another living fighter:
//   a blank on yourself: a point (the caller's), and the crossbow stays;
//   a blank on someone else: the crossbow is theirs;
//   the live chamber on yourself: out for the round (the caller's), and the next fighter holds the crossbow;
//   the live chamber on someone else: the crossbow is loaded for real. Once the shot is fired (OnFired) and has had
//   kRouletteLandMs to land, or was not fired for kRouletteShotMs, the crossbow goes to the fighter it was pulled on,
//   or to the next one when that fighter died.
// A holder that died or left hands on to the next fighter in id order. Fewer than two standing: nobody holds it.
constexpr uint8_t kRouletteChambers = 6;
constexpr uint32_t kRoulettePullGapMs = 800;    // between two pulls: a held key does not empty the cylinder
constexpr uint32_t kRouletteShotMs = 20000;     // a live round not shot by then is wasted
constexpr uint32_t kRouletteLandMs = 1500;      // after the shot the bolt lands before the crossbow moves on

class RouletteGame {
public:
    enum class Pull : uint8_t { Refused, BlankSelf, LiveSelf, BlankOther, LiveOther };
    struct Step { bool moved = false, wasted = false; };   // Update: the crossbow changed hands; a live round was never shot

    // A new match: forget everything; the seed starts the rotation of first holders and the cylinder.
    void Reset(uint32_t seed) { seed_ = seed; spins_ = rounds_ = 0; EndRound(); }
    // A round starts: the first holder is picked among the living (everybody's turn comes) and the cylinder is spun.
    void BeginRound(const std::vector<uint8_t>& living, uint32_t nowMs) {
        active_ = true; living_ = Set(living); loaded_ = false; nextPullAt_ = nowMs;
        holder_ = living_.size() >= 2 ? PickIt(living, seed_, rounds_++) : 0;
        Spin();
    }
    void EndRound() { active_ = false; holder_ = 0; loaded_ = false; }
    bool active() const { return active_; }
    uint8_t holder() const { return holder_; }
    bool loaded() const { return loaded_; }                // a live chamber came up on someone else: the holder has a real bolt
    uint8_t target() const { return loaded_ ? target_ : 0; }
    uint8_t pulls() const { return pulls_; }               // blanks since the spin: the next pull is live one time in (6 - pulls)

    Pull OnPull(uint8_t who, uint8_t target, uint32_t nowMs) {
        if (!holder_ || who != holder_ || loaded_ || int32_t(nowMs - nextPullAt_) < 0) return Pull::Refused;   // no round: no holder
        const bool self = target == who;
        if (!self && !living_.count(target)) return Pull::Refused;
        nextPullAt_ = nowMs + kRoulettePullGapMs;
        const bool live = pulls_ == live_;
        if (live) Spin(); else ++pulls_;
        if (self) {
            if (!live) return Pull::BlankSelf;
            living_.erase(who);
            holder_ = living_.size() >= 2 ? NextAfter(who) : 0;
            return Pull::LiveSelf;
        }
        if (!live) { holder_ = target; return Pull::BlankOther; }
        loaded_ = true; fired_ = false; target_ = target; passAt_ = nowMs + kRouletteShotMs;
        return Pull::LiveOther;
    }
    // The holder's live round left the crossbow. True the first time.
    bool OnFired(uint8_t who, uint32_t nowMs) {
        if (!loaded_ || fired_ || who != holder_) return false;
        fired_ = true; passAt_ = nowMs + kRouletteLandMs;
        return true;
    }
    // Each tick of a round in play, with every fighter standing.
    Step Update(const std::vector<uint8_t>& living, uint32_t nowMs) {
        Step s;
        if (!active_) return s;
        living_ = Set(living);
        const uint8_t before = holder_;
        if (living_.size() < 2) { holder_ = 0; loaded_ = false; }
        else if (!living_.count(holder_)) { holder_ = NextAfter(before); loaded_ = false; }   // gone (or nobody held it)
        else if (loaded_ && int32_t(nowMs - passAt_) >= 0) {
            s.wasted = !fired_;
            loaded_ = false;
            holder_ = living_.count(target_) ? target_ : NextAfter(before);
        }
        s.moved = holder_ != before;
        return s;
    }

private:
    static std::set<uint8_t> Set(const std::vector<uint8_t>& v) { return {v.begin(), v.end()}; }
    void Spin() { pulls_ = 0; live_ = uint8_t(modes_detail::Mix(seed_ + 0x9E3779B9u * ++spins_) % kRouletteChambers); }
    uint8_t NextAfter(uint8_t e) const {                   // the next living fighter in id order, round the ring
        if (living_.empty()) return 0;
        const auto it = living_.upper_bound(e);
        return it != living_.end() ? *it : *living_.begin();
    }
    bool active_ = false, loaded_ = false, fired_ = false;
    uint8_t holder_ = 0, target_ = 0, pulls_ = 0, live_ = 0;
    uint32_t seed_ = 0, spins_ = 0, rounds_ = 0, nextPullAt_ = 0, passAt_ = 0;
    std::set<uint8_t> living_;
};

// What every machine shows of the cylinder, kept from the host's mode events (protocol ModeEventKind RouletteClick and
// RouletteBang) and the MatchInfo: the blanks since the spin, and whether the holder has a live round. A new round
// starts with a fresh spin; the crossbow in another hand has no live round.
struct RouletteView {
    uint8_t pulls = 0; bool loaded = false;
    uint16_t round = 0; uint8_t holder = 0;
    void OnClick() { if (pulls + 1 < kRouletteChambers) ++pulls; }
    void OnBang(bool onThemself) { pulls = 0; loaded = !onThemself; }
    void OnInfo(uint16_t infoRound, uint8_t infoHolder) {
        if (infoRound != round) { pulls = 0; loaded = false; }
        if (infoHolder != holder) loaded = false;
        round = infoRound; holder = infoHolder;
    }
    int OneIn() const { return int(kRouletteChambers) - int(pulls); }
};

// Who the holder faces (the C key): `eye` is the camera and `body` the holder, so ahead is from the camera through the
// body, on the ground plane. Of the marks within reach and within the cone, the one closest to straight ahead; 0 when
// there is none, or when the camera sits on the body and ahead is unknown.
struct AimMark { uint8_t entity = 0; double x = 0, y = 0; };
constexpr double kRouletteReachCm = 3000.0;
constexpr double kRouletteAimCos = 0.9;         // within about 25 degrees of straight ahead
inline uint8_t FacedFighter(double eyeX, double eyeY, double bodyX, double bodyY, const std::vector<AimMark>& marks) {
    const double fx = bodyX - eyeX, fy = bodyY - eyeY, fl = std::sqrt(fx * fx + fy * fy);
    if (fl < 1.0) return 0;
    uint8_t best = 0; double bestCos = kRouletteAimCos;
    for (const AimMark& k : marks) {
        const double vx = k.x - bodyX, vy = k.y - bodyY, vl = std::sqrt(vx * vx + vy * vy);
        if (!k.entity || vl < 1.0 || vl > kRouletteReachCm) continue;
        const double c = (fx * vx + fy * vy) / (fl * vl);
        if (best ? c > bestCos : c >= bestCos) { best = k.entity; bestCos = c; }
    }
    return best;
}

// ---------------------------------------------------------------- Hide and Seek
// Hide and Seek (#1082): the round's seekers (dealt by the roles) stand still and see nothing for the Hide time while the
// hiders run; then they hunt for the Seek time. The round is the seekers' when no hider is standing, and the hiders'
// when the Seek time runs out or no seeker is standing. The winners are the ones of that side still standing (nobody
// standing at all: a round without a winner).
class HideSeekGame {
public:
    enum class Phase : uint8_t { Off, Hiding, Seeking };
    enum class Outcome : uint8_t { None, Seekers, Hiders };
    struct Step { bool released = false; Outcome outcome = Outcome::None; std::vector<uint8_t> winners; };

    void BeginRound(const std::vector<uint8_t>& seekers, uint32_t nowMs, uint8_t hideS, uint16_t seekS) {
        seekers_ = {seekers.begin(), seekers.end()};
        phase_ = Phase::Hiding; hideEnd_ = nowMs + uint32_t(hideS) * 1000u; seekEnd_ = hideEnd_ + uint32_t(seekS) * 1000u;
    }
    void EndRound() { phase_ = Phase::Off; seekers_.clear(); }
    Phase phase() const { return phase_; }
    bool active() const { return phase_ != Phase::Off; }
    bool IsSeeker(uint8_t e) const { return seekers_.count(e) != 0; }
    uint8_t FirstSeeker() const { return seekers_.empty() ? uint8_t(0) : *seekers_.begin(); }
    uint32_t HideLeftMs(uint32_t nowMs) const { return phase_ == Phase::Hiding ? Left(hideEnd_, nowMs) : 0; }
    uint32_t RoundLeftMs(uint32_t nowMs) const { return active() ? Left(seekEnd_, nowMs) : 0; }   // the Hide and the Seek time together

    // Each tick of a round in play, with every fighter standing. An outcome ends the round here (EndRound).
    Step Update(const std::vector<uint8_t>& living, uint32_t nowMs) {
        Step s;
        if (!active()) return s;
        if (phase_ == Phase::Hiding && int32_t(nowMs - hideEnd_) >= 0) { phase_ = Phase::Seeking; s.released = true; }
        std::vector<uint8_t> seekers, hiders;
        for (uint8_t e : living) (seekers_.count(e) ? seekers : hiders).push_back(e);
        if (hiders.empty()) { s.outcome = Outcome::Seekers; s.winners = seekers; }
        else if (seekers.empty() || int32_t(nowMs - seekEnd_) >= 0) { s.outcome = Outcome::Hiders; s.winners = hiders; }
        if (s.outcome != Outcome::None) EndRound();
        return s;
    }

private:
    static uint32_t Left(uint32_t end, uint32_t nowMs) { const int32_t left = int32_t(end - nowMs); return left > 0 ? uint32_t(left) : 0; }
    Phase phase_ = Phase::Off;
    uint32_t hideEnd_ = 0, seekEnd_ = 0;
    std::set<uint8_t> seekers_;
};
// A fighter that joins mid-match (or comes back from a fall) in Crossbow TDM still gets one.
inline bool CrossbowForEveryone(Format f, bool classesPresent) { return IsCrossbowTdm(f) && classesPresent; }

}  // namespace hsm
