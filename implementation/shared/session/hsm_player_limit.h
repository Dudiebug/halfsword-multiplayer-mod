// The host's player limit (2..8): the Steam lobby's member limit, the browser's "3/4" and the host's own check at Hello.
// Steam enforces it at join; the host turns away anyone over it too (a modified client may not honour Steam's limit, an
// old steam_api64 may lack the export). It can never be set below the players already in: nobody is kicked by it.
// Game-independent and unit-tested; the Steam calls are in gameplay/lobby.cpp.
#pragma once
#include "session/hsm_roster.h"
#include <algorithm>
#include <cstdint>
#include <string>

namespace hsm {

constexpr int kMinPlayerLimit = 2;
constexpr int kMaxPlayerLimit = kMaxPlayers;   // the roster's fixed 8 stays the hard cap

// The limit the host may set: `want` within 2..8, and never below the players in now.
inline int ClampPlayerLimit(int want, int playersNow) {
    const int lo = std::clamp(playersNow, kMinPlayerLimit, kMaxPlayerLimit);
    return std::clamp(want, lo, kMaxPlayerLimit);
}

// The limit a lobby published (lobby data hsm_max, peer-written): absent or not a number is the old fixed 8.
inline int ParsePlayerLimit(const std::string& s) {
    if (s.empty() || s.size() > 3) return kMaxPlayerLimit;
    int v = 0;
    for (char c : s) { if (c < '0' || c > '9') return kMaxPlayerLimit; v = v * 10 + (c - '0'); }
    return std::clamp(v, kMinPlayerLimit, kMaxPlayerLimit);
}

// The Players stepper: one step up or down within [lowest allowed .. 8], wrapping.
inline int StepPlayerLimit(int cur, int playersNow, int dir) {
    const int lo = ClampPlayerLimit(kMinPlayerLimit, playersNow);
    cur = std::clamp(cur, lo, kMaxPlayerLimit);
    if (dir < 0) return cur <= lo ? kMaxPlayerLimit : cur - 1;
    return cur >= kMaxPlayerLimit ? lo : cur + 1;
}

// A lobby is full for someone not in it when its players have reached the limit.
inline bool LobbyIsFull(int players, int limit) { return players >= limit; }

// The host's Hello check: a newcomer (a key the roster does not hold) is turned away at the limit. A key the roster
// already holds is a repeat Hello or a rejoin within its reservation: its seat is its own.
inline bool RoomForHello(size_t rosterSize, bool alreadyHeld, int limit) {
    return alreadyHeld || int(rosterSize) < limit;
}

}  // namespace hsm
