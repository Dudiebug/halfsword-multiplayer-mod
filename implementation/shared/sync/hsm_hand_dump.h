// #3282: another player's hand shows open on a held shield, buckler or some weapons. The fields the rig already logs
// (LogHands) stay at 0 on owners that show a closed hand, so the field that closes it is not among them. The rig now
// dumps every Willie bool and double whose name is about hands (hand, grip, fist, grab, attach, lock) and differs from
// the class default, on the owner and on its stand-in, at each grip change and again a moment later; the two machines'
// lines name the field the stand-in lacks.
//
// Pure: which member names count, and when a dump is due. Game-independent.
#pragma once
#include <cstdint>
#include <string_view>

namespace hsm::sync {

// A member name about the hands, matched without case: "R Fisting Rate", "Grab L Target Found", "R Hand Constraint
// Locked", "L Hand Attached Correctly".
inline bool IsHandFieldName(std::string_view name) {
    static constexpr std::string_view kWords[] = {"hand", "grip", "fist", "grab", "attach", "lock"};
    auto lower = [](char c) { return c >= 'A' && c <= 'Z' ? char(c - 'A' + 'a') : c; };
    for (std::string_view w : kWords)
        for (size_t i = 0; i + w.size() <= name.size(); ++i) {
            size_t k = 0;
            while (k < w.size() && lower(name[i + k]) == w[k]) ++k;
            if (k == w.size()) return true;
        }
    return false;
}

// When a fighter's hand fields are dumped: at a grip change (unless a dump ran less than kGapMs ago) and once more
// kLaterMs after the latest change, when the hand has settled; never two within kGapMs, and at most kMax in all.
class HandDump {
public:
    static constexpr double kGapMs = 2000, kLaterMs = 2000;
    static constexpr int kMax = 200;

    void GripChanged(double nowMs) { now_ = true; later_ = true; laterAt_ = nowMs + kLaterMs; }
    // True when a dump is due now (and counts it).
    bool Due(double nowMs) {
        if (count_ >= kMax) return false;
        const bool gapOk = !any_ || nowMs - lastMs_ >= kGapMs;
        if (now_) {
            now_ = false;
            if (gapOk) return Take(nowMs);
        }
        if (later_ && nowMs >= laterAt_ && gapOk) { later_ = false; return Take(nowMs); }
        return false;
    }
    int Count() const { return count_; }

private:
    bool Take(double nowMs) { any_ = true; lastMs_ = nowMs; ++count_; return true; }
    bool now_ = false, later_ = false, any_ = false;
    double laterAt_ = 0, lastMs_ = 0;
    int count_ = 0;
};

}  // namespace hsm::sync
