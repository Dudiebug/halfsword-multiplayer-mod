// Unit tests for hsm_start_gate.h. Run via implementation/shared/run_tests.sh test_hsm_start_gate.
#include "match/hsm_start_gate.h"
#include <cstdio>

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++g_fail; } } while (0)
using namespace hsm;

static StartInputs Host(std::vector<PeerBuild> peers = {}) {
    StartInputs in;
    in.host = true; in.mine = "0.8.0 #aaaaaaaaaaaa";
    in.peers = std::move(peers);
    return in;
}
static const char* kMine = "0.8.0 #aaaaaaaaaaaa";

// True when `s` is well-formed UTF-8 (no half character at the cut).
static bool ValidUtf8(const std::string& s) {
    for (size_t i = 0; i < s.size();) {
        const uint8_t c = uint8_t(s[i]);
        const size_t n = c < 0x80 ? 1 : (c >> 5) == 0x6 ? 2 : (c >> 4) == 0xE ? 3 : (c >> 3) == 0x1E ? 4 : 0;
        if (n == 0 || i + n > s.size()) return false;
        for (size_t k = 1; k < n; ++k) if ((uint8_t(s[i + k]) & 0xC0) != 0x80) return false;
        i += n;
    }
    return true;
}

static void TestStartable() {
    StartVerdict v = EvaluateStart(Host());
    CHECK(v.ok && v.why == StartBlock::None && v.text.empty() && v.line.empty() && !v.warn);
    v = EvaluateStart(Host({{"Bob", kMine, true}, {"Cara", kMine, false}}));   // every build matches
    CHECK(v.ok && v.why == StartBlock::None && v.line.empty());
}

static void TestNotHost() {
    StartInputs in = Host({{"Bob", kMine, false}});
    in.host = false;
    StartVerdict v = EvaluateStart(in);
    CHECK(!v.ok && v.why == StartBlock::NotHost);
    CHECK(v.text == "Only the host starts the match." && v.line == "Waiting for the host to start." && !v.warn);
    in.peers = {{"Bob", "0.7.0", false}};            // a member is still told about another build, in words that fit any side
    v = EvaluateStart(in);
    CHECK(!v.ok && v.why == StartBlock::NotHost && v.warn);
    CHECK(v.text == "Only the host starts the match.");
    CHECK(v.line == "Bob is on a different build. Everyone needs the same build to start.");
    in.allowMismatch = true;                         // a member's own INI does not turn START on
    CHECK(!EvaluateStart(in).ok);
}

static void TestNoMode() {
    StartInputs in = Host();
    in.hasMode = false;
    const StartVerdict v = EvaluateStart(in);
    CHECK(!v.ok && v.why == StartBlock::NoMode);
    CHECK(v.text == "The game is still loading. Try again in a moment." && v.line == v.text);
    in.host = false;                                 // a member is a member first
    CHECK(EvaluateStart(in).why == StartBlock::NotHost);
}

static void TestOlderBuild() {
    // A joined peer whose Hello carried no build is on an older HSMMod.
    StartVerdict v = EvaluateStart(Host({{"Dag", "", true}}));
    CHECK(!v.ok && v.why == StartBlock::OlderBuild && v.warn);
    CHECK(v.text == "Dag is on an older build. Everyone has to update before you can start.");
    CHECK(v.line == v.text);
    // Two or more: the first is named, the rest counted.
    v = EvaluateStart(Host({{"Dag", "", true}, {"Eve", "", true}}));
    CHECK(v.why == StartBlock::OlderBuild && v.text == "Dag and 1 other are on older builds. Everyone has to update before you can start.");
    v = EvaluateStart(Host({{"Dag", "", true}, {"Eve", "", true}, {"Fay", "", true}}));
    CHECK(v.text == "Dag and 2 others are on older builds. Everyone has to update before you can start.");
    v = EvaluateStart(Host({{"", "", true}}));       // no name: still a sentence
    CHECK(v.text == "Another player is on an older build. Everyone has to update before you can start.");
}

static void TestNotJoinedYetPasses() {
    // A member whose Hello has not come yet (no build, not joined) never blocks START.
    StartVerdict v = EvaluateStart(Host({{"Dag", "", false}}));
    CHECK(v.ok && v.why == StartBlock::None && !v.warn && v.line.empty());
    v = EvaluateStart(Host({{"Dag", "", false}, {"Bob", kMine, true}}));
    CHECK(v.ok);
    // ... and once it joins with no build, it does.
    v = EvaluateStart(Host({{"Dag", "", true}, {"Bob", kMine, true}}));
    CHECK(!v.ok && v.why == StartBlock::OlderBuild);
    // A joined peer on the same build is fine.
    CHECK(EvaluateStart(Host({{"Bob", kMine, true}})).ok);
}

static void TestOtherBuild() {
    StartVerdict v = EvaluateStart(Host({{"Bob", "0.7.0", false}}));   // from the Steam member data: not joined needed
    CHECK(!v.ok && v.why == StartBlock::OtherBuild && v.warn);
    CHECK(v.text == "Bob is on a different build. Everyone needs the same build to start." && v.line == v.text);
    v = EvaluateStart(Host({{"Bob", "0.7.0", true}, {"Cara", "0.7.1", true}}));
    CHECK(v.text == "Bob and 1 other are on different builds. Everyone needs the same build to start.");
    // Some older, some other: "different" is true of both.
    v = EvaluateStart(Host({{"Dag", "", true}, {"Bob", "0.7.0", true}}));
    CHECK(v.why == StartBlock::OtherBuild && v.text == "Dag and 1 other are on different builds. Everyone needs the same build to start.");
    // The first one in the list is named, whatever its kind.
    v = EvaluateStart(Host({{"Bob", "0.7.0", true}, {"Dag", "", true}}));
    CHECK(v.text.rfind("Bob and 1 other", 0) == 0);
}

static void TestOverride() {
    // allow_build_mismatch=1: START works, and the line says what it overrode.
    StartInputs in = Host({{"Dag", "", true}});
    in.allowMismatch = true;
    StartVerdict v = EvaluateStart(in);
    CHECK(v.ok && v.why == StartBlock::OlderBuild && v.warn && v.text.empty());
    CHECK(v.line == "Dag is on an older build. You allowed it, so you can still start.");
    in.peers = {{"Bob", "0.7.0", true}};
    v = EvaluateStart(in);
    CHECK(v.ok && v.why == StartBlock::OtherBuild && v.line == "Bob is on a different build. You allowed it, so you can still start.");
    // The words the player reads never name the INI key.
    CHECK(v.line.find("allow_build_mismatch") == std::string::npos);
    // The override does not lift the other blocks.
    in.modsChecking = true;
    CHECK(!EvaluateStart(in).ok && EvaluateStart(in).why == StartBlock::ModsChecking);
    in.modsChecking = false; in.modeOk = false; in.modeHint = "Needs 2 or more players";
    CHECK(!EvaluateStart(in).ok && EvaluateStart(in).why == StartBlock::ModeUnavailable);
}

static void TestModsChecking() {
    StartInputs in = Host();
    in.modsChecking = true;
    StartVerdict v = EvaluateStart(in);
    CHECK(!v.ok && v.why == StartBlock::ModsChecking && !v.warn);
    CHECK(v.text == "Checking the map pack. Try again in a moment." && v.line == "Checking the map pack.");
}

static void TestModeUnavailable() {
    StartInputs in = Host();
    in.modeOk = false; in.modeHint = "Needs 2 or more players";
    StartVerdict v = EvaluateStart(in);
    CHECK(!v.ok && v.why == StartBlock::ModeUnavailable);
    CHECK(v.text == "Needs 2 or more players. Invite someone or pick another mode." && v.line == "Needs 2 or more players.");
    in.modeHint = "Duel is for 2 players";            // too many cannot be fixed by inviting
    v = EvaluateStart(in);
    CHECK(v.text == "Duel is for 2 players. Pick another mode." && v.line == "Duel is for 2 players.");
    in.modeHint.clear();
    v = EvaluateStart(in);
    CHECK(!v.ok && v.text == "This mode cannot start yet. Pick another mode.");
    in.progression = true;                            // the Progression card has no player rule
    CHECK(EvaluateStart(in).ok);
}

static void TestOrder() {
    // Builds first, then the map pack, then the player count (the order the page reads them in).
    StartInputs in = Host({{"Dag", "", true}});
    in.modsChecking = true; in.modeOk = false; in.modeHint = "Needs 2 or more players";
    CHECK(EvaluateStart(in).why == StartBlock::OlderBuild);
    in.peers.clear();
    CHECK(EvaluateStart(in).why == StartBlock::ModsChecking);
    in.modsChecking = false;
    CHECK(EvaluateStart(in).why == StartBlock::ModeUnavailable);
    in.hasMode = false;
    CHECK(EvaluateStart(in).why == StartBlock::NoMode);
    in.host = false;
    CHECK(EvaluateStart(in).why == StartBlock::NotHost);
}

static void TestTextCap() {
    // Every text fits the chat line's cut, long names included, and is never cut inside a character.
    CHECK(kStartTextMax == 120);
    const std::string longAscii(300, 'x');
    std::string twoByte, threeByte;
    for (int i = 0; i < 80; ++i) { twoByte += "\xC3\xA9"; threeByte += "\xE2\x82\xAC"; }   // e-acute, euro sign
    for (const std::string& name : {longAscii, twoByte, threeByte}) {
        for (bool older : {true, false}) {
            for (size_t others : {size_t(0), size_t(1), size_t(25), size_t(120)}) {
                std::vector<PeerBuild> peers{{name, older ? "" : "0.7.0", true}};
                for (size_t k = 0; k < others; ++k) peers.push_back({"p", older ? "" : "0.7.0", true});
                StartInputs in = Host(peers);
                StartVerdict v = EvaluateStart(in);
                CHECK(v.text.size() <= kStartTextMax && v.line.size() <= kStartTextMax && ValidUtf8(v.text) && ValidUtf8(v.line));
                CHECK(v.text.size() > 60);           // the name is cut, not the sentence
                CHECK(v.text.find("before you can start.") != std::string::npos || v.text.find("same build to start.") != std::string::npos);
                in.allowMismatch = true;
                v = EvaluateStart(in);
                CHECK(v.ok && v.line.size() <= kStartTextMax && ValidUtf8(v.line));
                CHECK(v.line.find("so you can still start.") != std::string::npos);
                in.host = false;
                v = EvaluateStart(in);
                CHECK(v.line.size() <= kStartTextMax && ValidUtf8(v.line) && v.line.find("to start.") != std::string::npos);
            }
        }
    }
    StartInputs in = Host();
    in.modeOk = false; in.modeHint = std::string(300, 'h');
    StartVerdict v = EvaluateStart(in);
    CHECK(v.text.size() <= kStartTextMax && v.line.size() <= kStartTextMax);
    in.modeHint = "Needs " + twoByte + threeByte;
    v = EvaluateStart(in);
    CHECK(v.text.size() <= kStartTextMax && v.line.size() <= kStartTextMax && ValidUtf8(v.text) && ValidUtf8(v.line));
    // The cut itself.
    CHECK(CutBytes("abc", 5) == "abc" && CutBytes("abcdef", 3) == "abc");
    CHECK(CutBytes("ab\xC3\xA9", 3) == "ab");        // would split the e-acute
    CHECK(CutBytes("ab \xC3\xA9", 3) == "ab");       // and the space before the cut goes
    CHECK(CutBytes("abc", 0).empty());
    // Every fixed line fits too.
    for (const char* s : {kStartNotHost, kStartWaitHost, kStartNoMode, kStartChecking, kStartCheckingClick, kOnlyHostChanges})
        CHECK(std::string(s).size() <= kStartTextMax);
}

static void TestDisabledWhy() {
    CHECK(DisabledWhy(false, "Needs 2 or more players") == "Only the host changes this.");   // a client: always the host's
    CHECK(DisabledWhy(false, "") == "Only the host changes this.");
    CHECK(DisabledWhy(true, "Needs 2 or more players") == "Needs 2 or more players");        // the host: the control's own reason
    CHECK(DisabledWhy(true, "").empty());
}

int main() {
    TestStartable();
    TestNotHost();
    TestNoMode();
    TestOlderBuild();
    TestNotJoinedYetPasses();
    TestOtherBuild();
    TestOverride();
    TestModsChecking();
    TestModeUnavailable();
    TestOrder();
    TestTextCap();
    TestDisabledWhy();
    if (g_fail) { std::printf("test_hsm_start_gate: %d failure(s)\n", g_fail); return 1; }
    std::printf("test_hsm_start_gate: ok\n");
    return 0;
}
