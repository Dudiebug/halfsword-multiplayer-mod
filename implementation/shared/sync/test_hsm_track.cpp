// Unit tests for hsm_track.h. Run via implementation/shared/run_tests.sh test_hsm_track.
#include "sync/hsm_track.h"
#include "sync/hsm_linksim.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <vector>

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++g_fail; } } while (0)

using namespace hsm::sync;

static double ExactNearestRank(std::vector<double> v, double q) {
    std::sort(v.begin(), v.end());
    size_t rank = static_cast<size_t>(std::ceil(q * double(v.size())));
    if (rank < 1) rank = 1;
    return v[rank - 1];
}

// 1. Empty stats read as zero.
static void TestEmpty() {
    ErrorStats s;
    CHECK(s.Count() == 0 && s.Mean() == 0.0 && s.Max() == 0.0 && s.Percentile(0.99) == 0.0 && s.Bad() == 0);
}

// 2. Mean and max are exact; 1..100 gives p99 in [99, 99.1] and p50 in [50, 50.1].
static void TestExactMeanMax() {
    ErrorStats s;
    for (int i = 1; i <= 100; ++i) s.Add(i);
    CHECK(s.Count() == 100);
    CHECK(std::fabs(s.Mean() - 50.5) < 1e-12);
    CHECK(s.Max() == 100.0);
    CHECK(s.Percentile(0.99) >= 99.0 && s.Percentile(0.99) <= 99.1 + 1e-9);
    CHECK(s.Percentile(0.50) >= 50.0 && s.Percentile(0.50) <= 50.1 + 1e-9);
    CHECK(s.Percentile(1.0) == 100.0);
}

// 3. Property: for random samples (including some past the histogram top), every percentile is within
// [exact, exact + bin] of the exact nearest-rank value, and never above the max.
static void TestPercentileBoundProperty() {
    SplitMix64 rng(11);
    for (int trial = 0; trial < 200; ++trial) {
        ErrorStats s;
        std::vector<double> v;
        const int n = 1 + int(rng.Next() % 3000);
        const double scale = trial % 4 == 0 ? 400.0 : 60.0;
        for (int i = 0; i < n; ++i) {
            const double u = rng.Unit();
            const double e = u * u * scale;   // skewed toward small errors, like tracking error
            v.push_back(e);
            s.Add(e);
        }
        for (double q : {0.5, 0.9, 0.99, 0.999, 1.0}) {
            const double exact = ExactNearestRank(v, q), got = s.Percentile(q);
            CHECK(got >= exact - 1e-9);
            CHECK(got <= std::max(exact + ErrorStats::kBinUu, exact) + 1e-9 || (exact >= ErrorStats::kTopUu && got == s.Max()));
            CHECK(got <= s.Max());
        }
    }
}

// 4. Errors past the top of the histogram: a percentile landing among them is the exact max.
static void TestOverflow() {
    ErrorStats s;
    for (int i = 0; i < 98; ++i) s.Add(1.0);
    s.Add(300.0);
    s.Add(500.0);
    CHECK(s.Max() == 500.0);
    CHECK(s.Percentile(0.99) == 500.0);         // rank 99 is the 300 sample, in the overflow bin -> reported as max
    CHECK(s.Percentile(0.98) >= 1.0 && s.Percentile(0.98) <= 1.1 + 1e-9);
}

// 5. NaN, infinities and negatives are counted as bad and leave the numbers alone.
static void TestBadSamples() {
    ErrorStats s;
    s.Add(2.0);
    s.Add(std::nan(""));
    s.Add(INFINITY);
    s.Add(-1.0);
    CHECK(s.Count() == 1 && s.Bad() == 3);
    CHECK(s.Mean() == 2.0 && s.Max() == 2.0);
}

// 6. Reset clears everything.
static void TestReset() {
    ErrorStats s;
    s.Add(5.0); s.Add(-1.0);
    s.Reset();
    CHECK(s.Count() == 0 && s.Bad() == 0 && s.Max() == 0.0 && s.Percentile(0.99) == 0.0);
    s.Add(1.0);
    CHECK(s.Count() == 1 && s.Percentile(0.99) <= 1.1 + 1e-9 && s.Percentile(0.99) >= 1.0);
}

int main() {
    TestEmpty();
    TestExactMeanMax();
    TestPercentileBoundProperty();
    TestOverflow();
    TestBadSamples();
    TestReset();
    if (g_fail) { std::printf("test_hsm_track: %d failure(s)\n", g_fail); return 1; }
    std::printf("test_hsm_track: all passed\n");
    return 0;
}
