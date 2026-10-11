// Tracking-error statistics with bounded memory, for the stand-in quality lines (mirror mode, desync log): exact
// count, mean and max, and percentiles from a fixed 0.1 uu histogram. A percentile is the nearest-rank value
// (rank ceil(q*n)) reported as the upper edge of its bin, capped at the exact max, so it is never below the exact
// nearest-rank value and at most kBinUu above it (past kTopUu it is the exact max). Pure; no allocation.
#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

namespace hsm::sync {

class ErrorStats {
public:
    static constexpr double kBinUu = 0.1;
    static constexpr int kBins = 2560;                    // 0 .. 256 uu; larger errors share one overflow bin
    static constexpr double kTopUu = kBinUu * kBins;

    // Non-finite or negative samples are counted as bad and otherwise ignored.
    void Add(double e) {
        if (!std::isfinite(e) || e < 0.0) { ++bad_; return; }
        ++n_;
        sum_ += e;
        max_ = std::max(max_, e);
        const double b = std::floor(e * (1.0 / kBinUu));
        ++bins_[b >= kBins ? kBins : static_cast<size_t>(b)];
    }

    void Reset() { *this = ErrorStats{}; }

    uint64_t Count() const { return n_; }
    uint64_t Bad() const { return bad_; }
    double Mean() const { return n_ ? sum_ / static_cast<double>(n_) : 0.0; }
    double Max() const { return max_; }

    // q in (0, 1]; 0 when empty.
    double Percentile(double q) const {
        if (n_ == 0) return 0.0;
        q = std::clamp(q, 1e-9, 1.0);
        const uint64_t rank = std::max<uint64_t>(1, static_cast<uint64_t>(std::ceil(q * static_cast<double>(n_))));
        uint64_t seen = 0;
        for (int i = 0; i < kBins; ++i) {
            seen += bins_[static_cast<size_t>(i)];
            if (seen >= rank) return std::min((i + 1) * kBinUu, max_);
        }
        return max_;   // the rank falls among the overflow samples
    }

private:
    std::array<uint32_t, kBins + 1> bins_{};
    uint64_t n_ = 0, bad_ = 0;
    double sum_ = 0.0, max_ = 0.0;
};

}  // namespace hsm::sync
