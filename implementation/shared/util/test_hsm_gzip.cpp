// The gzip writer: every output inflates back to its input (a fixed-Huffman inflater of its own, below), the CRC and
// size trailer are right, and a log shrinks. Run via implementation/shared/run_tests.sh test_hsm_gzip.
#include "util/hsm_gzip.h"
#include <cstdio>
#include <random>

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++g_fail; } } while (0)
using namespace hsm::util;
using namespace hsm::util::gzip_detail;

// RFC 1951 inflate of one fixed-Huffman block; false on anything else or past the end.
class Inflater {
public:
    Inflater(const std::string& s, size_t at) : s_(s), pos_(at) {}
    bool Run(std::string& out) {
        if (Get(1) != 1 || Get(2) != 1) return false;
        for (;;) {
            const int sym = Sym();
            if (sym < 0 || bad_) return false;
            if (sym < 256) { out.push_back(char(sym)); continue; }
            if (sym == 256) return true;
            const int l = sym - 257;
            if (l > 28) return false;
            const int len = kLenBase[l] + int(Get(kLenExtra[l]));
            int dc = 0;
            for (int k = 0; k < 5; ++k) dc = (dc << 1) | int(Get(1));
            if (dc > 29) return false;
            const int dist = kDistBase[dc] + int(Get(kDistExtra[dc]));
            if (bad_ || dist > int(out.size())) return false;
            for (int k = 0; k < len; ++k) out.push_back(out[out.size() - size_t(dist)]);
        }
    }
    size_t End() const { return pos_ + (bit_ ? 1 : 0); }
private:
    uint32_t Get(int n) {
        uint32_t v = 0;
        for (int k = 0; k < n; ++k) {
            if (pos_ >= s_.size()) { bad_ = true; return 0; }
            v |= uint32_t((uint8_t(s_[pos_]) >> bit_) & 1) << k;
            if (++bit_ == 8) { bit_ = 0; ++pos_; }
        }
        return v;
    }
    int Sym() {   // codes of 7, 8, 9 bits, read most significant first
        int code = 0;
        for (int len = 1; len <= 9; ++len) {
            code = (code << 1) | int(Get(1));
            if (len == 7 && code <= 0x17) return 256 + code;
            if (len == 8 && code >= 0x30 && code <= 0xBF) return code - 0x30;
            if (len == 8 && code >= 0xC0 && code <= 0xC7) return 280 + code - 0xC0;
            if (len == 9 && code >= 0x190) return 144 + code - 0x190;
        }
        return -1;
    }
    const std::string& s_;
    size_t pos_;
    int bit_ = 0;
    bool bad_ = false;
};

static bool RoundTrip(const std::string& in, size_t* gzSize = nullptr) {
    const std::string gz = Gzip(in);
    if (gzSize) *gzSize = gz.size();
    if (gz.size() < 18 || uint8_t(gz[0]) != 0x1f || uint8_t(gz[1]) != 0x8b || gz[2] != 8) return false;
    Inflater inf(gz, 10);
    std::string out;
    if (!inf.Run(out) || out != in || inf.End() + 8 != gz.size()) return false;
    uint32_t crc = 0, size = 0;
    for (int k = 0; k < 4; ++k) crc |= uint32_t(uint8_t(gz[inf.End() + size_t(k)])) << (8 * k);
    for (int k = 0; k < 4; ++k) size |= uint32_t(uint8_t(gz[inf.End() + 4 + size_t(k)])) << (8 * k);
    return crc == Crc32(reinterpret_cast<const uint8_t*>(in.data()), in.size()) && size == uint32_t(in.size());
}

int main() {
    CHECK(Crc32(reinterpret_cast<const uint8_t*>("123456789"), 9) == 0xCBF43926u);   // the CRC-32 check value
    CHECK(RoundTrip(""));
    CHECK(RoundTrip("a"));
    CHECK(RoundTrip("ab"));
    CHECK(RoundTrip("abc"));
    CHECK(RoundTrip(std::string(100000, 'x')));                 // matches of 258, distance 1
    std::mt19937 rng(7);
    std::string noise;
    for (int i = 0; i < 200000; ++i) noise.push_back(char(rng() & 0xFF));
    CHECK(RoundTrip(noise));                                     // every byte value, few matches
    std::string log;
    for (int i = 0; i < 40000; ++i) {
        char line[160];
        std::snprintf(line, sizeof line, "[12:%02d:%02d.%03d][%u] grabs: e%d held by e%d (hand R, bone hand_r, grab %d)\n",
                      i / 3600 % 60, i / 60 % 60, i % 1000, 1000 + unsigned(rng() % 8), int(rng() % 8), int(rng() % 8), i);
        log += line;
    }
    size_t gz = 0;
    CHECK(RoundTrip(log, &gz));
    CHECK(gz * 4 < log.size());                                  // a log shrinks at least 4x
    std::string far = noise.substr(0, 40000) + noise.substr(0, 40000);   // a repeat farther than the window
    CHECK(RoundTrip(far));
    if (g_fail == 0) std::printf("test_hsm_gzip: all passed (log %zu -> %zu bytes)\n", log.size(), gz);
    return g_fail == 0 ? 0 : 1;
}
