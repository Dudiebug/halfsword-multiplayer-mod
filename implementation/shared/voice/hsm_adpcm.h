// Voice from a microphone Steam does not record (0.7.0-pre6): Steam's voice API only records the Windows default
// device, so a device the player picks is recorded by the mod (16 kHz mono) and sent as IMA ADPCM, 4 bits a sample.
// Each chunk carries its own starting state, so a lost chunk costs only itself. Receivers play it at 24 kHz like
// Steam's voice (Upsample16To24).
#pragma once
#include <cstddef>
#include <cstdint>
#include <vector>

namespace hsm::voice {

constexpr uint32_t kAdpcmRate = 16000;
constexpr size_t kAdpcmHeader = 3;                 // int16 first sample, uint8 step index
constexpr size_t kAdpcmChunkSamples = 640;         // 40 ms: 3 + 320 bytes

namespace adpcm_detail {
inline const int16_t* Steps() {
    static const int16_t t[89] = {7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45, 50, 55, 60, 66, 73, 80,
        88, 97, 107, 118, 130, 143, 157, 173, 190, 209, 230, 253, 279, 307, 337, 371, 408, 449, 494, 544, 598, 658, 724, 796, 876,
        963, 1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024, 3327, 3660, 4026, 4428, 4871, 5358, 5894, 6484,
        7132, 7845, 8630, 9493, 10442, 11487, 12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767};
    return t;
}
constexpr int kIndexShift[8] = {-1, -1, -1, -1, 2, 4, 6, 8};

struct State { int pred = 0; int index = 0; };

inline int Clamp16(int v) { return v < -32768 ? -32768 : v > 32767 ? 32767 : v; }
inline int ClampIndex(int i) { return i < 0 ? 0 : i > 88 ? 88 : i; }

// Applies one 4-bit code to the state; returns the new sample.
inline int Step(State& s, int code) {
    const int step = Steps()[s.index];
    int diff = step >> 3;
    if (code & 4) diff += step;
    if (code & 2) diff += step >> 1;
    if (code & 1) diff += step >> 2;
    s.pred = Clamp16((code & 8) ? s.pred - diff : s.pred + diff);
    s.index = ClampIndex(s.index + kIndexShift[code & 7]);
    return s.pred;
}
}  // namespace adpcm_detail

// pcm: 16 kHz mono. The first sample goes in the header as is, every later one as a code; an odd last code is padded.
inline std::vector<uint8_t> EncodeAdpcm(const int16_t* pcm, size_t n) {
    using namespace adpcm_detail;
    std::vector<uint8_t> out;
    if (!pcm || n == 0) return out;
    State s; s.pred = pcm[0];
    s.index = 0;
    // Start at a step near the chunk's first difference, so a loud chunk does not spend its first codes ramping up.
    if (n > 1) {
        const int d = pcm[1] - pcm[0] < 0 ? pcm[0] - pcm[1] : pcm[1] - pcm[0];
        while (s.index < 88 && Steps()[s.index] < d) ++s.index;
    }
    out.push_back(uint8_t(s.pred & 0xFF)); out.push_back(uint8_t((s.pred >> 8) & 0xFF)); out.push_back(uint8_t(s.index));
    uint8_t pending = 0;
    bool half = false;
    for (size_t i = 1; i < n; ++i) {
        const int step = Steps()[s.index];
        int diff = pcm[i] - s.pred;
        int code = 0;
        if (diff < 0) { code = 8; diff = -diff; }
        if (diff >= step) { code |= 4; diff -= step; }
        if (diff >= step >> 1) { code |= 2; diff -= step >> 1; }
        if (diff >= step >> 2) { code |= 1; }
        Step(s, code);
        if (!half) { pending = uint8_t(code); half = true; }
        else { out.push_back(uint8_t(pending | (code << 4))); half = false; }
    }
    if (half) out.push_back(pending);
    return out;
}

// False for a chunk too short or with a bad header. Every byte after the header holds two samples (low nibble first);
// a padded last code decodes to one extra sample, harmless at 16 kHz.
inline bool DecodeAdpcm(const uint8_t* d, size_t n, std::vector<int16_t>& out) {
    using namespace adpcm_detail;
    out.clear();
    if (!d || n < kAdpcmHeader + 1 || d[2] > 88) return false;
    State s;
    s.pred = int16_t(uint16_t(d[0] | (d[1] << 8)));
    s.index = d[2];
    out.reserve(1 + (n - kAdpcmHeader) * 2);
    out.push_back(int16_t(s.pred));
    for (size_t i = kAdpcmHeader; i < n; ++i) {
        out.push_back(int16_t(Step(s, d[i] & 0x0F)));
        out.push_back(int16_t(Step(s, d[i] >> 4)));
    }
    return true;
}

// 16 kHz to 24 kHz: three output samples per two input samples, linear in between. Each chunk stands alone (its
// last sample is held at the edge), like its ADPCM state.
inline void Upsample16To24(const int16_t* in, size_t n, std::vector<int16_t>& out) {
    out.clear();
    if (!in || n == 0) return;
    out.reserve(n * 3 / 2 + 2);
    for (size_t i = 0; i + 1 < n; i += 2) {
        const int a = in[i], b = in[i + 1], c = i + 2 < n ? in[i + 2] : in[i + 1];
        out.push_back(int16_t(a));
        out.push_back(int16_t((a + 2 * b) / 3));                 // 2/3 of the way from a to b
        out.push_back(int16_t((2 * b + c) / 3));                 // 1/3 of the way from b to c
    }
    if (n % 2) out.push_back(in[n - 1]);
}

}  // namespace hsm::voice
