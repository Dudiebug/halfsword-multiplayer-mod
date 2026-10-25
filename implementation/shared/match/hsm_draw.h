// When a Duel round is drawn because nobody can finish it (pure, no engine types; the host's referee).
//
// Two-player test 2026-10-25: two rounds of almost four minutes each with both fighters on the ground, unable to act;
// eleven times the host held G to give a round up, the only way out. The game's own states did not see it: Fallen is
// torso tilt (|roll| + |pitch| > 60, Willie_BP.cpp:16897) and a knock-out is Consciousness <= 0, and a fighter
// kneeling, sitting or crawling in pain is neither.
//
// A fighter is down while its pelvis is less than kLowPelvisCm above its lower foot (kneeling, sitting, lying), or the
// game says Fallen or knocked out; standing up for less than kUpGraceMs does not count (a stagger). It is
// incapacitated once it has been down for kDownMs with neither hand moving kHandMoveCm from where it was and without
// dealing a hit (a hand moving that far, or a hit, starts the kDownMs again). When every live fighter of the round
// (two or more) has been incapacitated for kDrawMs together, the round is drawn. One fighter down is never a draw: the
// other may still finish it.
// A hard cap besides (a round cannot last for ever): no hit dealt by anyone for kStallMs while every live fighter is
// below kStallHealth also draws it.
#pragma once
#include "session/hsm_spawn.h"
#include <cstdint>
#include <map>
#include <vector>

namespace hsm {

struct DownSample {
    uint8_t entity = 0;
    double pelvisAboveFeetCm = 100;   // pelvis height over the lower foot
    bool fallen = false, knockedOut = false;
    Point3 handR, handL;              // world positions
    float health = 100;               // Willie's Health, 0..100
};

enum class DrawReason : uint8_t { None = 0, BothDown = 1, Stalled = 2 };

class DrawCheck {
public:
    static constexpr uint32_t kDownMs = 8000, kDrawMs = 12000, kStallMs = 90000, kUpGraceMs = 1000;
    static constexpr double kLowPelvisCm = 70, kHandMoveCm = 100;
    static constexpr float kStallHealth = 40;

    // A new round: nothing seen yet, the stall clock starts now.
    void Reset(uint32_t nowMs) { st_.clear(); allDown_ = false; allSince_ = 0; lastHit_ = nowMs; fired_ = false; started_ = true; }
    // A hit dealt by `attacker`: it is acting (its kDownMs starts again), and the round is not stalled.
    void OnHit(uint8_t attacker, uint32_t nowMs) {
        lastHit_ = nowMs;
        auto it = st_.find(attacker);
        if (it != st_.end() && it->second.down) it->second.downSince = nowMs;
    }
    // Every few hundred ms while the round is fought, with every live fighter's sample. The reason once, the tick the
    // round is to be drawn; None after that until Reset.
    DrawReason Update(const std::vector<DownSample>& live, uint32_t nowMs) {
        if (!started_) Reset(nowMs);
        for (auto it = st_.begin(); it != st_.end();) {            // the dead and the gone are forgotten
            bool there = false;
            for (const DownSample& s : live) there = there || s.entity == it->first;
            it = there ? std::next(it) : st_.erase(it);
        }
        bool all = live.size() >= 2, weak = live.size() >= 2;
        for (const DownSample& s : live) {
            Track(s, nowMs);
            all = all && Incapacitated(s.entity, nowMs);
            weak = weak && s.health < kStallHealth;
        }
        if (!all) allDown_ = false;
        else if (!allDown_) { allDown_ = true; allSince_ = nowMs; }
        if (fired_) return DrawReason::None;
        if (allDown_ && nowMs - allSince_ >= kDrawMs) { fired_ = true; return DrawReason::BothDown; }
        if (weak && nowMs - lastHit_ >= kStallMs) { fired_ = true; return DrawReason::Stalled; }
        return DrawReason::None;
    }
    bool Incapacitated(uint8_t e, uint32_t nowMs) const {
        auto it = st_.find(e);
        return it != st_.end() && it->second.down && nowMs - it->second.downSince >= kDownMs;
    }
    bool Down(uint8_t e) const { auto it = st_.find(e); return it != st_.end() && it->second.down; }

private:
    struct State { bool down = false; uint32_t downSince = 0, upSince = 0; bool rising = false; Point3 anchorR, anchorL; };
    static bool Moved(const Point3& a, const Point3& b) { return Dist2(a, b) > kHandMoveCm * kHandMoveCm; }
    void Track(const DownSample& s, uint32_t nowMs) {
        State& t = st_[s.entity];
        const bool low = s.fallen || s.knockedOut || s.pelvisAboveFeetCm < kLowPelvisCm;
        if (!t.down) {
            if (!low) return;
            t.down = true; t.rising = false; t.downSince = nowMs; t.anchorR = s.handR; t.anchorL = s.handL;
            return;
        }
        if (low) t.rising = false;
        else if (!t.rising) { t.rising = true; t.upSince = nowMs; }
        else if (nowMs - t.upSince >= kUpGraceMs) { t.down = false; t.rising = false; return; }
        if (Moved(s.handR, t.anchorR) || Moved(s.handL, t.anchorL)) { t.downSince = nowMs; t.anchorR = s.handR; t.anchorL = s.handL; }
    }

    std::map<uint8_t, State> st_;
    bool allDown_ = false, fired_ = false, started_ = false;
    uint32_t allSince_ = 0, lastHit_ = 0;
};

}  // namespace hsm
