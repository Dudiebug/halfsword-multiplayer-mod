// Shared dice (HSMMod 0.8.0-pre6, protocol rev 28): a map's Blueprint randomness (UKismetMathLibrary.RandomInteger,
// RandomFloatInRange, RandomBool ...) comes out the same on every machine of a session. Every machine rolls from the
// same stream: the value is a hash of (the level's seed, the rolling object's stable identity, how many times that
// object has rolled), not the C runtime's rand(), so no machine's other randomness shifts anybody's door or turbulence.
// The host picks a 64-bit seed per travel and sends it ahead of the Travel (LevelSeed 55). Game-independent and
// unit-tested; the native-thunk swap and the object filter are in gameplay/shared_dice.cpp.
//
// The converters follow UKismetMathLibrary as this build runs it (thunks read from the shipping exe):
//   RandomInteger(Max)              Max > 0 ? min(floor(u * Max), Max - 1) : 0
//   RandomIntegerInRange(Min, Max)  Min + min(floor(u * (Max - Min + 1)), Max - Min), Min when Max < Min; inclusive
//   RandomFloat()                   [0, 1)
//   RandomFloatInRange(Min, Max)    Min + (Max - Min) * u, [Min, Max)
//   RandomBool()                    u >= 0.5
//   RandomBoolWithWeight(Weight)    Weight > 0 and u < Weight (Weight 1 is always true)
// Wire: LevelSeed (host -> client, reliable): 4 header, u32 travelSeq, u64 seed.
#pragma once
#include "hsm_protocol.h"
#include <cmath>
#include <cstdint>
#include <string_view>
#include <vector>

namespace hsm::dice {

// ------------------------------------------------------------------ the stream

inline uint64_t Mix64(uint64_t x) {   // splitmix64 finalizer: every input bit reaches every output bit
    x += 0x9E3779B97F4A7C15ull;
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ull;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBull;
    return x ^ (x >> 31);
}

// An object's identity from its name, 64-bit FNV-1a. The caller builds the text from what every machine shares (the
// world's name and the actor's name), never from an address or an index.
inline uint64_t IdentityKey(std::string_view text) {
    uint64_t h = 0xCBF29CE484222325ull;
    for (unsigned char c : text) { h ^= c; h *= 0x100000001B3ull; }
    return Mix64(h);
}

// The n-th roll of the object under key in the level seeded with seed; sub picks a further, independent draw within the
// same roll (a vector's three axes). Equal inputs always give the equal 64 bits.
inline uint64_t Draw(uint64_t seed, uint64_t key, uint64_t n, uint32_t sub = 0) {
    return Mix64(Mix64(seed ^ Mix64(key)) + Mix64(n * 0x100000001B3ull + sub + 1));
}

// 53 random bits as a double in [0, 1).
inline double Unit(uint64_t bits) { return double(bits >> 11) * (1.0 / 9007199254740992.0); }

// ------------------------------------------------------------------ UKismetMathLibrary semantics

inline int32_t IntBelow(double u, int32_t count) {   // [0, count), 0 for count <= 0 (FMath::RandHelper)
    if (count <= 0) return 0;
    int32_t v = int32_t(u * double(count));
    return v < count ? v : count - 1;
}
inline int32_t RandomInteger(double u, int32_t max) { return IntBelow(u, max); }
inline int32_t RandomIntegerInRange(double u, int32_t min, int32_t max) {
    const int64_t range = int64_t(max) - int64_t(min) + 1;
    if (range <= 0) return min;
    const int64_t pick = int64_t(u * double(range));
    return int32_t(int64_t(min) + (pick < range ? pick : range - 1));
}
inline double RandomFloatInRange(double u, double min, double max) {
    if (!std::isfinite(min) || !std::isfinite(max)) return min;
    double v = min + (max - min) * u;
    if (max > min && v >= max) v = std::nextafter(max, min);   // rounding on a huge range must not reach Max
    return v;
}
inline bool RandomBool(double u) { return u >= 0.5; }
inline bool RandomBoolWithWeight(double u, float weight) { return weight > 0.0f && u < double(weight); }

struct Vec3 { double x = 0, y = 0, z = 0; };
// A direction, uniform over the sphere (z uniform in [-1, 1], the angle uniform around it).
inline Vec3 RandomUnitVector(double u1, double u2) {
    const double z = 2.0 * u1 - 1.0, r = std::sqrt(std::fmax(0.0, 1.0 - z * z)), a = 6.283185307179586 * u2;
    return {r * std::cos(a), r * std::sin(a), z};
}
// FRotator (pitch, yaw, roll) in degrees: pitch and yaw in [0, 360), roll only when asked for (else 0).
inline Vec3 RandomRotator(double u1, double u2, double u3, bool roll) { return {u1 * 360.0, u2 * 360.0, roll ? u3 * 360.0 : 0.0}; }
// A point in the box centre +- halfSize.
inline Vec3 RandomPointInBox(double u1, double u2, double u3, const Vec3& centre, const Vec3& half) {
    return {centre.x + half.x * (2.0 * u1 - 1.0), centre.y + half.y * (2.0 * u2 - 1.0), centre.z + half.z * (2.0 * u3 - 1.0)};
}

// ------------------------------------------------------------------ wire

struct LevelSeedMsg {
    uint32_t travelSeq = 0;   // the Travel this seed belongs to (the host's Travel.seq)
    uint64_t seed = 0;
};
inline std::vector<uint8_t> Encode(const LevelSeedMsg& m) {
    Writer w; w.header(MsgType::LevelSeed); w.u32(m.travelSeq); w.u64(m.seed);
    return w.buf;
}
inline bool Decode(const uint8_t* d, size_t n, LevelSeedMsg& m) {
    Reader r(d, n); MsgType t; if (!r.header(t) || t != MsgType::LevelSeed) return false;
    m = LevelSeedMsg{};
    m.travelSeq = r.u32(); m.seed = r.u64();
    return r.done();
}

}  // namespace hsm::dice
