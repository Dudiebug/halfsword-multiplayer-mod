// The live check of the game names HSMMod uses (gameplay/game_check.cpp). tools/gen_game_manifest.py lists each name with
// the class that owns it in the Half Sword build the mod was verified on (generated/hsm_game_manifest.h); at run time each
// one is looked up in the game as its class loads. A Half Sword update can rename or remove a Blueprint field without any
// error: 26641406 renamed ModularWeaponBP_C's "Parent Actor" to "Parent Actors", and about 30 reads came back empty. The
// mod keeps running (multiplayer stays on); a miss is logged and the multiplayer page says the game changed.
// A miss counts only when a second look at least kGameCheckConfirmMs later still misses, so a class caught while it is
// still loading never shows as changed. Pure; the DLL does the lookups.
#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace hsm {

enum class GameCheckKind : uint8_t { Property, Function, NativeFunction, Param };

struct GameCheckEntry {
    GameCheckKind kind;
    const char* owner;    // the class; "" for a native function (and its parameters), which is looked up by name
    const char* name;     // the field or the function
    const char* member;   // Param: the parameter
    const char* type;     // Property: the FField classes it may have, "A|B"; "" is not checked
    const char* source;   // the mod file that uses it first
};

// What one look found. Skipped: a parameter of a function that is not there (the function is the miss, counted once).
enum class GameCheckLook : uint8_t { Ok, Missing, WrongType, Skipped };

constexpr uint32_t kGameCheckConfirmMs = 5000;

// `expected` is "" (anything goes) or alternatives split by '|'; `actual` must be one of them, whole.
inline bool GameCheckTypeOk(std::string_view expected, std::string_view actual) {
    if (expected.empty()) return true;
    size_t at = 0;
    while (at <= expected.size()) {
        const size_t bar = expected.find('|', at);
        const size_t end = bar == std::string_view::npos ? expected.size() : bar;
        if (expected.substr(at, end - at) == actual) return true;
        if (bar == std::string_view::npos) break;
        at = bar + 1;
    }
    return false;
}

inline GameCheckLook GameCheckLookProperty(bool found, std::string_view expectedType, std::string_view actualType) {
    if (!found) return GameCheckLook::Missing;
    return GameCheckTypeOk(expectedType, actualType) ? GameCheckLook::Ok : GameCheckLook::WrongType;
}

inline GameCheckLook GameCheckLookParam(bool functionFound, bool paramFound) {
    if (!functionFound) return GameCheckLook::Skipped;
    return paramFound ? GameCheckLook::Ok : GameCheckLook::Missing;
}

// Each entry's verdict. Ok and Skipped are final at the first look; a miss is Suspect until a look at least
// kGameCheckConfirmMs later agrees (Missing or WrongType, final) or finds it (Ok).
enum class GameCheckState : uint8_t { Unchecked, Suspect, Ok, Missing, WrongType, Skipped };

class GameCheckTally {
public:
    explicit GameCheckTally(size_t entries) : state_(entries, GameCheckState::Unchecked), since_(entries, 0) {}

    size_t Size() const { return state_.size(); }
    GameCheckState State(size_t i) const { return i < state_.size() ? state_[i] : GameCheckState::Unchecked; }
    // Unchecked or Suspect: the next look at this entry still counts.
    bool Open(size_t i) const { const GameCheckState s = State(i); return i < state_.size() && (s == GameCheckState::Unchecked || s == GameCheckState::Suspect); }

    // True when this look confirms a miss (log it, once).
    bool Record(size_t i, GameCheckLook look, uint32_t nowMs) {
        if (!Open(i)) return false;
        GameCheckState& s = state_[i];
        if (look == GameCheckLook::Ok || look == GameCheckLook::Skipped) {
            if (s == GameCheckState::Suspect) --suspects_;
            s = look == GameCheckLook::Ok ? GameCheckState::Ok : GameCheckState::Skipped;
            if (look == GameCheckLook::Ok) ++ok_;
            return false;
        }
        if (s == GameCheckState::Unchecked) { s = GameCheckState::Suspect; since_[i] = nowMs; ++suspects_; return false; }
        if (uint32_t(nowMs - since_[i]) < kGameCheckConfirmMs) return false;
        s = look == GameCheckLook::WrongType ? GameCheckState::WrongType : GameCheckState::Missing;
        --suspects_;
        ++changed_;
        return true;
    }

    int Changed() const { return changed_; }   // confirmed misses: the number the notice gives
    int Ok() const { return ok_; }
    int Suspects() const { return suspects_; }

private:
    std::vector<GameCheckState> state_;
    std::vector<uint32_t> since_;
    int ok_ = 0, changed_ = 0, suspects_ = 0;
};

// "game check: ModularWeaponBP_C.Parent Actor missing (gameplay/adapter.cpp)"
inline std::string GameCheckLogLine(const GameCheckEntry& e, GameCheckState s, std::string_view actualType = {}) {
    const std::string owner = e.owner && *e.owner ? std::string(e.owner) + "." : std::string();
    std::string what;
    switch (e.kind) {
        case GameCheckKind::Property:
            what = owner + e.name + (s == GameCheckState::WrongType
                                         ? " is " + std::string(actualType) + ", not " + e.type
                                         : std::string(" missing"));
            break;
        case GameCheckKind::Function: what = owner + e.name + " (function) missing"; break;
        case GameCheckKind::NativeFunction: what = std::string("native function ") + e.name + " missing"; break;
        case GameCheckKind::Param: what = owner + e.name + " has no parameter " + e.member; break;
    }
    return "game check: " + what + " (" + e.source + ")";
}

// The multiplayer page's line; empty while nothing is confirmed changed. steamBuild 0: Steam did not say.
inline std::string GameCheckNotice(int steamBuild, int changed) {
    if (changed <= 0) return "";
    const std::string build = steamBuild > 0 ? " (" + std::to_string(steamBuild) + ")" : "";
    return "This Half Sword build" + build + " changed " + std::to_string(changed) + (changed == 1 ? " thing" : " things") +
           " HSMMod uses; some features may not work. Look for an HSMMod update.";
}

}  // namespace hsm
