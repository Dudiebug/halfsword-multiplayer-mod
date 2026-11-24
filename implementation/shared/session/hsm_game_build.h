// The Half Sword Steam build this HSMMod was last verified on, told by Steam's own build id (ISteamApps::GetAppBuildId).
// It is not a limit: a newer build runs, and gameplay/game_check.cpp says in game what it changed that the mod uses
// (the multiplayer page's "changed K things" line). tools/game_watch.py moves this number when a new build passes the
// offline gates.
// The exe key (ue/engine.cpp kBuilds) cannot tell every update apart: builds 26615445, 26641406, 26737211 and 26820056 changed only the
// content pak, and their HalfSwordUE5-Win64-Shipping.exe is byte for byte the one of 26568321 (sha256 726c10f7...). The content
// differs in ways a session feels (26615445: Willie_BP's grab rule, the middle mouse fighter spawn, the zombies' hearing;
// 26641406: a weapon lists its holders in "Parent Actors"; 26737211: Willie_BP's dislocation and blood events; 26820056: Get Damage x1.125, the win and lose screens;
// docs/REVERSE_ENGINEERING.md sections 12, 13 and 15), so the Steam
// build id is checked as well: a notice on the multiplayer page when this machine is on an older
// build, and a lobby whose host is on another build is not joinable. Steam may not say (0): then the exe key alone decides.
// Pure; the DLL reads the id and shows the text.
#pragma once
#include <string>
#include <string_view>

namespace hsm {

constexpr int kVerifiedSteamBuild = 26820056;

enum class SteamBuildCheck : unsigned char { Unknown, Verified, Older, Newer };

inline SteamBuildCheck CheckSteamBuild(int id) {
    if (id <= 0) return SteamBuildCheck::Unknown;
    if (id == kVerifiedSteamBuild) return SteamBuildCheck::Verified;
    return id < kVerifiedSteamBuild ? SteamBuildCheck::Older : SteamBuildCheck::Newer;
}

// The multiplayer page's line for this machine's build: only an older build has one (update the game). A newer build
// gets none here: the game check's line says whether it changed anything the mod uses.
inline std::string SteamBuildNotice(int id) {
    if (CheckSteamBuild(id) != SteamBuildCheck::Older) return "";
    return "Half Sword is not up to date (Steam build " + std::to_string(id) + "). Update it in Steam: this HSMMod was checked "
           "on build " + std::to_string(kVerifiedSteamBuild) + ".";
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
