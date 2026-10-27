#include "gameplay/voice.h"
#include "log.h"
#include "steam/steam_api.h"
#include <windows.h>
#include <mmsystem.h>

namespace hsm::game {
namespace {
constexpr size_t kBufferFrames = 480;   // 20 ms at 24 kHz
constexpr int kBuffers = 4;
}  // namespace

void Voice::Configure(const VoiceSettings& s) {
    set_ = s;
    gate_.Configure({s.levelDb, 400});
    std::lock_guard<std::mutex> lock(mu_);
    mixer_.SetVolume(s.volume);
}

void Voice::SetRecording(bool on) {
    if (on == recording_) return;
    recording_ = on;
    if (on) steam::StartVoice(); else steam::StopVoice();
    if (!on) { gate_.Reset(); preroll_.Clear(); sending_ = false; }
}

void Voice::Capture(bool mayTalk, uint32_t nowMs, std::vector<std::vector<uint8_t>>& out, bool* spurt) {
    out.clear();
    if (spurt) *spurt = false;
    if (!steam::VoiceAvailable()) {
        if (mayTalk && !steamWarned_) { steamWarned_ = true; HSM_LOG("voice: Steam voice is not available here; this machine only listens"); }
        SetRecording(false);
        return;
    }
    bool want = mayTalk;
    if (want && set_.mode == 1) want = (GetAsyncKeyState(set_.key) & 0x8000) != 0;   // push to talk
    if (want != recording_) HSM_LOG("voice: microphone %s", want ? "on" : "off");
    SetRecording(want);
    std::vector<uint8_t> chunk;
    const bool ok = steam::TakeVoice(chunk);
    const uint32_t dt = lastCaptureMs_ ? nowMs - lastCaptureMs_ : 0;
    lastCaptureMs_ = nowMs;
    if (sending_) stats_.gateOpenMs += dt;
    if (!want || !ok || chunk.empty()) {
        if (!want) sending_ = false;
        else if (sending_ && set_.mode == 0 && !gate_.Feed(-120.f, nowMs)) sending_ = false;   // silence: let the hold run out
        return;
    }
    bool open = true;
    if (set_.mode == 0) {
        const bool decoded = steam::DecompressVoice(chunk.data(), chunk.size(), voice::kSampleRate, pcm_);
        open = gate_.Feed(decoded ? voice::RmsDbfs(pcm_.data(), pcm_.size()) : -120.f, nowMs);
    }
    if (!open) { sending_ = false; preroll_.Keep(std::move(chunk)); return; }
    if (!sending_) {
        sending_ = true;
        if (spurt) *spurt = true;
        for (std::vector<uint8_t>& c : preroll_.Take()) out.push_back(std::move(c));
    }
    out.push_back(std::move(chunk));
    for (const std::vector<uint8_t>& c : out) { ++stats_.sent; stats_.sentBytes += uint32_t(c.size()); }
}

void Voice::Receive(uint8_t entity, uint16_t seq, const uint8_t* data, size_t size, uint32_t nowMs) {
    if (entity >= voice::kMaxSpeakers) return;
    if (!seq_[entity].Accept(seq, nowMs)) { ++stats_.late; return; }
    std::vector<int16_t> pcm;
    if (!steam::DecompressVoice(data, size, voice::kSampleRate, pcm) || pcm.empty()) { ++stats_.badChunks; return; }
    ++stats_.received; stats_.receivedBytes += uint32_t(size);
    heardAt_[entity] = nowMs ? nowMs : 1;
    if (!audioRun_ && !deviceFailed_) StartDevice();
    std::lock_guard<std::mutex> lock(mu_);
    mixer_.Push(entity, pcm.data(), pcm.size());
}

void Voice::SetGains(uint8_t entity, voice::Gains g) {
    std::lock_guard<std::mutex> lock(mu_);
    mixer_.SetGains(entity, g);
}

void Voice::Forget(uint8_t entity) {
    if (entity >= voice::kMaxSpeakers) return;
    seq_[entity].Reset(); heardAt_[entity] = 0;
    std::lock_guard<std::mutex> lock(mu_);
    mixer_.Forget(entity);
}

bool Voice::Talking(uint8_t entity, uint32_t nowMs, uint32_t withinMs) const {
    return entity < voice::kMaxSpeakers && heardAt_[entity] && nowMs - heardAt_[entity] <= withinMs;
}

Voice::Stats Voice::TakeStats() {
    Stats s = stats_;
    stats_ = Stats{};
    s.recording = recording_;
    s.device = audioRun_;
    { std::lock_guard<std::mutex> lock(mu_); s.mixerDropped = mixer_.dropped() - mixerDroppedSeen_; mixerDroppedSeen_ = mixer_.dropped(); }
    return s;
}

void Voice::Stop() {
    SetRecording(false);
    StopDevice();
    for (voice::SeqGuard& g : seq_) g.Reset();
    heardAt_.fill(0);
    std::lock_guard<std::mutex> lock(mu_);
    mixer_.Clear();
}

bool Voice::StartDevice() {
    if (audioRun_) return true;
    if (audio_.joinable()) audio_.join();   // a loop that ended on its own (no device)
    audioRun_ = true;
    audio_ = std::thread([this] { AudioLoop(); });
    return true;
}

void Voice::StopDevice() {
    audioRun_ = false;
    if (audio_.joinable()) audio_.join();
}

// waveOut, 24 kHz stereo, four 20 ms buffers: each one is refilled from the mixer as the device finishes it.
void Voice::AudioLoop() {
    WAVEFORMATEX fmt{};
    fmt.wFormatTag = WAVE_FORMAT_PCM; fmt.nChannels = 2; fmt.nSamplesPerSec = voice::kSampleRate; fmt.wBitsPerSample = 16;
    fmt.nBlockAlign = 4; fmt.nAvgBytesPerSec = voice::kSampleRate * 4;
    HANDLE done = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    HWAVEOUT out = nullptr;
    const MMRESULT r = done ? waveOutOpen(&out, WAVE_MAPPER, &fmt, DWORD_PTR(done), 0, CALLBACK_EVENT) : MMSYSERR_ERROR;
    if (r != MMSYSERR_NOERROR) {
        HSM_LOG("voice: could not open the audio output (waveOutOpen %u); other players' voices are not played", unsigned(r));
        if (done) CloseHandle(done);
        deviceFailed_ = true;
        audioRun_ = false;
        return;
    }
    HSM_LOG("voice: audio output open (24 kHz stereo, %d x 20 ms buffers)", kBuffers);
    std::array<std::vector<int16_t>, kBuffers> buf;
    std::array<WAVEHDR, kBuffers> hdr{};
    for (int i = 0; i < kBuffers; ++i) {
        buf[i].assign(kBufferFrames * 2, 0);
        hdr[i].lpData = reinterpret_cast<LPSTR>(buf[i].data());
        hdr[i].dwBufferLength = DWORD(buf[i].size() * sizeof(int16_t));
        waveOutPrepareHeader(out, &hdr[i], sizeof(WAVEHDR));
        hdr[i].dwFlags |= WHDR_DONE;   // first pass: every buffer is free
    }
    while (audioRun_) {
        for (int i = 0; i < kBuffers; ++i) {
            if (!(hdr[i].dwFlags & WHDR_DONE)) continue;
            { std::lock_guard<std::mutex> lock(mu_); mixer_.Mix(buf[i].data(), kBufferFrames); }
            hdr[i].dwFlags &= ~DWORD(WHDR_DONE);
            waveOutWrite(out, &hdr[i], sizeof(WAVEHDR));
        }
        WaitForSingleObject(done, 50);
    }
    waveOutReset(out);
    for (WAVEHDR& h : hdr) waveOutUnprepareHeader(out, &h, sizeof(WAVEHDR));
    waveOutClose(out);
    CloseHandle(done);
    HSM_LOG("voice: audio output closed");
}

}  // namespace hsm::game
