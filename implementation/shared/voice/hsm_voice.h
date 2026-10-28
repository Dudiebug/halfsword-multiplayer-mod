// Proximity voice chat (0.7.0-pre5), the game-independent parts. Steam records and compresses the microphone
// (ISteamUser voice); the client glue (gameplay/voice.cpp) decodes, mixes and plays through waveOut. This header
// decides what is sent, who hears it and how loud:
// - LevelGate: an open microphone sends while the input is above a level (dBFS), and for a short hold after.
// - PreRoll: the chunks just before the gate opened go out first, so the first syllable is not cut.
// - ConsciousGate: a fighter whose consciousness drops below a share of its full value is cut off (and a knocked-out
//   or dead one); the full value is the highest seen, so the game's scale does not matter.
// - Audible / RelayVoice: the living are heard near them (dead spectators hear them too, from their camera); the dead
//   are heard only by the dead, at full volume. AudibleIn: the host's setting (pre6) can make the living heard by
//   every living player at full volume instead (Everyone).
// - Spatialize: distance fade and left/right pan from the listener's camera.
// - SeqGuard, VoiceRate: late or repeated frames are dropped; a sender cannot flood.
// - Mixer: per speaker queue of mono PCM, a short prebuffer (jitter), gains ramped per buffer (no clicks).
#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <vector>

namespace hsm::voice {

constexpr uint32_t kSampleRate = 24000;              // Steam decompresses to this; waveOut plays it (stereo)
constexpr size_t kMaxSpeakers = 20;                  // indexed by entity (fighters 1..9; 0 unused); room to spare
constexpr uint32_t kPrebufferMs = 80;                // a speaker starts playing once this much is queued
constexpr uint32_t kMaxQueueMs = 500;                // older audio is dropped beyond this (a stalled link catches up)

// ---------------------------------------------------------------- level gate
inline float RmsDbfs(const int16_t* s, size_t n) {
    if (!s || n == 0) return -120.f;
    double sum = 0;
    for (size_t i = 0; i < n; ++i) sum += double(s[i]) * double(s[i]);
    const double rms = std::sqrt(sum / double(n)) / 32768.0;
    return rms <= 1e-6 ? -120.f : float(20.0 * std::log10(rms));
}

struct GateConfig { float thresholdDb = -40.f; uint32_t holdMs = 400; };

class LevelGate {
public:
    explicit LevelGate(GateConfig c = {}) : c_(c) {}
    void Configure(GateConfig c) { c_ = c; }
    // One decoded chunk's level. Returns whether the gate is open after it.
    bool Feed(float db, uint32_t nowMs) {
        if (db >= c_.thresholdDb) { open_ = true; lastLoud_ = nowMs; }
        else if (open_ && nowMs - lastLoud_ > c_.holdMs) open_ = false;
        return open_;
    }
    bool open() const { return open_; }
    void Reset() { open_ = false; lastLoud_ = 0; }
private:
    GateConfig c_;
    bool open_ = false;
    uint32_t lastLoud_ = 0;
};

// Compressed chunks seen while the gate was closed; the newest few go out first when it opens.
class PreRoll {
public:
    static constexpr size_t kChunks = 3;
    void Keep(std::vector<uint8_t> chunk) {
        if (chunk.empty()) return;
        q_.push_back(std::move(chunk));
        while (q_.size() > kChunks) q_.pop_front();
    }
    std::vector<std::vector<uint8_t>> Take() { std::vector<std::vector<uint8_t>> out(q_.begin(), q_.end()); q_.clear(); return out; }
    void Clear() { q_.clear(); }
    size_t size() const { return q_.size(); }
private:
    std::deque<std::vector<uint8_t>> q_;
};

// ---------------------------------------------------------------- consciousness
constexpr float kReopenMargin = 0.05f;   // hysteresis: the voice comes back at cut + 5 % of full

class ConsciousGate {
public:
    // value: the fighter's Consciousness now; dead: DED (or no fighter). cut: share of full (0.3 = 30 %).
    // True: the fighter may talk.
    bool Update(float value, bool dead, float cut) {
        if (!std::isfinite(value)) value = 0.f;
        if (value > peak_) peak_ = value;
        if (dead || value <= 0.f || peak_ <= 0.f) { ok_ = false; return ok_; }
        const float share = value / peak_;
        if (ok_ && share < cut) ok_ = false;
        else if (!ok_ && share >= cut + kReopenMargin - 1e-6f) ok_ = true;
        return ok_;
    }
    bool ok() const { return ok_; }
    float peak() const { return peak_; }
    void Reset() { *this = ConsciousGate{}; }
private:
    float peak_ = 0.f;
    bool ok_ = false;
};

// ---------------------------------------------------------------- who hears whom
enum class Hear : uint8_t { None, Spatial, Flat };

// speakerAlive: the speaker's fighter is alive; speakerAwake: and conscious enough (ConsciousGate);
// listenerAlive: the listener has a live fighter (a dead or waiting player, or a spectator, does not).
inline Hear Audible(bool speakerAlive, bool speakerAwake, bool listenerAlive) {
    if (!speakerAlive) return listenerAlive ? Hear::None : Hear::Flat;   // the dead talk among themselves
    if (!speakerAwake) return Hear::None;                                 // knocked out or nearly: cut off
    return Hear::Spatial;
}

// The host's session setting (0.7.0-pre6): no voice, by distance, or every living player hears every living one.
enum class Reach : uint8_t { Off = 0, Proximity = 1, Everyone = 2 };

inline Hear AudibleIn(Reach reach, bool speakerAlive, bool speakerAwake, bool listenerAlive) {
    if (reach == Reach::Off) return Hear::None;
    const Hear h = Audible(speakerAlive, speakerAwake, listenerAlive);
    return reach == Reach::Everyone && h == Hear::Spatial ? Hear::Flat : h;
}

struct Range { float nearCm = 500.f, farCm = 3000.f; };
constexpr float kRelayMarginCm = 500.f;   // the host relays a little past far: positions are sampled, people move

// The host's relay decision for one listener. distCm: speaker head to listener head (negative: unknown).
inline bool RelayVoice(Hear h, float distCm, bool listenerAlive, Range r) {
    if (h == Hear::None) return false;
    if (h == Hear::Flat || !listenerAlive || distCm < 0.f) return true;   // dead spectators place it from their camera
    return distCm <= r.farCm + kRelayMarginCm;
}

struct Vec3 { float x = 0, y = 0, z = 0; };
struct Gains { float left = 0, right = 0; };

inline float DistanceGain(float d, Range r) {
    if (!(d > r.nearCm)) return 1.f;
    if (d >= r.farCm || r.farCm <= r.nearCm) return 0.f;
    const float t = 1.f - (d - r.nearCm) / (r.farCm - r.nearCm);
    return t * t;
}

constexpr float kPanFloor = 0.3f;   // the far ear keeps this much: a hard pan sounds wrong on headphones

// UE axes: yaw 0 faces +X, yaw grows clockwise seen from above, right = (-sin yaw, cos yaw).
inline Gains Spatialize(Vec3 listener, float yawDeg, Vec3 speaker, Range r) {
    const float dx = speaker.x - listener.x, dy = speaker.y - listener.y, dz = speaker.z - listener.z;
    const float g = DistanceGain(std::sqrt(dx * dx + dy * dy + dz * dz), r);
    const float h = std::sqrt(dx * dx + dy * dy);
    float pan = 0.f;
    if (h > 1.f) {
        const float yaw = yawDeg * 3.14159265f / 180.f;
        pan = std::clamp((dx * -std::sin(yaw) + dy * std::cos(yaw)) / h, -1.f, 1.f);
    }
    const float a = (pan + 1.f) * 3.14159265f / 4.f;
    float l = std::min(1.f, std::cos(a) * 1.41421356f), rr = std::min(1.f, std::sin(a) * 1.41421356f);
    l = kPanFloor + (1.f - kPanFloor) * std::max(0.f, l);
    rr = kPanFloor + (1.f - kPanFloor) * std::max(0.f, rr);
    return {g * l, g * rr};
}

// ---------------------------------------------------------------- frames
// Per speaker: drops repeats and frames older than the newest (16-bit sequence, wraps). A gap of a second or more
// starts over (a new talk spurt, a rejoin).
class SeqGuard {
public:
    bool Accept(uint16_t seq, uint32_t nowMs) {
        if (!any_ || nowMs - lastAt_ >= 1000 || int16_t(uint16_t(seq - last_)) > 0) {
            any_ = true; last_ = seq; lastAt_ = nowMs;
            return true;
        }
        return false;
    }
    void Reset() { any_ = false; }
private:
    bool any_ = false;
    uint16_t last_ = 0;
    uint32_t lastAt_ = 0;
};

// Per sender: at most kFramesPerS frames and kBytesPerS bytes a second (token buckets, one second of burst).
class VoiceRate {
public:
    static constexpr uint32_t kFramesPerS = 60, kBytesPerS = 16 * 1024;
    bool Allow(size_t bytes, uint32_t nowMs) {
        if (!started_) { started_ = true; at_ = nowMs; frames_ = kFramesPerS; bytes_ = kBytesPerS; }
        const uint32_t dt = std::min<uint32_t>(nowMs - at_, 1000);
        at_ = nowMs;
        frames_ = std::min<double>(kFramesPerS, frames_ + kFramesPerS * dt / 1000.0);
        bytes_ = std::min<double>(kBytesPerS, bytes_ + kBytesPerS * dt / 1000.0);
        if (frames_ < 1.0 || bytes_ < double(bytes)) return false;
        frames_ -= 1.0; bytes_ -= double(bytes);
        return true;
    }
private:
    bool started_ = false;
    uint32_t at_ = 0;
    double frames_ = 0, bytes_ = 0;
};

// ---------------------------------------------------------------- mixer
// Not thread-safe: the glue holds a mutex around Push/SetGains (game thread) and Mix (audio thread).
class Mixer {
public:
    static constexpr size_t kPrebuffer = size_t(kSampleRate) * kPrebufferMs / 1000;
    static constexpr size_t kMaxQueue = size_t(kSampleRate) * kMaxQueueMs / 1000;

    void Push(size_t speaker, const int16_t* pcm, size_t n) {
        if (speaker >= kMaxSpeakers || !pcm) return;
        Ch& c = ch_[speaker];
        c.q.insert(c.q.end(), pcm, pcm + n);
        if (c.q.size() > kMaxQueue) { dropped_ += c.q.size() - kMaxQueue; c.q.erase(c.q.begin(), c.q.begin() + long(c.q.size() - kMaxQueue)); }
    }
    void SetGains(size_t speaker, Gains g) { if (speaker < kMaxSpeakers) ch_[speaker].target = g; }
    void SetVolume(float v) { volume_ = std::clamp(v, 0.f, 2.f); }

    // Fills frames stereo samples (interleaved L R). Each speaker's gain moves linearly to its target over the buffer.
    void Mix(int16_t* out, size_t frames) {
        std::vector<float> acc(frames * 2, 0.f);
        for (Ch& c : ch_) {
            if (!c.playing && c.q.size() >= kPrebuffer) c.playing = true;
            if (!c.playing) continue;
            const size_t n = std::min(frames, c.q.size());
            for (size_t i = 0; i < n; ++i) {
                const float t = frames > 1 ? float(i) / float(frames - 1) : 1.f;
                const float gl = c.cur.left + (c.target.left - c.cur.left) * t, gr = c.cur.right + (c.target.right - c.cur.right) * t;
                const float s = float(c.q[i]);
                acc[2 * i] += s * gl; acc[2 * i + 1] += s * gr;
            }
            c.q.erase(c.q.begin(), c.q.begin() + long(n));
            c.cur = c.target;
            if (c.q.empty()) c.playing = false;   // underrun: wait for the prebuffer again
        }
        for (size_t i = 0; i < frames * 2; ++i) out[i] = int16_t(std::clamp(acc[i] * volume_, -32768.f, 32767.f));
    }
    size_t Queued(size_t speaker) const { return speaker < kMaxSpeakers ? ch_[speaker].q.size() : 0; }
    bool Playing(size_t speaker) const { return speaker < kMaxSpeakers && ch_[speaker].playing; }
    void Forget(size_t speaker) { if (speaker < kMaxSpeakers) ch_[speaker] = Ch{}; }
    void Clear() { for (Ch& c : ch_) c = Ch{}; }
    uint64_t dropped() const { return dropped_; }
private:
    struct Ch { std::deque<int16_t> q; Gains cur, target; bool playing = false; };
    std::array<Ch, kMaxSpeakers> ch_{};
    float volume_ = 1.f;
    uint64_t dropped_ = 0;
};

}  // namespace hsm::voice
