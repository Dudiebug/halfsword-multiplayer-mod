// Netcode measurement (competitive netcode Phase 0, docs/PLAN_COMPETITIVE_NETCODE.md 1 and 7). Pure logic, unit-tested.
//
// The plan's four properties, as numbers per remote fighter over a sliding window:
//   staleness   how far in the past its stand-in is shown: the playout delay, plus the one-way latency, minus any lead
//   error       how far the stand-in's pelvis was off the pose it was driven toward (uu)
//   margin      for its inputs (rev 42 InputBatch): execution time minus arrival time; negative = arrived late, predicted
//   claims      blows checked on this machine (combat/hsm_claimcheck.h): the distance and how many were far
// Each is a windowed series with p50/p95, so a health line reads like a fighting game's netcode overlay.
#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace hsm::sync {

constexpr double kStatsWindowMs = 5000.0;
constexpr size_t kStatsCap = 512;

// Samples of one metric over the last kStatsWindowMs (bounded: the oldest go first past kStatsCap).
class Series {
public:
    void Add(double nowMs, double v) {
        if (!std::isfinite(v) || !std::isfinite(nowMs)) return;
        Trim(nowMs);
        if (n_ == kStatsCap) { head_ = (head_ + 1) % kStatsCap; --n_; }
        s_[(head_ + n_) % kStatsCap] = {nowMs, v};
        ++n_;
    }
    size_t Count(double nowMs) { Trim(nowMs); return n_; }
    // The q quantile (0..1) of the window, or NaN when empty.
    double Quantile(double nowMs, double q) {
        Trim(nowMs);
        if (n_ == 0) return std::nan("");
        std::array<double, kStatsCap> tmp{};
        for (size_t i = 0; i < n_; ++i) tmp[i] = s_[(head_ + i) % kStatsCap].second;
        const size_t k = std::min(n_ - 1, size_t(std::ceil(std::clamp(q, 0.0, 1.0) * double(n_))) - (q > 0 ? 1 : 0));
        std::nth_element(tmp.begin(), tmp.begin() + long(k), tmp.begin() + long(n_));
        return tmp[k];
    }
    // Share of the window's samples below v (0..1), or NaN when empty.
    double ShareBelow(double nowMs, double v) {
        Trim(nowMs);
        if (n_ == 0) return std::nan("");
        size_t c = 0;
        for (size_t i = 0; i < n_; ++i) if (s_[(head_ + i) % kStatsCap].second < v) ++c;
        return double(c) / double(n_);
    }
    void Clear() { n_ = 0; head_ = 0; }

private:
    void Trim(double nowMs) {
        while (n_ > 0 && nowMs - s_[head_].first > kStatsWindowMs) { head_ = (head_ + 1) % kStatsCap; --n_; }
    }
    std::array<std::pair<double, double>, kStatsCap> s_{};
    size_t head_ = 0, n_ = 0;
};

struct FighterNetStats {
    Series staleness, error, margin, correction;
    Series predicted;   // per game frame of the input timeline: 1 predicted, 0 played on time
    void Clear() { staleness.Clear(); error.Clear(); margin.Clear(); correction.Clear(); predicted.Clear(); }
};

struct ClaimNetStats {
    Series dist;        // checked blows: struck spot to contact, uu
    uint32_t ok = 0, far = 0, unchecked = 0, dropped = 0;
};

// Staleness of a stand-in shown with playout delay delayMs over a link of rttMs, sampled leadMs ahead.
inline double StalenessMs(double delayMs, double rttMs, double leadMs) {
    return std::max(0.0, delayMs + std::max(0.0, rttMs) * 0.5 - std::max(0.0, leadMs));
}

// One health-window line for a fighter: "e3 stale p50 92 p95 118 ms | err p95 7.1 cm | input late 4% | corr p95 3.0 cm".
inline std::string FighterLine(uint8_t entity, FighterNetStats& s, double nowMs) {
    auto q = [&](Series& x, double p) { return x.Quantile(nowMs, p); };
    char b[256];
    int n = std::snprintf(b, sizeof b, "e%u stale p50 %.0f p95 %.0f ms | err p95 %.1f cm", unsigned(entity),
                          q(s.staleness, 0.5), q(s.staleness, 0.95), q(s.error, 0.95));
    if (s.margin.Count(nowMs) > 0 && n > 0 && size_t(n) < sizeof b)
        n += std::snprintf(b + n, sizeof b - size_t(n), " | input late %.0f%% margin p50 %.0f ms", 100.0 * s.margin.ShareBelow(nowMs, 0.0), q(s.margin, 0.5));
    if (s.predicted.Count(nowMs) > 0 && n > 0 && size_t(n) < sizeof b)
        n += std::snprintf(b + n, sizeof b - size_t(n), " | predicted %.0f%%", 100.0 * (1.0 - s.predicted.ShareBelow(nowMs, 0.5)));
    if (s.correction.Count(nowMs) > 0 && n > 0 && size_t(n) < sizeof b)
        std::snprintf(b + n, sizeof b - size_t(n), " | corr p95 %.1f cm", q(s.correction, 0.95));
    return b;
}

inline std::string ClaimLine(ClaimNetStats& s, double nowMs) {
    char b[160];
    std::snprintf(b, sizeof b, "claims checked: ok %u far %u unchecked %u dropped %u | dist p50 %.0f p95 %.0f uu",
                  s.ok, s.far, s.unchecked, s.dropped, s.dist.Quantile(nowMs, 0.5), s.dist.Quantile(nowMs, 0.95));
    return b;
}

// ---- the in-game connection panel (chat /net) ---------------------------------------------------------------------
// One row per other player, as a fighting game's netcode overlay shows it: the link (round trip, direct or relayed, how
// much arrives), how far in the past their fighter is shown, and how often their inputs came after they were due.
struct PanelRow {
    std::string name;
    double rttMs = -1;         // -1 unknown
    int relayed = -1;          // -1 unknown, 0 direct, 1 through Steam's relays
    double quality = -1;       // share of packets arriving, 0..1 (-1 unknown)
    double staleP50 = -1;      // ms (-1 none yet)
    double lateShare = -1;     // inputs that arrived after they were due, 0..1 (-1 none yet)
    double predictedShare = -1;// frames their inputs were predicted, 0..1 (-1 none yet)
};

inline std::vector<std::string> NetPanel(uint16_t delayMs, bool lateSend, const std::vector<PanelRow>& rows) {
    std::vector<std::string> out;
    char b[200];
    std::snprintf(b, sizeof b, "netcode: input delay %u ms%s", unsigned(delayMs), lateSend ? ", poses sent right after physics" : "");
    out.push_back(b);
    if (rows.empty()) { out.push_back("netcode: no other players"); return out; }
    for (const PanelRow& r : rows) {
        std::string line = r.name + ": ";
        if (r.rttMs >= 0) { std::snprintf(b, sizeof b, "%.0f ms", r.rttMs); line += b; } else line += "ping ?";
        if (r.relayed >= 0) line += r.relayed ? " relayed" : " direct";
        if (r.quality >= 0) { std::snprintf(b, sizeof b, ", loss %.0f%%", 100.0 * std::clamp(1.0 - r.quality, 0.0, 1.0)); line += b; }
        if (r.staleP50 >= 0) { std::snprintf(b, sizeof b, " | shown %.0f ms late", r.staleP50); line += b; }
        if (r.lateShare >= 0) { std::snprintf(b, sizeof b, " | inputs late %.0f%%", 100.0 * r.lateShare); line += b; }
        if (r.predictedShare >= 0) { std::snprintf(b, sizeof b, " | predicted %.0f%%", 100.0 * r.predictedShare); line += b; }
        out.push_back(line);
    }
    return out;
}

// ---- the frame-rate floor ([net] min_fps) ----------------------------------------------------------------------------
// Below its owner's frame rate a fighter's pose stream thins (sync/hsm_slowsend.h) and everyone sees it in bigger steps.
// FrameFloor says, at most once per kFloorRewarnMs, when this machine's frame rate stayed under the floor for a whole
// kFloorWindowMs window.
constexpr double kFloorWindowMs = 5000.0, kFloorRewarnMs = 60000.0;
class FrameFloor {
public:
    explicit FrameFloor(int minFps = 0) : minFps_(minFps) {}
    // dtMs: this frame's length. True when a warning is due (the window's average frame rate under the floor).
    bool Push(double dtMs, double nowMs) {
        if (minFps_ <= 0 || !(dtMs > 0) || !std::isfinite(dtMs)) return false;
        if (startMs_ < 0) startMs_ = nowMs;
        sumMs_ += dtMs; ++frames_;
        if (nowMs - startMs_ < kFloorWindowMs) return false;
        fps_ = sumMs_ > 0 ? 1000.0 * double(frames_) / sumMs_ : 0.0;
        startMs_ = nowMs; sumMs_ = 0; frames_ = 0;
        if (fps_ >= double(minFps_) || (warnedMs_ >= 0 && nowMs - warnedMs_ < kFloorRewarnMs)) return false;
        warnedMs_ = nowMs;
        return true;
    }
    double LastFps() const { return fps_; }
    int MinFps() const { return minFps_; }
private:
    int minFps_ = 0;
    double startMs_ = -1, sumMs_ = 0, fps_ = 0, warnedMs_ = -1;
    uint64_t frames_ = 0;
};

}  // namespace hsm::sync
