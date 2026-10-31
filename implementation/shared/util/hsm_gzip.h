// A small gzip writer (RFC 1952 around RFC 1951 deflate) for report uploads: LZ77 over a 32 KB window with hash chains,
// one fixed-Huffman block. Logs are line after line of the same words, so this alone shrinks them 5-10x; the relay
// inflates with Python's zlib. Compression only (nothing here reads gzip), no allocation past the output and the window
// tables, no dependency. Pure: any thread.
#pragma once
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace hsm::util {

inline uint32_t Crc32(const uint8_t* d, size_t n, uint32_t crc = 0) {
    static const std::array<uint32_t, 256> table = [] {
        std::array<uint32_t, 256> t{};
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t c = i;
            for (int k = 0; k < 8; ++k) c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            t[i] = c;
        }
        return t;
    }();
    crc = ~crc;
    for (size_t i = 0; i < n; ++i) crc = table[(crc ^ d[i]) & 0xFF] ^ (crc >> 8);
    return ~crc;
}

namespace gzip_detail {

class Bits {
public:
    explicit Bits(std::string& out) : out_(out) {}
    void Put(uint32_t value, int count) {   // LSB first, as deflate packs everything but Huffman codes
        acc_ |= uint64_t(value) << n_;
        n_ += count;
        while (n_ >= 8) { out_.push_back(char(acc_ & 0xFF)); acc_ >>= 8; n_ -= 8; }
    }
    void Code(uint32_t code, int len) {     // a Huffman code: its bits most significant first
        uint32_t r = 0;
        for (int i = 0; i < len; ++i) r |= ((code >> i) & 1u) << (len - 1 - i);
        Put(r, len);
    }
    void Flush() { if (n_ > 0) { out_.push_back(char(acc_ & 0xFF)); acc_ = 0; n_ = 0; } }
private:
    std::string& out_;
    uint64_t acc_ = 0;
    int n_ = 0;
};

// RFC 1951 3.2.6, the fixed literal/length code.
inline void Symbol(Bits& b, int sym) {
    if (sym < 144) b.Code(0x30 + sym, 8);
    else if (sym < 256) b.Code(0x190 + (sym - 144), 9);
    else if (sym < 280) b.Code(sym - 256, 7);
    else b.Code(0xC0 + (sym - 280), 8);
}

constexpr uint16_t kLenBase[29] = { 3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258 };
constexpr uint8_t kLenExtra[29] = { 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0 };
constexpr uint16_t kDistBase[30] = { 1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385, 513, 769, 1025, 1537, 2049, 3073,
                                     4097, 6145, 8193, 12289, 16385, 24577 };
constexpr uint8_t kDistExtra[30] = { 0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13 };

inline void Match(Bits& b, int len, int dist) {
    int l = 28;
    while (kLenBase[l] > len) --l;
    Symbol(b, 257 + l);
    if (kLenExtra[l]) b.Put(uint32_t(len - kLenBase[l]), kLenExtra[l]);
    int d = 29;
    while (kDistBase[d] > dist) --d;
    b.Code(uint32_t(d), 5);
    if (kDistExtra[d]) b.Put(uint32_t(dist - kDistBase[d]), kDistExtra[d]);
}

}  // namespace gzip_detail

// The raw deflate stream (one final fixed-Huffman block). maxChain: how many earlier places a match is looked for.
inline std::string Deflate(const uint8_t* d, size_t n, int maxChain = 32) {
    using namespace gzip_detail;
    constexpr int kWindow = 32768, kMinMatch = 3, kMaxMatch = 258, kHashBits = 15;
    std::string out;
    out.reserve(n / 4 + 64);
    Bits b(out);
    b.Put(1, 1);   // BFINAL
    b.Put(1, 2);   // BTYPE 01: fixed Huffman
    std::vector<int32_t> head(size_t(1) << kHashBits, -1), prev(kWindow, -1);
    auto hash = [&](size_t i) { return ((uint32_t(d[i]) << 10) ^ (uint32_t(d[i + 1]) << 5) ^ d[i + 2]) & ((1u << kHashBits) - 1); };
    auto insert = [&](size_t i) {
        if (i + kMinMatch > n) return;
        const uint32_t h = hash(i);
        prev[i % kWindow] = head[h];
        head[h] = int32_t(i);
    };
    size_t i = 0;
    while (i < n) {
        int bestLen = 0, bestDist = 0;
        if (i + kMinMatch <= n) {
            const size_t limit = std::min<size_t>(kMaxMatch, n - i);
            int32_t cand = head[hash(i)];
            for (int chain = 0; cand >= 0 && chain < maxChain; ++chain) {
                const size_t c = size_t(cand);
                if (i - c > size_t(kWindow) - 1 || c >= i) break;
                if (d[c + size_t(bestLen)] == d[i + size_t(bestLen)] || bestLen == 0) {
                    size_t len = 0;
                    while (len < limit && d[c + len] == d[i + len]) ++len;
                    if (int(len) > bestLen) { bestLen = int(len); bestDist = int(i - c); if (len == limit) break; }
                }
                const int32_t next = prev[c % kWindow];
                if (next >= cand) break;   // the slot was reused by a newer place: the chain ended
                cand = next;
            }
        }
        if (bestLen >= kMinMatch) {
            Match(b, bestLen, bestDist);
            for (int k = 0; k < bestLen; ++k) insert(i + size_t(k));
            i += size_t(bestLen);
        } else {
            Symbol(b, d[i]);
            insert(i);
            ++i;
        }
    }
    Symbol(b, 256);   // end of block
    b.Flush();
    return out;
}

inline std::string Gzip(const std::string& text, int maxChain = 32) {
    const auto* d = reinterpret_cast<const uint8_t*>(text.data());
    std::string out = std::string("\x1f\x8b\x08\x00\x00\x00\x00\x00\x00\xff", 10);   // no name, no time, unknown OS
    out += Deflate(d, text.size(), maxChain);
    const uint32_t crc = Crc32(d, text.size()), size = uint32_t(text.size());
    for (int k = 0; k < 4; ++k) out.push_back(char((crc >> (8 * k)) & 0xFF));
    for (int k = 0; k < 4; ++k) out.push_back(char((size >> (8 * k)) & 0xFF));
    return out;
}

}  // namespace hsm::util
