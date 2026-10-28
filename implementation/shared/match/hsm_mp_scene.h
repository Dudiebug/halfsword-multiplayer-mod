// The multiplayer menus' content (0.7.0-pre6): the voice settings block (on the multiplayer page and in the in-match
// Esc menu), the in-match player list with mute, volume, kick and votekick, and the vote bar. The session fills the
// plain structs below; these functions decide every label, value, hint and which buttons show, so the UMG code only
// maps them to widgets. Pure, no engine types.
#pragma once
#include "voice/hsm_voice_ui.h"
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

namespace hsm {

// ---------------------------------------------------------------- what the session reports

struct MpVoice {
    bool session = false;                 // in a session or a lobby (the reach is the host's then)
    bool host = true;                     // this machine sets the reach (hosting, or not in anyone's lobby)
    bool server = false;                  // the host is a role=server instance (it relays, never talks)
    voice::Reach reach = voice::Reach::Off;
    voice::Mic mic = voice::Mic::PushToTalk;
    int levelDb = -40;
    int key = 'V';
    int volumePct = 100;
    std::string device;                   // empty: the Windows default
    std::vector<std::string> devices;
    bool monitor = false;
    float micDb = -120.f;                 // the microphone's level now (-120: silent or not recording)
    bool gateOpen = false, sending = false, recording = false;
    std::string source;                   // what records ("Steam", a device's name, ...)
};

struct MpPlayer {
    uint8_t slot = 0;
    std::string name;
    bool me = false, host = false, talking = false, muted = false;
    int volumePct = 100;
    int pingMs = -1;                      // -1: not measured
    std::string route;                    // "direct", "through the host", "through <name>", or empty
    bool canKick = false;                 // this machine hosts and may kick or ban this player
    bool canVote = false;                 // this machine may start a vote against this player
    bool canVoteBan = false;              // ... and a voteban too (a role=server session)
};

struct MpVote {
    bool active = false;
    uint8_t target = 0xFF;
    std::string name;                     // the target's name
    int yes = 0, no = 0, needed = 0, voters = 0, secondsLeft = 0;
    int mine = -2;                        // 1 yes, -1 no, 0 not yet, -2 not a voter
    bool ban = false;                     // a voteban (7 days), not a votekick
};

// ---------------------------------------------------------------- the voice block

enum class VoiceRow : uint8_t { Reach, Mic, Device, Level, Key, Volume, Monitor, Count };
struct VoiceRowView {
    VoiceRow id = VoiceRow::Reach;
    std::string label, value, hint;
    bool enabled = true;                  // the < > (or the toggle) may be used
};
struct VoiceScene {
    std::vector<VoiceRowView> rows;       // in VoiceRow order, those that apply
    float meter = 0;                      // 0..1, the microphone now
    float threshold = 0;                  // 0..1, where the open microphone opens (shown in Open mode only)
    bool showThreshold = false;
    bool meterHot = false;                // over the threshold, or the key held: what would be sent
    std::string meterText;                // "-38 dB, sending" ...
    std::string note;                     // one line under the block (why voice is off, ...)
};

inline std::string DbText(float db) {
    if (!(db > -119.f)) return "silent";
    return std::to_string(int(std::lround(db))) + " dB";
}

inline std::string ReachLabel(voice::Reach r) {
    switch (r) {
        case voice::Reach::Off: return "Off";
        case voice::Reach::Proximity: return "Proximity";
        case voice::Reach::Everyone: return "Everyone";
    }
    return "Off";
}
inline std::string MicLabel(voice::Mic m) {
    switch (m) {
        case voice::Mic::Open: return "Open mic";
        case voice::Mic::PushToTalk: return "Push to talk";
        case voice::Mic::Off: return "Muted";
    }
    return "Push to talk";
}

// binding: the menu waits for the next key press for push to talk.
inline VoiceScene BuildVoiceScene(const MpVoice& v, bool binding) {
    VoiceScene s;
    {
        VoiceRowView r; r.id = VoiceRow::Reach; r.label = "Voice chat"; r.value = ReachLabel(v.reach); r.enabled = v.host && !v.server;
        if (v.server) r.hint = "Set by the server.";
        else if (!v.host) r.hint = "The host's setting.";
        else if (v.reach == voice::Reach::Off) r.hint = "Nobody can talk in your sessions.";
        else if (v.reach == voice::Reach::Proximity) r.hint = "Voices fade with distance; the dead hear the dead.";
        else r.hint = "Every living player hears every living one; the dead hear the dead.";
        s.rows.push_back(r);
    }
    {
        VoiceRowView r; r.id = VoiceRow::Mic; r.label = "Microphone"; r.value = MicLabel(v.mic);
        r.hint = v.mic == voice::Mic::Open ? "Sends whenever you speak above the threshold."
               : v.mic == voice::Mic::PushToTalk ? "Sends while you hold the key." : "Nothing is sent; you still hear the others.";
        s.rows.push_back(r);
    }
    {
        VoiceRowView r; r.id = VoiceRow::Device; r.label = "Device";
        const int idx = voice::DeviceIndex(v.devices, v.device);
        r.value = !v.device.empty() && idx < 0 ? v.device + " (not found)" : voice::DeviceLabel(v.devices, idx);
        r.enabled = !v.devices.empty();
        r.hint = v.source.empty() ? std::string() : "Recording through " + v.source + ".";
        s.rows.push_back(r);
    }
    if (v.mic == voice::Mic::Open) {
        VoiceRowView r; r.id = VoiceRow::Level; r.label = "Threshold"; r.value = std::to_string(v.levelDb) + " dB";
        r.hint = "Lower picks up quieter speech (and more noise).";
        s.rows.push_back(r);
    }
    if (v.mic == voice::Mic::PushToTalk) {
        VoiceRowView r; r.id = VoiceRow::Key; r.label = "Push-to-talk key"; r.value = binding ? "Press a key..." : voice::VoiceKeyLabel(v.key);
        r.hint = binding ? "A letter, a digit, F1-F12 or a side mouse button. Esc cancels." : "Click to change.";
        s.rows.push_back(r);
    }
    {
        VoiceRowView r; r.id = VoiceRow::Volume; r.label = "Voice volume"; r.value = std::to_string(v.volumePct) + "%";
        s.rows.push_back(r);
    }
    {
        VoiceRowView r; r.id = VoiceRow::Monitor; r.label = "Hear myself"; r.value = v.monitor ? "On" : "Off";
        r.enabled = v.mic != voice::Mic::Off;
        r.hint = "Plays back what would be sent, to check your microphone.";
        s.rows.push_back(r);
    }
    s.meter = voice::MeterFraction(v.micDb);
    s.threshold = voice::MeterFraction(float(v.levelDb));
    s.showThreshold = v.mic == voice::Mic::Open;
    s.meterHot = v.sending || (v.mic == voice::Mic::Open && v.gateOpen);
    if (v.mic == voice::Mic::Off) s.meterText = "Microphone muted";
    else if (!v.recording) s.meterText = "Not recording";
    else s.meterText = DbText(v.micDb) + (v.sending ? ", sending" : s.meterHot ? ", would send" : "");
    if (v.session && v.reach == voice::Reach::Off) s.note = v.host ? "Voice chat is off for your session." : "The host has voice chat off.";
    return s;
}

// The settings a < or > on `row` leads to (Reach and Monitor are applied by the caller: they are not preferences).
// devices: the list the Device row steps through.
inline MpVoice StepVoice(MpVoice v, VoiceRow row, int dir) {
    switch (row) {
        case VoiceRow::Reach: v.reach = voice::StepReach(v.reach, dir); break;
        case VoiceRow::Mic: v.mic = voice::StepMic(v.mic, dir); break;
        case VoiceRow::Device: {
            const int idx = voice::StepDevice(voice::DeviceIndex(v.devices, v.device), v.devices.size(), dir);
            v.device = idx < 0 ? std::string() : v.devices[size_t(idx)];
            break;
        }
        case VoiceRow::Level: v.levelDb = voice::StepLevelDb(v.levelDb, dir); break;
        case VoiceRow::Volume: v.volumePct = voice::StepVolumePct(v.volumePct, dir); break;
        case VoiceRow::Monitor: v.monitor = !v.monitor; break;
        case VoiceRow::Key: case VoiceRow::Count: break;
    }
    return v;
}

// ---------------------------------------------------------------- the player list

struct PlayerRowView {
    std::string name;                     // with "(you)" / "(host)"
    std::string detail;                   // "48 ms, direct" ...
    bool talking = false;
    bool showMute = false;                // everyone but me
    std::string muteLabel;                // "MUTE" / "UNMUTE"
    bool showVolume = false;
    std::string volume;                   // "100%"
    bool showKick = false, showBan = false, showVote = false, showVoteBan = false;
    bool voteTarget = false;              // the running vote is about this player
};

inline std::vector<PlayerRowView> BuildPlayerRows(const std::vector<MpPlayer>& players, const MpVote& vote) {
    std::vector<PlayerRowView> out;
    for (const MpPlayer& p : players) {
        PlayerRowView r;
        r.name = p.name.empty() ? "Player " + std::to_string(p.slot) : p.name;
        if (p.me) r.name += " (you)";
        else if (p.host) r.name += " (host)";
        if (p.pingMs >= 0) r.detail = std::to_string(p.pingMs) + " ms";
        if (!p.route.empty()) r.detail += (r.detail.empty() ? "" : ", ") + p.route;
        if (!p.me && p.muted) r.detail += (r.detail.empty() ? "" : ", ") + std::string("muted");
        r.talking = p.talking && !(p.muted && !p.me);
        r.showMute = r.showVolume = !p.me;
        r.muteLabel = p.muted ? "UNMUTE" : "MUTE";
        r.volume = std::to_string(p.volumePct) + "%";
        r.showKick = r.showBan = p.canKick && !p.me;
        r.showVote = p.canVote && !p.me && !p.canKick && !vote.active;
        r.showVoteBan = r.showVote && p.canVoteBan;
        r.voteTarget = vote.active && vote.target == p.slot;
        out.push_back(std::move(r));
    }
    return out;
}

// ---------------------------------------------------------------- the vote bar

struct VoteBar {
    bool show = false;
    std::string text;                     // "Kick Bob? 2 of 3 yes, 1 no, 21 s" / "Ban Bob for 7 days? ..."
    bool showButtons = false;             // YES / NO: this machine is a voter
    bool yesOn = false, noOn = false;     // its current vote
};

inline VoteBar BuildVoteBar(const MpVote& v) {
    VoteBar b;
    if (!v.active) return b;
    b.show = true;
    b.text = (v.ban ? "Ban " : "Kick ") + (v.name.empty() ? std::string("player ") + std::to_string(v.target) : v.name) +
             (v.ban ? " for 7 days? " : "? ") + std::to_string(v.yes) + " of " +
             std::to_string(v.needed) + " yes needed, " + std::to_string(v.no) + " no, " + std::to_string(v.secondsLeft) + " s left";
    b.showButtons = v.mine >= -1;
    b.yesOn = v.mine == 1;
    b.noOn = v.mine == -1;
    if (v.mine == -2) b.text += " (you can't vote on this one)";
    return b;
}

}  // namespace hsm
