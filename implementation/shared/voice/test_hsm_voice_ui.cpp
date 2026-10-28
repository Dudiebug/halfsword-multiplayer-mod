// Voice settings (0.7.0-pre6): the host's reach, mic modes, steppers, the meter, key names, devices, per-speaker
// gains, and the ADPCM codec for a picked microphone with its 16 -> 24 kHz upsampler.
#include "voice/hsm_adpcm.h"
#include "voice/hsm_voice_ui.h"
#include <cmath>
#include <cstdio>
#include <vector>

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++g_fail; } } while (0)
using namespace hsm::voice;

static void TestReach() {
    // Off: nobody hears anybody, whatever the fighters' states.
    CHECK(AudibleIn(Reach::Off, true, true, true) == Hear::None);
    CHECK(AudibleIn(Reach::Off, false, false, false) == Hear::None);
    // Proximity: the pre5 rules.
    CHECK(AudibleIn(Reach::Proximity, true, true, true) == Hear::Spatial);
    CHECK(AudibleIn(Reach::Proximity, false, false, false) == Hear::Flat);
    CHECK(AudibleIn(Reach::Proximity, false, false, true) == Hear::None);
    CHECK(AudibleIn(Reach::Proximity, true, false, true) == Hear::None);
    // Everyone: the living are heard flat; the dead still only by the dead, the knocked out by nobody.
    CHECK(AudibleIn(Reach::Everyone, true, true, true) == Hear::Flat);
    CHECK(AudibleIn(Reach::Everyone, true, true, false) == Hear::Flat);
    CHECK(AudibleIn(Reach::Everyone, false, false, true) == Hear::None);
    CHECK(AudibleIn(Reach::Everyone, false, false, false) == Hear::Flat);
    CHECK(AudibleIn(Reach::Everyone, true, false, true) == Hear::None);
    // Everyone relays at any distance (Flat).
    CHECK(RelayVoice(AudibleIn(Reach::Everyone, true, true, true), 100000.f, true, Range{}));
    CHECK(!RelayVoice(AudibleIn(Reach::Proximity, true, true, true), 100000.f, true, Range{}));

    for (Reach r : {Reach::Off, Reach::Proximity, Reach::Everyone}) {
        CHECK(ReachFromIni(ReachIni(r), Reach::Everyone) == r);
        CHECK(StepReach(StepReach(r, 1), -1) == r);
    }
    CHECK(StepReach(Reach::Everyone, 1) == Reach::Off && StepReach(Reach::Off, -1) == Reach::Everyone);
    CHECK(ReachFromIni("loud", Reach::Proximity) == Reach::Proximity);
    CHECK(std::string(ReachName(Reach::Everyone)) == "Everyone");
}

static void TestMic() {
    Mic m = Mic::Open;
    CHECK(MicFromIni("ptt", m) && m == Mic::PushToTalk);
    CHECK(MicFromIni("level", m) && m == Mic::Open);
    CHECK(MicFromIni("off", m) && m == Mic::Off);
    m = Mic::PushToTalk;
    CHECK(!MicFromIni("on", m) && m == Mic::PushToTalk);   // unknown leaves it alone
    for (Mic x : {Mic::Open, Mic::PushToTalk, Mic::Off}) {
        Mic y; CHECK(MicFromIni(MicIni(x), y) && y == x);
        CHECK(StepMic(StepMic(x, -1), 1) == x);
    }
    CHECK(StepMic(Mic::Off, 1) == Mic::Open);
}

static void TestSteps() {
    CHECK(StepLevelDb(-40, 1) == -35 && StepLevelDb(-40, -1) == -45);
    CHECK(StepLevelDb(-10, 1) == -10 && StepLevelDb(-70, -1) == -70);   // ends hold
    CHECK(StepLevelDb(-42, 1) == -40 && StepLevelDb(-42, -1) == -45);   // off the grid: onto it first
    CHECK(StepLevelDb(-200, 1) == -70 && StepLevelDb(50, -1) == -10);
    int db = -70; for (int i = 0; i < 20; ++i) db = StepLevelDb(db, 1);
    CHECK(db == -10);
    CHECK(StepVolumePct(100, 1) == 110 && StepVolumePct(100, -1) == 90);
    CHECK(StepVolumePct(0, -1) == 0 && StepVolumePct(200, 1) == 200);
    CHECK(StepVolumePct(-30, 1) == 0 && StepVolumePct(900, -1) == 200);
    CHECK(StepVolumePct(55, 1) == 60 && StepVolumePct(55, -1) == 50);
    int v = 0; for (int i = 0; i < 30; ++i) v = StepVolumePct(v, 1);
    CHECK(v == 200);
}

static void TestStepsSnap() {
    // Off-grid values snap to the neighbour in the step's direction.
    CHECK(StepVolumePct(57, -1) == 50);
    CHECK(StepVolumePct(57, 1) == 60);
    CHECK(StepLevelDb(-37, 1) == -35);
    CHECK(StepLevelDb(-37, -1) == -40);
}

static void TestMeter() {
    CHECK(MeterFraction(-120.f) == 0.f && MeterFraction(-70.f) == 0.f);
    CHECK(MeterFraction(0.f) == 1.f && MeterFraction(6.f) == 1.f);
    CHECK(std::fabs(MeterFraction(-35.f) - 0.5f) < 1e-6f);
    CHECK(MeterFraction(NAN) == 0.f);
    CHECK(MeterFraction(-40.f) < MeterFraction(-39.f));
}

static void TestKeys() {
    CHECK(ParseVoiceKey("v") == 'V' && ParseVoiceKey("V") == 'V' && ParseVoiceKey("7") == '7');
    CHECK(ParseVoiceKey("f1") == 0x70 && ParseVoiceKey("F12") == 0x7B);
    CHECK(ParseVoiceKey("mouse4") == 0x05 && ParseVoiceKey("Mouse5") == 0x06);
    CHECK(ParseVoiceKey("") == 0 && ParseVoiceKey("f0") == 0 && ParseVoiceKey("f13") == 0 && ParseVoiceKey("f01") == 0);
    CHECK(ParseVoiceKey("space") == 0 && ParseVoiceKey("-") == 0 && ParseVoiceKey("vv") == 0);
    for (int vk : VoiceKeyCandidates()) {
        CHECK(!VoiceKeyIni(vk).empty());
        CHECK(ParseVoiceKey(VoiceKeyIni(vk)) == vk);   // every key the prompt accepts survives the ini
        CHECK(!VoiceKeyLabel(vk).empty());
    }
    CHECK(VoiceKeyCandidates().size() == 26 + 10 + 12 + 2);
    CHECK(VoiceKeyLabel('V') == "V" && VoiceKeyLabel(0x74) == "F5" && VoiceKeyLabel(0x05) == "Mouse 4");
    CHECK(VoiceKeyIni(0x20).empty() && VoiceKeyLabel(0x20).empty());
}

static void TestDevices() {
    const std::vector<std::string> d = {"Headset", "USB Mic"};
    CHECK(DeviceIndex(d, "") == -1 && DeviceIndex(d, "USB Mic") == 1 && DeviceIndex(d, "Gone") == -1);
    CHECK(StepDevice(-1, 2, 1) == 0 && StepDevice(0, 2, 1) == 1 && StepDevice(1, 2, 1) == -1);
    CHECK(StepDevice(-1, 2, -1) == 1 && StepDevice(0, 2, -1) == -1);
    CHECK(StepDevice(-1, 0, 1) == -1 && StepDevice(-1, 0, -1) == -1);   // no devices: stays on default
    CHECK(StepDevice(7, 2, 1) == -1);                                   // stale index clamps first
    CHECK(DeviceLabel(d, -1) == "Windows default" && DeviceLabel(d, 0) == "Headset" && DeviceLabel(d, 5) == "Windows default");
}

static void TestListen() {
    CHECK(ListenGain(Listen{}) == 1.f);
    CHECK(ListenGain(Listen{true, 150}) == 0.f);
    CHECK(ListenGain(Listen{false, 50}) == 0.5f);
    CHECK(ListenGain(Listen{false, 999}) == 2.f && ListenGain(Listen{false, -5}) == 0.f);
}

static double SnrDb(const std::vector<int16_t>& ref, const std::vector<int16_t>& got, size_t n) {
    double sig = 0, err = 0;
    for (size_t i = 0; i < n; ++i) { sig += double(ref[i]) * ref[i]; const double e = double(ref[i]) - got[i]; err += e * e; }
    return err <= 0 ? 200.0 : 10.0 * std::log10(sig / err);
}

static void TestAdpcm() {
    // A voice-like signal: two tones with a swell, 40 ms at 16 kHz.
    std::vector<int16_t> pcm(kAdpcmChunkSamples);
    for (size_t i = 0; i < pcm.size(); ++i) {
        const double t = double(i) / kAdpcmRate, env = 0.3 + 0.7 * double(i) / pcm.size();
        pcm[i] = int16_t(env * (9000.0 * std::sin(2 * M_PI * 220 * t) + 4000.0 * std::sin(2 * M_PI * 1250 * t)));
    }
    const std::vector<uint8_t> enc = EncodeAdpcm(pcm.data(), pcm.size());
    CHECK(enc.size() == kAdpcmHeader + (pcm.size() - 1 + 1) / 2);   // 4 bits a sample after the first
    CHECK(enc.size() == 3 + 320);
    std::vector<int16_t> dec;
    CHECK(DecodeAdpcm(enc.data(), enc.size(), dec));
    CHECK(dec.size() >= pcm.size() && dec.size() <= pcm.size() + 1);
    CHECK(dec[0] == pcm[0]);
    CHECK(SnrDb(pcm, dec, pcm.size()) > 18.0);

    // Silence stays near silence; a full-scale square does not overflow.
    std::vector<int16_t> quiet(320, 0), sq(320);
    for (size_t i = 0; i < sq.size(); ++i) sq[i] = (i / 8) % 2 ? 32767 : -32768;
    auto e2 = EncodeAdpcm(quiet.data(), quiet.size());
    CHECK(DecodeAdpcm(e2.data(), e2.size(), dec));
    int peak = 0; for (int16_t s : dec) peak = std::max(peak, std::abs(int(s)));
    CHECK(peak < 64);
    e2 = EncodeAdpcm(sq.data(), sq.size());
    CHECK(DecodeAdpcm(e2.data(), e2.size(), dec) && dec.size() >= sq.size());

    // Odd and tiny lengths.
    int16_t one = 1234; e2 = EncodeAdpcm(&one, 1);
    CHECK(e2.size() == kAdpcmHeader && !DecodeAdpcm(e2.data(), e2.size(), dec));   // header alone carries nothing to play
    int16_t two[2] = {100, -100}; e2 = EncodeAdpcm(two, 2);
    CHECK(e2.size() == kAdpcmHeader + 1 && DecodeAdpcm(e2.data(), e2.size(), dec) && dec.size() == 3 && dec[0] == 100);
    CHECK(EncodeAdpcm(nullptr, 5).empty() && EncodeAdpcm(pcm.data(), 0).empty());

    // Bad input: short, bad step index; every byte value decodes without trouble.
    const uint8_t badIdx[5] = {0, 0, 89, 0x77, 0x88};
    CHECK(!DecodeAdpcm(badIdx, 5, dec) && dec.empty());
    CHECK(!DecodeAdpcm(enc.data(), 3, dec) && !DecodeAdpcm(nullptr, 10, dec));
    std::vector<uint8_t> all = {0xFF, 0x7F, 88};
    for (int b = 0; b < 256; ++b) all.push_back(uint8_t(b));
    CHECK(DecodeAdpcm(all.data(), all.size(), dec) && dec.size() == 1 + 512);
}

static void TestUpsample() {
    std::vector<int16_t> out;
    const int16_t ramp[4] = {0, 300, 600, 900};
    Upsample16To24(ramp, 4, out);
    CHECK(out.size() == 6);
    // A straight line stays straight: steps of 200 at 24 kHz, the last sample held at the chunk's edge.
    for (size_t i = 0; i < 5; ++i) CHECK(out[i] == int16_t(200 * i));
    CHECK(out[5] == 900);
    const int16_t odd[3] = {10, 20, 30};
    Upsample16To24(odd, 3, out);
    CHECK(out.size() == 4 && out[3] == 30);
    Upsample16To24(nullptr, 3, out); CHECK(out.empty());
    // Rate: 640 samples (40 ms at 16 kHz) become 960 (40 ms at 24 kHz).
    std::vector<int16_t> chunk(kAdpcmChunkSamples, 7);
    Upsample16To24(chunk.data(), chunk.size(), out);
    CHECK(out.size() == chunk.size() * kSampleRate / kAdpcmRate);
    // Extremes do not wrap.
    const int16_t ext[4] = {32767, -32768, 32767, -32768};
    Upsample16To24(ext, 4, out);
    for (int16_t s : out) CHECK(s >= -32768 && s <= 32767);
    CHECK(out[1] < 0 && out[2] > -32768);
}

int main() {
    TestReach(); TestMic(); TestSteps(); TestStepsSnap(); TestMeter(); TestKeys(); TestDevices(); TestListen(); TestAdpcm(); TestUpsample();
    if (g_fail) { std::printf("%d failure(s)\n", g_fail); return 1; }
    std::printf("test_hsm_voice_ui: ok\n");
    return 0;
}
