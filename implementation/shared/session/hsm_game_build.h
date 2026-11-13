// The Half Sword Steam build this HSMMod is made for, told by Steam's own build id (ISteamApps::GetAppBuildId).
// The exe key (ue/engine.cpp kBuilds) cannot tell every update apart: builds 26615445 and 26641406 changed only the content
// pak, and their HalfSwordUE5-Win64-Shipping.exe is byte for byte the one of 26568321 (sha256 726c10f7...). The content
// differs in ways a session feels (26615445: Willie_BP's grab rule, the middle mouse fighter spawn, the zombies' hearing;
// 26641406: a weapon lists its holders in "Parent Actors"; docs/REVERSE_ENGINEERING.md sections 12 and 13), so the Steam
// build id is checked as well: a notice on the multiplayer page when this machine is on another
// build, and a lobby whose host is on another build is not joinable. Steam may not say (0): then the exe key alone decides.
// Pure; the DLL reads the id and shows the text.
#pragma once
#include <string>
#include <string_view>

namespace hsm {

constexpr int kTargetSteamBuild = 26641406;

enum class SteamBuildCheck : unsigned char { Unknown, Target, Older, Newer };

inline SteamBuildCheck CheckSteamBuild(int id) {
    if (id <= 0) return SteamBuildCheck::Unknown;
    if (id == kTargetSteamBuild) return SteamBuildCheck::Target;
    return id < kTargetSteamBuild ? SteamBuildCheck::Older : SteamBuildCheck::Newer;
}

// The multiplayer page's line for this machine's build; empty on the target build or when Steam cannot say.
inline std::string SteamBuildNotice(int id) {
    switch (CheckSteamBuild(id)) {
        case SteamBuildCheck::Older:
            return "Half Sword is not up to date (Steam build " + std::to_string(id) + "). Update it in Steam: this HSMMod is made "
                   "for build " + std::to_string(kTargetSteamBuild) + ".";
        case SteamBuildCheck::Newer:
            return "Half Sword has an update (Steam build " + std::to_string(id) + ") newer than this HSMMod (made for build " +
                   std::to_string(kTargetSteamBuild) + "). Look for an HSMMod update.";
        default: return "";
    }
}

// A Steam build id written by another machine (lobby data "hsm_sbuild"): 1 to 9 digits, else 0 (unknown).
inline int ParseSteamBuild(std::string_view s) {
    if (s.empty() || s.size() > 9) return 0;
    int v = 0;
    for (char c : s) {
        if (c < '0' || c > '9') return 0;
        v = v * 10 + (c - '0');
    }
    return v;
}

// Why a lobby cannot be joined for its host's game build; empty when both are on one build or either cannot say (an older
// HSMMod's lobby has no "hsm_sbuild"; the exe key check still applies to it).
inline std::string LobbySteamBuildReason(int mine, int host) {
    if (mine <= 0 || host <= 0 || mine == host) return "";
    if (mine < host) return "update Half Sword in Steam (the host is on build " + std::to_string(host) + ")";
    return "the host's Half Sword is not up to date (build " + std::to_string(host) + ")";
}

}  // namespace hsm
