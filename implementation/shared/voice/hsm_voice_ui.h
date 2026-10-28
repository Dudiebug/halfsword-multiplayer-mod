// Voice settings as the menus show and change them (0.7.0-pre6): the host's session setting (Off / Proximity /
// Everyone), each player's microphone mode, device, level, push-to-talk key and volume, per-speaker mute and volume,
// the level meter, and the ini values behind them. Pure: the menus (ui/) draw the rows, the session applies them.
#pragma once
#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <vector>

#include "voice/hsm_voice.h"

namespace hsm::voice {

// ---------------------------------------------------------------- the host's session setting
inline const char* ReachName(Reach r) { return r == Reach::Proximity ? "Proximity" : r == Reach::Everyone ? "Everyone" : "Off"; }
inline const char* ReachIni(Reach r) { return r == Reach::Proximity ? "proximity" : r == Reach::Everyone ? "everyone" : "off"; }
inline Reach ReachFromIni(const std::string& s, Reach def = Reach::Off) {
    return s == "proximity" ? Reach::Proximity : s == "everyone" ? Reach::Everyone : s == "off" ? Reach::Off : def;
}
inline Reach StepReach(Reach r, int dir) { return Reach((uint8_t(r) + (dir < 0 ? 2 : 1)) % 3); }

// ---------------------------------------------------------------- this player's microphone
enum class Mic : uint8_t { Open = 0, PushToTalk = 1, Off = 2 };
inline const char* MicName(Mic m) { return m == Mic::Open ? "Open mic" : m == Mic::PushToTalk ? "Push to talk" : "Off"; }
inline const char* MicIni(Mic m) { return m == Mic::Open ? "level" : m == Mic::PushToTalk ? "ptt" : "off"; }
inline bool MicFromIni(const std::string& s, Mic& out) {
    if (s == "level") out = Mic::Open; else if (s == "ptt") out = Mic::PushToTalk; else if (s == "off") out = Mic::Off; else return false;
    return true;
}
inline Mic StepMic(Mic m, int dir) { return Mic((uint8_t(m) + (dir < 0 ? 2 : 1)) % 3); }

// Open-mic level: -70..-10 dBFS in 5 dB steps; volume (own and per speaker): 0..200 % in 10 % steps.
constexpr int kLevelMinDb = -70, kLevelMaxDb = -10, kLevelStepDb = 5;
constexpr int kVolumeMaxPct = 200, kVolumeStepPct = 10;
inline int StepLevelDb(int db, int dir) {
    if (db < kLevelMinDb || db > kLevelMaxDb) return std::clamp(db, kLevelMinDb, kLevelMaxDb);   // a bad ini value: into range first
    const int snapped = kLevelMinDb + (db - kLevelMinDb + (dir > 0 ? kLevelStepDb - 1 : 0)) / kLevelStepDb * kLevelStepDb;   // off-grid: to the grid first
    if (snapped != db) return snapped;
    return std::clamp(db + (dir < 0 ? -kLevelStepDb : kLevelStepDb), kLevelMinDb, kLevelMaxDb);
}
inline int StepVolumePct(int pct, int dir) {
    if (pct < 0 || pct > kVolumeMaxPct) return std::clamp(pct, 0, kVolumeMaxPct);
    const int snapped = (pct + (dir > 0 ? kVolumeStepPct - 1 : 0)) / kVolumeStepPct * kVolumeStepPct;
    if (snapped != pct) return snapped;
    return std::clamp(pct + (dir < 0 ? -kVolumeStepPct : kVolumeStepPct), 0, kVolumeMaxPct);
}

// The level meter: dBFS onto 0..1 over the open-mic range (the threshold mark uses the same mapping).
inline float MeterFraction(float db) {
    if (!(db > float(kLevelMinDb))) return 0.f;
    return std::min(1.f, (db - float(kLevelMinDb)) / float(-kLevelMinDb));
}

// ---------------------------------------------------------------- push-to-talk keys
// A letter, a digit, f1..f12, mouse4 or mouse5 (case-insensitive) as a Windows virtual-key code; 0: none of those.
inline int ParseVoiceKey(std::string k) {
    for (char& ch : k) ch = char(std::tolower(static_cast<unsigned char>(ch)));
    if (k.size() == 1 && ((k[0] >= 'a' && k[0] <= 'z') || (k[0] >= '0' && k[0] <= '9'))) return std::toupper(static_cast<unsigned char>(k[0]));
    if (k == "mouse4") return 0x05;   // VK_XBUTTON1
    if (k == "mouse5") return 0x06;   // VK_XBUTTON2
    if (k.size() >= 2 && k.size() <= 3 && k[0] == 'f') {
        const int n = std::atoi(k.c_str() + 1);
        if (n >= 1 && n <= 12 && std::to_string(n) == k.substr(1)) return 0x70 + n - 1;   // VK_F1..VK_F12
    }
    return 0;
}
// The ini spelling of a key ParseVoiceKey accepts ("v", "f5", "mouse4"); empty for any other code.
inline std::string VoiceKeyIni(int vk) {
    if ((vk >= 'A' && vk <= 'Z') || (vk >= '0' && vk <= '9')) return std::string(1, char(std::tolower(vk)));
    if (vk == 0x05) return "mouse4";
    if (vk == 0x06) return "mouse5";
    if (vk >= 0x70 && vk <= 0x7B) return "f" + std::to_string(vk - 0x70 + 1);
    return {};
}
// What the menus show: "V", "F5", "Mouse 4".
inline std::string VoiceKeyLabel(int vk) {
    if (vk == 0x05) return "Mouse 4";
    if (vk == 0x06) return "Mouse 5";
    std::string s = VoiceKeyIni(vk);
    for (char& c : s) c = char(std::toupper(static_cast<unsigned char>(c)));
    return s;
}
// Every key a "press a key" prompt listens for, in the order it checks them.
inline std::vector<int> VoiceKeyCandidates() {
    std::vector<int> out;
    for (int c = 'A'; c <= 'Z'; ++c) out.push_back(c);
    for (int c = '0'; c <= '9'; ++c) out.push_back(c);
    for (int c = 0x70; c <= 0x7B; ++c) out.push_back(c);
    out.push_back(0x05); out.push_back(0x06);
    return out;
}

// ---------------------------------------------------------------- devices
// The device the ini names (by its Windows name: indexes move when devices come and go) among the ones present now:
// its index, or -1 for the Windows default (empty name, or a device that is gone).
inline int DeviceIndex(const std::vector<std::string>& devices, const std::string& name) {
    if (name.empty()) return -1;
    for (size_t i = 0; i < devices.size(); ++i) if (devices[i] == name) return int(i);
    return -1;
}
// Steps through "Windows default" (-1) and every device.
inline int StepDevice(int cur, size_t count, int dir) {
    const int n = int(count) + 1;   // -1 .. count-1
    int i = (std::clamp(cur, -1, int(count) - 1) + 1 + (dir < 0 ? n - 1 : 1)) % n;
    return i - 1;
}
inline std::string DeviceLabel(const std::vector<std::string>& devices, int index) {
    return index >= 0 && size_t(index) < devices.size() ? devices[size_t(index)] : std::string("Windows default");
}

// ---------------------------------------------------------------- per-speaker mute and volume (this player only)
struct Listen { bool muted = false; int volumePct = 100; };
inline float ListenGain(const Listen& l) { return l.muted ? 0.f : float(std::clamp(l.volumePct, 0, kVolumeMaxPct)) / 100.f; }

}  // namespace hsm::voice
