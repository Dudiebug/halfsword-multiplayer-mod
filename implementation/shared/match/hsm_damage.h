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
#include <optional>
#include <string>

namespace hsm {

// The host's Player Damage Rate as a percent of the rate (1.0 = 100). kDamageStock: every machine uses the game's own
// stock rate (its GI_Settings class default), whatever that is.
inline constexpr uint16_t kDamageStock = 0xFFFF;
inline constexpr uint16_t kDamageMaxPct = 1000;   // the most Sanitize lets through, and the most a host can type in
inline constexpr int kDamageSliderMaxPct = 200;   // the lobby slider's end (0.8.0-pre26); a typed value can go past it

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

// The value a stored percent stands for (kDamageStock: the stock one). The host's is from the slider's minimum up to
// kDamageMaxPct (a typed custom value can pass the slider's end, 0.8.0-pre26); onlyMax (a client taking the host's
// value): as sent, up to kDamageMaxPct, whatever this machine's slider runs.
inline int DamagePct(uint16_t stored, const DamageRange& r, bool onlyMax = false) {
    if (stored == kDamageStock) return r.stockPct;
    if (onlyMax) return std::min(int(stored), int(kDamageMaxPct));
    return std::clamp(int(stored), r.minPct, int(kDamageMaxPct));
}

// One stepper click: by the slider's step, inside its range, stopping at the stock value on the way (it is the one
// to come back to); landing on it stores kDamageStock again.
// A typed value past the slider's end steps down onto the end and does not step further up.
inline uint16_t StepDamage(uint16_t stored, int dir, const DamageRange& r) {
    const int cur = DamagePct(stored, r);
    if (cur > r.maxPct) return dir < 0 ? (r.maxPct == r.stockPct ? kDamageStock : uint16_t(r.maxPct)) : stored;
    int next = cur + (dir < 0 ? -r.stepPct : r.stepPct);
    if ((cur < r.stockPct && next > r.stockPct) || (cur > r.stockPct && next < r.stockPct)) next = r.stockPct;
    next = std::clamp(next, r.minPct, r.maxPct);
    return next == r.stockPct ? kDamageStock : uint16_t(next);
}

inline std::string DamageLabel(uint16_t stored, const DamageRange& r) {
    const int p = DamagePct(stored, r);
    return std::to_string(p) + "%" + (p == r.stockPct ? " (stock)" : "");
}

// The lobby slider runs from the vanilla slider's minimum to kDamageSliderMaxPct (or the vanilla end, if that is
// further), in its step. Where its thumb sits for a stored value: a typed value past the end sits at the end.
inline int DamageSliderMax(const DamageRange& r) { return std::max(kDamageSliderMaxPct, r.maxPct); }
inline float DamageSliderPos(uint16_t stored, const DamageRange& r) { return float(std::min(DamagePct(stored, r), DamageSliderMax(r))); }

// The slider let go at pct: snapped to the step from the minimum, inside the slider; the stock value stores kDamageStock.
inline uint16_t DamageFromSlider(float pct, const DamageRange& r) {
    const int top = DamageSliderMax(r);
    int v = std::isfinite(pct) ? int(std::lround((double(pct) - r.minPct) / r.stepPct)) * r.stepPct + r.minPct : r.stockPct;
    v = std::clamp(v, r.minPct, top);
    return v == r.stockPct ? kDamageStock : uint16_t(v);
}

// A value the host typed on the Damage row: digits, an optional % and spaces around, 0 to kDamageMaxPct. No sign,
// no decimals, nothing else. Below the slider's minimum is the caller's (DamagePct clamps it).
inline std::optional<uint16_t> ParseDamageInput(const std::string& text) {
    size_t a = 0, b = text.size();
    while (a < b && std::isspace(static_cast<unsigned char>(text[a]))) ++a;
    while (b > a && std::isspace(static_cast<unsigned char>(text[b - 1]))) --b;
    if (b > a && text[b - 1] == '%') --b;
    while (b > a && std::isspace(static_cast<unsigned char>(text[b - 1]))) --b;
    if (a == b || b - a > 4) return std::nullopt;
    int v = 0;
    for (size_t i = a; i < b; ++i) {
        if (!std::isdigit(static_cast<unsigned char>(text[i]))) return std::nullopt;
        v = v * 10 + (text[i] - '0');
    }
    if (v > kDamageMaxPct) return std::nullopt;
    return uint16_t(v);
}

// The value the host typed, as stored: the stock value stores kDamageStock, below the slider's minimum its minimum.
inline uint16_t DamageFromInput(uint16_t typed, const DamageRange& r) {
    const int v = std::max(int(typed), r.minPct);
    return v == r.stockPct ? kDamageStock : uint16_t(v);
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
