// SHA-256 (FIPS 180-4), streaming, header-only, no dependencies. Used to identify content packs (.pak files) by hash:
// session/hsm_modmaps.h compares the host's map pak with the player's own, byte for byte, by this digest.
#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <string>

namespace hsm {

using Sha256Digest = std::array<uint8_t, 32>;

class Sha256 {
public:
    Sha256() { Reset(); }
    void Reset() {
        static constexpr uint32_t init[8] = {0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au, 0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u};
        for (int i = 0; i < 8; ++i) h_[i] = init[i];
        len_ = 0; fill_ = 0;
    }
    void Update(const uint8_t* d, size_t n) {
        len_ += uint64_t(n);
        while (n) {
            const size_t take = n < 64 - fill_ ? n : 64 - fill_;
            for (size_t i = 0; i < take; ++i) buf_[fill_ + i] = d[i];
            fill_ += take; d += take; n -= take;
            if (fill_ == 64) { Block(buf_); fill_ = 0; }
        }
    }
    // The digest of everything fed so far; the object is reset (ready for a new message).
    Sha256Digest Final() {
        const uint64_t bits = len_ * 8;
        const uint8_t pad = 0x80;
        Update(&pad, 1);
        const uint8_t zero = 0;
        while (fill_ != 56) Update(&zero, 1);
        uint8_t lenBytes[8];
        for (int i = 0; i < 8; ++i) lenBytes[i] = uint8_t(bits >> (56 - 8 * i));
        Update(lenBytes, 8);
        Sha256Digest out{};
        for (int i = 0; i < 8; ++i) for (int j = 0; j < 4; ++j) out[size_t(i * 4 + j)] = uint8_t(h_[i] >> (24 - 8 * j));
        Reset();
        return out;
    }
private:
    static uint32_t Rotr(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }
    void Block(const uint8_t* p) {
        static constexpr uint32_t k[64] = {
            0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu, 0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u,
            0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u, 0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u,
            0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu, 0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
            0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u, 0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u,
            0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u, 0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
            0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u, 0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
            0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
            0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u, 0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u};
        uint32_t w[64];
        for (int i = 0; i < 16; ++i)
            w[i] = uint32_t(p[i * 4]) << 24 | uint32_t(p[i * 4 + 1]) << 16 | uint32_t(p[i * 4 + 2]) << 8 | uint32_t(p[i * 4 + 3]);
        for (int i = 16; i < 64; ++i) {
            const uint32_t s0 = Rotr(w[i - 15], 7) ^ Rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
            const uint32_t s1 = Rotr(w[i - 2], 17) ^ Rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }
        uint32_t a = h_[0], b = h_[1], c = h_[2], d = h_[3], e = h_[4], f = h_[5], g = h_[6], hh = h_[7];
        for (int i = 0; i < 64; ++i) {
            const uint32_t s1 = Rotr(e, 6) ^ Rotr(e, 11) ^ Rotr(e, 25);
            const uint32_t ch = (e & f) ^ (~e & g);
            const uint32_t t1 = hh + s1 + ch + k[i] + w[i];
            const uint32_t s0 = Rotr(a, 2) ^ Rotr(a, 13) ^ Rotr(a, 22);
            const uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
            const uint32_t t2 = s0 + maj;
            hh = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
        }
        h_[0] += a; h_[1] += b; h_[2] += c; h_[3] += d; h_[4] += e; h_[5] += f; h_[6] += g; h_[7] += hh;
    }
    uint32_t h_[8];
    uint8_t buf_[64];
    size_t fill_ = 0;
    uint64_t len_ = 0;
};

inline Sha256Digest Sha256Of(const uint8_t* d, size_t n) { Sha256 s; s.Update(d, n); return s.Final(); }

inline std::string Sha256Hex(const Sha256Digest& dg) {
    static const char* const hex = "0123456789abcdef";
    std::string s;
    s.reserve(64);
    for (uint8_t b : dg) { s += hex[b >> 4]; s += hex[b & 15]; }
    return s;
}
// 64 hex digits (either case) -> digest. False for anything else.
inline bool Sha256FromHex(const std::string& s, Sha256Digest& out) {
    if (s.size() != 64) return false;
    auto nib = [](char c) -> int { return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1; };
    for (size_t i = 0; i < 32; ++i) {
        const int hi = nib(s[i * 2]), lo = nib(s[i * 2 + 1]);
        if (hi < 0 || lo < 0) return false;
        out[i] = uint8_t(hi << 4 | lo);
    }
    return true;
}
inline bool Sha256IsZero(const Sha256Digest& d) { for (uint8_t b : d) if (b) return false; return true; }

}  // namespace hsm
