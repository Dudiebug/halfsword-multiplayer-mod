// Proximity voice (0.7.0-pre5): the microphone through Steam (ISteamUser voice), playback through waveOut on its own
// thread. The decisions (level gate, who hears whom, gains) are in shared/voice/hsm_voice.h; the session feeds this
// class what it may send and how loud each speaker is. Game thread, except the audio thread inside.
#pragma once
#include "voice/hsm_voice.h"
#include <array>
#include <atomic>
#include <cstdint>
#include <mutex>
#include <thread>
#include <vector>

namespace hsm::game {

struct VoiceSettings {
    int mode = 0;               // 0: open microphone above levelDb; 1: push to talk on key
    float levelDb = -40.f;
    int key = 'V';
    float volume = 1.f;
};

class Voice {
public:
    ~Voice() { Stop(); }
    void Configure(const VoiceSettings& s);

    // Each frame. mayTalk: this machine's fighter may talk now (voice on here and at the host, alive, conscious
    // enough, the game window in front, not typing). Fills out with the chunks to send; *spurt is true when the first
    // of them starts a talk spurt.
    void Capture(bool mayTalk, uint32_t nowMs, std::vector<std::vector<uint8_t>>& out, bool* spurt);

    // A chunk from a speaker (an entity) that this machine may hear. Late or repeated chunks are dropped.
    void Receive(uint8_t entity, uint16_t seq, const uint8_t* data, size_t size, uint32_t nowMs);
    void SetGains(uint8_t entity, voice::Gains g);
    void Forget(uint8_t entity);
    bool Talking(uint8_t entity, uint32_t nowMs, uint32_t withinMs = 300) const;   // heard in the last withinMs
    bool MeTalking() const { return sending_; }

    void Stop();   // recording and the audio device off, every queue cleared (the session ended)

    struct Stats {
        uint32_t sent = 0, sentBytes = 0, gateOpenMs = 0, received = 0, receivedBytes = 0, late = 0, badChunks = 0;
        uint64_t mixerDropped = 0;
        bool recording = false, device = false;
    };
    Stats TakeStats();   // since the last call

private:
    bool StartDevice();
    void StopDevice();
    void AudioLoop();
    void SetRecording(bool on);

    VoiceSettings set_;
    voice::LevelGate gate_;
    voice::PreRoll preroll_;
    bool recording_ = false, sending_ = false, steamWarned_ = false;
    uint32_t lastCaptureMs_ = 0;
    std::vector<int16_t> pcm_;
    std::array<voice::SeqGuard, voice::kMaxSpeakers> seq_{};
    std::array<uint32_t, voice::kMaxSpeakers> heardAt_{};
    Stats stats_;
    uint64_t mixerDroppedSeen_ = 0;

    mutable std::mutex mu_;          // guards mixer_ (game thread pushes, audio thread mixes)
    voice::Mixer mixer_;
    std::thread audio_;
    std::atomic<bool> audioRun_{false};
    std::atomic<bool> deviceFailed_{false};   // set by the audio thread: no output device, do not retry
};

}  // namespace hsm::game
