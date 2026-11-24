// The START gate (pure, no engine types): may the host start the match, and if not, why, in plain words. One rule for the
// multiplayer page (the START button, the line under it) and the host's session (StartOnMap, StartProgression), so the
// two cannot disagree and a refused START always says why. Fed by the page's data on the page and by the session's peers
// in the session: a joined peer (its Hello came) with no build is "on an older build"; a member whose Hello has not come
// yet never blocks (session/hsm_build.h BuildStateOf).
#pragma once
#include "hsm_chat.h"
#include "session/hsm_build.h"
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace hsm {

// Why START is not available. None: it is, or it is, only because the host allows other builds (then `why` still names the
// build kind and `warn` is set).
enum class StartBlock : uint8_t { None, NotHost, NoMode, ModeUnavailable, OlderBuild, OtherBuild, ModsChecking };

// A reason is shown through the chat line, which cuts at kMaxChatBytes: every text here fits, on a code-point boundary.
constexpr size_t kStartTextMax = 120;
static_assert(kStartTextMax <= kMaxChatBytes, "a START reason must survive the chat line's cut");

inline constexpr const char* kStartNotHost = "Only the host starts the match.";
inline constexpr const char* kStartWaitHost = "Waiting for the host to start.";
inline constexpr const char* kStartNoMode = "The game is still loading. Try again in a moment.";
inline constexpr const char* kStartChecking = "Checking the map pack.";
inline constexpr const char* kStartCheckingClick = "Checking the map pack. Try again in a moment.";
inline constexpr const char* kStartUpdateTail = "Everyone has to update before you can start.";
inline constexpr const char* kStartMatchTail = "Everyone needs the same build to start.";
inline constexpr const char* kStartAllowedTail = "You allowed it, so you can still start.";
// A client's click on a row, card or arrow only the host changes.
inline constexpr const char* kOnlyHostChanges = "Only the host changes this.";

struct StartInputs {
    bool host = false;                  // this machine hosts the lobby
    bool hasMode = true;                // the game is up far enough to start a level (the session's game mode exists)
    bool progression = false;           // the Progression card: no player count rule
    bool allowMismatch = false;         // allow_build_mismatch=1: the host starts with other builds in
    bool modsChecking = false;          // the picked modded map's pak is still being hashed
    bool modeOk = true;                 // the mode takes this many players (ModeAvailable().ok) ...
    std::string modeHint;               // ... and if not, its hint ("Needs 2 or more players")
    std::string mine;                   // this machine's build (ModBuildString)
    std::vector<PeerBuild> peers;       // the other players
};

struct StartVerdict {
    bool ok = false;                    // START may be clicked
    StartBlock why = StartBlock::None;
    std::string text;                   // the reason a click on the dimmed START shows; empty when ok
    std::string line;                   // the standing line under START; empty: nothing to say
    bool warn = false;                  // another player is on another build (the line reads as a warning)
};

// At most `max` bytes, cut on a code-point boundary (never half a character), trailing spaces dropped.
inline std::string CutBytes(const std::string& s, size_t max) {
    if (s.size() <= max) return s;
    size_t cut = max;
    while (cut > 0 && (static_cast<uint8_t>(s[cut]) & 0xC0) == 0x80) --cut;   // back up to a lead byte
    std::string out = s.substr(0, cut);
    while (!out.empty() && out.back() == ' ') out.pop_back();
    return out;
}

// "Dag is on an older build. <tail>", "Dag and 1 other are on older builds. <tail>"; the first name is cut to fit the cap.
inline std::string BuildSentence(const std::vector<std::string>& names, bool older, const char* tail) {
    const size_t more = names.empty() ? 0 : names.size() - 1;
    std::string rest = more == 0 ? " is" : " and " + (more == 1 ? std::string("1 other") : std::to_string(more) + " others") + " are";
    rest += older ? (more ? " on older builds." : " on an older build.") : (more ? " on different builds." : " on a different build.");
    rest += std::string(" ") + tail;
    const std::string first = names.empty() || names[0].empty() ? std::string("Another player") : names[0];
    return CutBytes(CutBytes(first, kStartTextMax > rest.size() ? kStartTextMax - rest.size() : 0) + rest, kStartTextMax);
}

inline StartVerdict EvaluateStart(const StartInputs& in) {
    StartVerdict v;
    std::vector<std::string> names;     // everyone on another build, in order
    bool allOlder = true;
    for (const PeerBuild& p : in.peers) {
        const BuildState st = BuildStateOf(in.mine, p);
        if (st != BuildState::Older && st != BuildState::Other) continue;
        names.push_back(p.name);
        if (st == BuildState::Other) allOlder = false;
    }
    const bool builds = !names.empty();
    const StartBlock kind = allOlder ? StartBlock::OlderBuild : StartBlock::OtherBuild;
    if (!in.host) {                     // a member waits; it is still told about another build in the lobby
        v.why = StartBlock::NotHost;
        v.text = kStartNotHost;
        v.line = builds ? BuildSentence(names, false, kStartMatchTail) : std::string(kStartWaitHost);
        v.warn = builds;
        return v;
    }
    if (!in.hasMode) {
        v.why = StartBlock::NoMode;
        v.text = v.line = kStartNoMode;
        return v;
    }
    if (builds && !in.allowMismatch) {
        v.why = kind;
        v.text = v.line = BuildSentence(names, allOlder, allOlder ? kStartUpdateTail : kStartMatchTail);
        v.warn = true;
        return v;
    }
    if (in.modsChecking) {
        v.why = StartBlock::ModsChecking;
        v.text = kStartCheckingClick;
        v.line = kStartChecking;
        return v;
    }
    if (!in.progression && !in.modeOk) {
        const std::string hint = in.modeHint.empty() ? std::string("This mode cannot start yet") : in.modeHint;
        v.why = StartBlock::ModeUnavailable;
        v.text = CutBytes(hint + (hint.rfind("Needs", 0) == 0 ? ". Invite someone or pick another mode." : ". Pick another mode."), kStartTextMax);
        v.line = CutBytes(hint + ".", kStartTextMax);
        return v;
    }
    v.ok = true;
    if (builds) {                       // allowed by the host: START works, and the line says what it overrode
        v.why = kind;
        v.line = BuildSentence(names, allOlder, kStartAllowedTail);
        v.warn = true;
    }
    return v;
}

// A click on a dimmed control: a client reads that only the host changes it; the host reads the control's own reason
// (empty: it has none to give).
inline std::string DisabledWhy(bool host, const std::string& own) {
    return host ? own : std::string(kOnlyHostChanges);
}

}  // namespace hsm
