// The multiplayer menus' content (match/hsm_mp_scene.h): the voice block, the in-match player list and the vote bar.
#include "match/hsm_mp_scene.h"
#include <cstdio>

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++g_fail; } } while (0)
using namespace hsm;

static const VoiceRowView* Row(const VoiceScene& s, VoiceRow id) {
    for (const VoiceRowView& r : s.rows) if (r.id == id) return &r;
    return nullptr;
}

static void TestVoiceRows() {
    MpVoice v;   // defaults: push to talk on V, not in a session, host of our own sessions, voice off
    VoiceScene s = BuildVoiceScene(v, false);
    CHECK(Row(s, VoiceRow::Reach) && Row(s, VoiceRow::Reach)->value == "Off" && Row(s, VoiceRow::Reach)->enabled);
    CHECK(Row(s, VoiceRow::Key) && Row(s, VoiceRow::Key)->value == "V");
    CHECK(!Row(s, VoiceRow::Level));                         // the threshold only for an open microphone
    CHECK(Row(s, VoiceRow::Device) && Row(s, VoiceRow::Device)->value == "Windows default" && !Row(s, VoiceRow::Device)->enabled);
    CHECK(s.note.empty());                                   // not in a session: nothing to warn about
    CHECK(s.meterText == "Not recording" && s.meter == 0.f);

    s = BuildVoiceScene(v, true);
    CHECK(Row(s, VoiceRow::Key)->value == "Press a key...");

    v.mic = voice::Mic::Open; v.levelDb = -45; v.recording = true; v.micDb = -30.f; v.gateOpen = true;
    s = BuildVoiceScene(v, false);
    CHECK(Row(s, VoiceRow::Level) && Row(s, VoiceRow::Level)->value == "-45 dB" && !Row(s, VoiceRow::Key));
    CHECK(s.showThreshold && s.meterHot && s.meter > s.threshold);
    CHECK(s.meterText == "-30 dB, would send");
    v.sending = true; s = BuildVoiceScene(v, false); CHECK(s.meterText == "-30 dB, sending");

    v.mic = voice::Mic::Off; s = BuildVoiceScene(v, false);
    CHECK(s.meterText == "Microphone muted" && !Row(s, VoiceRow::Monitor)->enabled && !Row(s, VoiceRow::Level) && !Row(s, VoiceRow::Key));

    // 0.7.1: a client in someone's session sets its own reach; its own off says so.
    MpVoice c; c.session = true; c.host = false; c.reach = voice::Reach::Off;
    s = BuildVoiceScene(c, false);
    CHECK(Row(s, VoiceRow::Reach)->enabled && s.note == "Your voice chat is off.");
    CHECK(!Row(s, VoiceRow::Cut));                           // nothing to cut while voice is off
    c.reach = voice::Reach::Everyone; s = BuildVoiceScene(c, false);
    CHECK(Row(s, VoiceRow::Reach)->value == "Everyone" && s.note.empty());
    c.server = true; s = BuildVoiceScene(c, false);
    CHECK(Row(s, VoiceRow::Reach)->enabled);                 // a client of a server talks as it likes
    c.host = true; s = BuildVoiceScene(c, false);
    CHECK(!Row(s, VoiceRow::Reach)->enabled);                // the role=server host itself relays only

    // The knocked-out toggle: shown with voice on, flipped by either arrow.
    MpVoice k; k.reach = voice::Reach::Proximity;
    s = BuildVoiceScene(k, false);
    CHECK(Row(s, VoiceRow::Cut) && Row(s, VoiceRow::Cut)->value == "Go quiet" && Row(s, VoiceRow::Cut)->enabled);
    k = StepVoice(k, VoiceRow::Cut, 1); CHECK(!k.cut);
    s = BuildVoiceScene(k, false); CHECK(Row(s, VoiceRow::Cut)->value == "Keep talking");
    k = StepVoice(k, VoiceRow::Cut, -1); CHECK(k.cut);

    // Devices: a named one that is gone says so; the list is stepped through, the default first.
    MpVoice d; d.devices = {"Headset", "USB Mic"}; d.device = "Old Mic";
    s = BuildVoiceScene(d, false);
    CHECK(Row(s, VoiceRow::Device)->value == "Old Mic (not found)" && Row(s, VoiceRow::Device)->enabled);
    d.device = "";
    d = StepVoice(d, VoiceRow::Device, 1); CHECK(d.device == "Headset");
    d = StepVoice(d, VoiceRow::Device, 1); CHECK(d.device == "USB Mic");
    d = StepVoice(d, VoiceRow::Device, 1); CHECK(d.device.empty());
    d = StepVoice(d, VoiceRow::Device, -1); CHECK(d.device == "USB Mic");
}

static void TestStepVoice() {
    MpVoice v;
    v = StepVoice(v, VoiceRow::Mic, 1); CHECK(v.mic == voice::Mic::Off);
    v = StepVoice(v, VoiceRow::Mic, 1); CHECK(v.mic == voice::Mic::Open);
    v = StepVoice(v, VoiceRow::Mic, -1); CHECK(v.mic == voice::Mic::Off);
    v.levelDb = -40; v = StepVoice(v, VoiceRow::Level, 1); CHECK(v.levelDb == -35);
    v.levelDb = voice::kLevelMaxDb; v = StepVoice(v, VoiceRow::Level, 1); CHECK(v.levelDb == voice::kLevelMaxDb);
    v.volumePct = 100; v = StepVoice(v, VoiceRow::Volume, -1); CHECK(v.volumePct == 90);
    v.volumePct = 0; v = StepVoice(v, VoiceRow::Volume, -1); CHECK(v.volumePct == 0);
    v = StepVoice(v, VoiceRow::Reach, 1); CHECK(v.reach == voice::Reach::Proximity);
    v = StepVoice(v, VoiceRow::Monitor, 1); CHECK(v.monitor);
    const MpVoice k = StepVoice(v, VoiceRow::Key, 1); CHECK(k.key == v.key);
}

static void TestPlayers() {
    std::vector<MpPlayer> ps(3);
    ps[0].slot = 0; ps[0].name = "Ann"; ps[0].host = true; ps[0].pingMs = 40; ps[0].route = "direct";
    ps[1].slot = 1; ps[1].name = "Bob"; ps[1].me = true;
    ps[2].slot = 2; ps[2].name = ""; ps[2].muted = true; ps[2].talking = true; ps[2].canVote = true; ps[2].volumePct = 70;
    MpVote none;
    std::vector<PlayerRowView> r = BuildPlayerRows(ps, none);
    CHECK(r.size() == 3);
    CHECK(r[0].name == "Ann (host)" && r[0].detail == "40 ms, direct" && r[0].showMute && r[0].muteLabel == "MUTE");
    CHECK(r[1].name == "Bob (you)" && !r[1].showMute && !r[1].showVolume && !r[1].showVote);
    CHECK(r[2].name == "Player 2" && r[2].muteLabel == "UNMUTE" && r[2].detail == "muted" && !r[2].talking && r[2].volume == "70%");
    CHECK(r[2].showVote && !r[2].showKick && !r[2].showVoteBan);

    // The host's view: kick and ban, no vote button.
    ps[2].canKick = true;
    r = BuildPlayerRows(ps, none);
    CHECK(r[2].showKick && r[2].showBan && !r[2].showVote);
    ps[1].canKick = true; r = BuildPlayerRows(ps, none);
    CHECK(!r[1].showKick && !r[1].showBan);                    // never yourself
    ps[1].canKick = false;

    // A vote running: nobody starts another; its target is marked.
    ps[2].canKick = false;
    MpVote v; v.active = true; v.target = 2;
    r = BuildPlayerRows(ps, v);
    CHECK(!r[2].showVote && r[2].voteTarget && !r[0].voteTarget);
}

static void TestVoteBar() {
    CHECK(!BuildVoteBar(MpVote{}).show);
    MpVote v; v.active = true; v.target = 3; v.name = "Cid"; v.yes = 2; v.no = 1; v.needed = 3; v.secondsLeft = 21; v.mine = 0;
    VoteBar b = BuildVoteBar(v);
    CHECK(b.show && b.showButtons && !b.yesOn && !b.noOn);
    CHECK(b.text == "Kick Cid? 2 of 3 yes needed, 1 no, 21 s left");
    v.mine = 1; b = BuildVoteBar(v); CHECK(b.yesOn && !b.noOn);
    v.mine = -1; b = BuildVoteBar(v); CHECK(b.noOn && !b.yesOn);
    v.mine = -2; b = BuildVoteBar(v); CHECK(!b.showButtons && b.text.find("can't vote") != std::string::npos);
    CHECK(b.text.rfind("Kick Cid? ", 0) == 0);
    v.ban = true; b = BuildVoteBar(v); CHECK(b.text.rfind("Ban Cid for 7 days? ", 0) == 0);
}

int main() {
    TestVoiceRows();
    TestStepVoice();
    TestPlayers();
    TestVoteBar();
    if (g_fail) { std::printf("test_hsm_mp_scene: %d failure(s)\n", g_fail); return 1; }
    std::printf("test_hsm_mp_scene: ok\n");
    return 0;
}
