// Why a Steam lobby call failed (CreateLobby, RequestLobbyList), in a few words for the multiplayer page's status line
// and the log, and how long the mod waits for a call's answer. Pure: the call results are read in steam_api.cpp.
#pragma once
#include <cstdint>
#include <string>

namespace hsm::steam {

// EResult values (steamclientpublic.h) a lobby call answers with.
constexpr int kResultOK = 1, kResultFail = 2, kResultNoConnection = 3, kResultAccessDenied = 15, kResultTimeout = 16,
              kResultLimitExceeded = 25;
// Not Steam's: no EResult to read. kResultNoResult: the call completed but its result could not be read (Steam's IO
// failure, or a call such as the lobby list whose result has no EResult). kResultNoAnswer: it never completed in time.
constexpr int kResultNoResult = 0, kResultNoAnswer = -1;

// A call result that has not completed this long after the call is dropped as failed (kResultNoAnswer), so the page
// does not stay on "Creating lobby..." or "Searching lobbies..." for good. A lobby Steam still makes after that is
// entered late and left again (the stale enter in Lobby::Tick).
constexpr uint64_t kCallTimeoutMs = 20000;
inline bool CallTimedOut(uint64_t startedMs, uint64_t nowMs) { return nowMs - startedMs >= kCallTimeoutMs; }

// "Steam failed (error 2), try again". The number stays in every text, so a screenshot names it.
inline std::string ResultText(int result) {
    const std::string n = " (error " + std::to_string(result) + ")";
    switch (result) {
        case kResultFail: return "Steam failed" + n + ", try again";
        case kResultNoConnection: return "Steam is offline" + n + ", check your connection";
        case kResultAccessDenied: return "Steam refused it" + n;
        case kResultTimeout: return "Steam timed out" + n + ", try again";
        case kResultLimitExceeded: return "too many lobbies made" + n + ", wait a minute";
        case kResultNoResult: return "Steam gave no answer, try again";
        case kResultNoAnswer: return "Steam did not answer in " + std::to_string(kCallTimeoutMs / 1000) + " s, try again";
        default: return "Steam error " + std::to_string(result);
    }
}

}  // namespace hsm::steam
