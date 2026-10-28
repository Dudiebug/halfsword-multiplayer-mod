// Host damage (0.7.1): in a session every machine runs one Player Damage Rate (the host's) and the stock NPC Damage
// Rate. The game works damage out on the victim's machine (claims.cpp replays a hit through its own Get Damage, which
// reads GI_Settings' Player Damage Rate for a player's fighter: Willie_BP.cpp:2974), so each victim's own sliders used
// to decide how hard they were hit: a player with damage turned down soaked hits that visibly cut them on the
// attacker's screen. The player's own values are never saved over: the game writes every setting into its Settings
// save on any change (UI_GameSettings.cpp:268, GI_Settings.cpp:1950), so the client hooks that save and writes the
// player's own values for it (OwnBeforeSave), and puts them back when the session is over.
#pragma once
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <string>

namespace hsm {

// The host's Player Damage Rate as a percent of the rate (1.0 = 100). kDamageStock: every machine uses the game's own
// stock rate (its GI_Settings class default), whatever that is.
inline constexpr uint16_t kDamageStock = 0xFFFF;
inline constexpr uint16_t kDamageMaxPct = 1000;   // the most Sanitize lets through; the vanilla slider bounds the stepper

// The vanilla damage slider (UI_GameSettings' Slider_DamagePlayer), in percent. The defaults hold until the slider's
// template has been read (DamageRangeFrom).
struct DamageRange {
    int minPct = 0, maxPct = 200, stepPct = 10;
    int stockPct = 100;                            // the class default rate
};

// From the slider's MinValue, MaxValue and StepSize and the class default rate. A slider with no step (0: any value)
// steps by 10%; the stock value is always in range.
inline DamageRange DamageRangeFrom(double sliderMin, double sliderMax, double sliderStep, double stockRate) {
    DamageRange r;
    auto pct = [](double v) { return std::isfinite(v) ? int(std::lround(v * 100.0)) : -1; };
    const int lo = pct(sliderMin), hi = pct(sliderMax), step = pct(sliderStep), stock = pct(stockRate);
    if (lo >= 0 && hi > lo && hi <= kDamageMaxPct) { r.minPct = lo; r.maxPct = hi; }
    r.stepPct = step > 0 && step <= r.maxPct - r.minPct ? step : 10;
    if (stock >= 0 && stock <= kDamageMaxPct) r.stockPct = stock;
    r.minPct = std::min(r.minPct, r.stockPct);
    r.maxPct = std::max(r.maxPct, r.stockPct);
    return r;
}

// Whether a slider's full name is the one in the game's settings screen (UI_GameSettings_C). UI_CameraSettings has a
// slider of the same name with the range 0..1, which gave a host 0..100% and made a client cut the host's 150% to 100%.
inline bool IsGameSettingsSlider(const std::string& fullName) {
    static const std::string kOuter = "UI_GameSettings";
    auto word = [](char c) { return std::isalnum(static_cast<unsigned char>(c)) != 0; };
    for (size_t at = fullName.find(kOuter); at != std::string::npos; at = fullName.find(kOuter, at + 1)) {
        const size_t end = at + kOuter.size();
        const bool startOk = at == 0 || !word(fullName[at - 1]);
        const bool endOk = end == fullName.size() || !(word(fullName[end]) || (fullName[end] == '_' && fullName.compare(end, 2, "_C") != 0));
        const bool afterC = fullName.compare(end, 2, "_C") != 0 || end + 2 == fullName.size() || !word(fullName[end + 2]);
        if ((startOk || (at >= 2 && fullName.compare(at - 2, 2, "__") == 0)) && endOk && afterC) return true;
    }
    return false;
}

// The value a stored percent stands for (kDamageStock: the stock one). The host's is inside the slider's range;
// onlyMax (a client taking the host's value): as sent, up to kDamageMaxPct, whatever this machine's slider runs.
inline int DamagePct(uint16_t stored, const DamageRange& r, bool onlyMax = false) {
    if (stored == kDamageStock) return r.stockPct;
    if (onlyMax) return std::min(int(stored), int(kDamageMaxPct));
    return std::clamp(int(stored), r.minPct, r.maxPct);
}

// One stepper click: by the slider's step, inside its range, stopping at the stock value on the way (it is the one
// to come back to); landing on it stores kDamageStock again.
inline uint16_t StepDamage(uint16_t stored, int dir, const DamageRange& r) {
    const int cur = DamagePct(stored, r);
    int next = cur + (dir < 0 ? -r.stepPct : r.stepPct);
    if ((cur < r.stockPct && next > r.stockPct) || (cur > r.stockPct && next < r.stockPct)) next = r.stockPct;
    next = std::clamp(next, r.minPct, r.maxPct);
    return next == r.stockPct ? kDamageStock : uint16_t(next);
}

inline std::string DamageLabel(uint16_t stored, const DamageRange& r) {
    const int p = DamagePct(stored, r);
    return std::to_string(p) + "%" + (p == r.stockPct ? " (stock)" : "");
}

// The rate the game instance runs while in a session.
inline double DamageRate(uint16_t stored, const DamageRange& r, bool onlyMax = false) { return DamagePct(stored, r, onlyMax) / 100.0; }

// The game's Save Settings is about to write the game instance's rate into the Settings save. If the rate is not
// the forced one, the player moved their own slider (or a level load read their save back): that is their value from
// now on. Returns the value to save: always the player's own.
inline double OwnBeforeSave(double& own, double current, double forced) {
    if (current != forced) own = current;
    return own;
}

}  // namespace hsm
