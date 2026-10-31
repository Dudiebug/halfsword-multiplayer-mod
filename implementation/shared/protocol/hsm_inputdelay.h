// The host's own input delay under host physics (0.8.0-pre19, [net] host_input_delay). Pure logic, unit-tested.
//
// Under host physics a client's input reaches the host's simulation about half a round trip (and, on average, half a send
// interval) after it was given; the host's own input acts at once. The host delays its own fighter's input by the median
// of what the clients lose, so the fight is even. Set in HSMMod.ini only.
#pragma once
#include "hsm_protocol.h"
#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <deque>
#include <string>
#include <utility>
#include <vector>

namespace hsm {

constexpr int kHostDelayAutoMaxMs = 150, kHostDelayFixedMaxMs = 200, kHostDelayStepMs = 10;
constexpr uint32_t kHostDelayEvalMs = 5000;

enum class HostDelayMode : uint8_t { Off, Auto, Fixed };
struct HostDelaySetting {
    HostDelayMode mode = HostDelayMode::Auto;
    int fixedMs = 0;       // Fixed only
    bool valid = true;     // false: the text was neither auto, off nor a number (the setting is auto)
};

// "auto" (or empty) | "0" / "off" | "<ms>" (0..200, a trailing "ms" allowed).
inline HostDelaySetting ParseHostInputDelay(const std::string& raw) {
    std::string s;
    for (char c : raw) if (!std::isspace(static_cast<unsigned char>(c))) s.push_back(char(std::tolower(static_cast<unsigned char>(c))));
    HostDelaySetting r;
    if (s.empty() || s == "auto") return r;
    if (s == "off") { r.mode = HostDelayMode::Off; return r; }
    if (s.size() > 2 && s.compare(s.size() - 2, 2, "ms") == 0) s.resize(s.size() - 2);
    char* end = nullptr;
    const long v = std::strtol(s.c_str(), &end, 10);
    if (s.empty() || end == s.c_str() || *end) { r.valid = false; return r; }
    const int ms = int(std::clamp(v, 0L, long(kHostDelayFixedMaxMs)));
    if (ms == 0) { r.mode = HostDelayMode::Off; return r; }
    r.mode = HostDelayMode::Fixed; r.fixedMs = ms;
    return r;
}

// What one client's input loses against the host's: half its round trip plus half its send interval (1000 / inputHz).
inline int ClientInputLagMs(uint32_t rttMs, int inputHz) {
    const int hz = std::clamp(inputHz, 1, 1000);
    return int((double(rttMs) * 0.5 + 500.0 / double(hz)) + 0.5);
}

// The median over the clients whose round trip is known (0 = not measured yet: left out); 0 with none. An even count
// takes the mean of the middle two. At most kHostDelayAutoMaxMs.
inline int MedianInputLagMs(const std::vector<uint32_t>& rttsMs, int inputHz) {
    std::vector<int> lag;
    for (uint32_t r : rttsMs) if (r > 0) lag.push_back(ClientInputLagMs(r, inputHz));
    if (lag.empty()) return 0;
    std::sort(lag.begin(), lag.end());
    const size_t n = lag.size();
    const int m = n % 2 ? lag[n / 2] : (lag[n / 2 - 1] + lag[n / 2] + 1) / 2;
    return std::clamp(m, 0, kHostDelayAutoMaxMs);
}

// The delay in force. Auto: every kHostDelayEvalMs the target is the median; the delay moves at most
// kHostDelayStepMs per evaluation towards it, and is 0 at once with no clients. Fixed: the number, at once. Off: 0.
class HostDelayGovernor {
public:
    explicit HostDelayGovernor(HostDelaySetting s = {}) : set_(s) {}
    void Configure(HostDelaySetting s) { set_ = s; Suspend(); }
    // Owner sync, no fight, no host: nothing is delayed; the next evaluation starts from 0.
    void Suspend() { current_ = target_ = 0; clients_ = 0; started_ = false; }
    bool Off() const { return set_.mode == HostDelayMode::Off; }
    bool Due(uint32_t nowMs) const { return !Off() && (!started_ || int32_t(nowMs - nextMs_) >= 0); }
    // clientRttsMs: one entry per connected client with a fighter (0: not measured).
    // Returns true when the delay changed.
    bool Evaluate(uint32_t nowMs, const std::vector<uint32_t>& clientRttsMs, int inputHz) {
        const int before = current_;
        started_ = true; nextMs_ = nowMs + kHostDelayEvalMs;
        clients_ = int(clientRttsMs.size());
        if (set_.mode == HostDelayMode::Fixed) { target_ = current_ = set_.fixedMs; return current_ != before; }
        if (Off()) { target_ = current_ = 0; return current_ != before; }
        target_ = MedianInputLagMs(clientRttsMs, inputHz);
        if (!clients_) current_ = 0;
        else current_ += std::clamp(target_ - current_, -kHostDelayStepMs, kHostDelayStepMs);
        return current_ != before;
    }
    int delayMs() const { return current_; }
    int targetMs() const { return target_; }
    int clients() const { return clients_; }
    const char* ModeName() const { return set_.mode == HostDelayMode::Auto ? "auto" : set_.mode == HostDelayMode::Fixed ? "fixed" : "off"; }
private:
    HostDelaySetting set_;
    int current_ = 0, target_ = 0, clients_ = 0;
    bool started_ = false;
    uint32_t nextMs_ = 0;
};

// The host pawn's own events, held back. Actions keep their order and come out once delayMs old; an axis comes out as the
// value it had delayMs ago, every frame while it is not 0 (movement adds its value each call) and for kQuietMs after it
// became 0 (the arm axes still interpolate), then not at all. Take(now, 0) hands over everything that is held.
class DelayedInput {
public:
    static constexpr uint32_t kQuietMs = 500;
    static constexpr size_t kMaxActions = 1024;
    struct Out {
        std::vector<std::pair<uint8_t, bool>> actions;   // (Action index, pressed)
        std::array<float, kAxisCount> axes{};
        std::array<bool, kAxisCount> feed{};             // axes[i] is to be applied this frame
    };
    void PushAction(uint32_t nowMs, uint8_t action, bool pressed) {
        if (actions_.size() >= kMaxActions) actions_.pop_front();
        actions_.push_back({nowMs, action, pressed});
    }
    void PushAxis(uint32_t nowMs, uint8_t axis, float value) {
        if (axis >= kAxisCount) return;
        auto& q = axes_[axis];
        if (!q.empty() && q.back().value == value) return;   // the game fires the axes every frame: only changes matter
        q.push_back({nowMs, value});
    }
    Out Take(uint32_t nowMs, int delayMs) {
        Out out;
        while (!actions_.empty() && int32_t(nowMs - actions_.front().at) >= delayMs) {
            out.actions.push_back({actions_.front().action, actions_.front().pressed});
            actions_.pop_front();
        }
        for (size_t i = 0; i < kAxisCount; ++i) {
            auto& q = axes_[i];
            if (q.empty()) continue;
            while (q.size() >= 2 && int32_t(nowMs - q[1].at) >= delayMs) q.pop_front();
            const float v = q.front().value;
            out.axes[i] = v;
            if (v != 0.f) { out.feed[i] = true; zeroAt_[i] = 0; zeroSet_[i] = false; continue; }
            if (!zeroSet_[i]) { zeroSet_[i] = true; zeroAt_[i] = nowMs; }
            out.feed[i] = nowMs - zeroAt_[i] < kQuietMs;
        }
        return out;
    }
    bool Empty() const {
        if (!actions_.empty()) return false;
        for (const auto& q : axes_) if (!q.empty()) return false;
        return true;
    }
    void Clear() { actions_.clear(); for (auto& q : axes_) q.clear(); zeroSet_.fill(false); }
private:
    struct Act { uint32_t at; uint8_t action; bool pressed; };
    struct Sample { uint32_t at; float value; };
    std::deque<Act> actions_;
    std::array<std::deque<Sample>, kAxisCount> axes_{};
    std::array<uint32_t, kAxisCount> zeroAt_{};
    std::array<bool, kAxisCount> zeroSet_{};
};

}  // namespace hsm
