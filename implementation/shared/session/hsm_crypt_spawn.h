// The host spawns the shared Crypts' enemies for every player (docs/PLAN_CRYPTS_COOP.md, "the host drives the dungeon").
// The game's room fills itself once, from its Event Populate Level With Characters (BP_Procedural_Block_Master.cpp:616), and
// that event only knows the host's own body: a spawner point within 5 m of player 0 or 1, or in their line of sight, is
// skipped, and the event never runs again (a DoOnce). In the shared run a client walks into a room while the host stands a few
// metres off, every point is "seen" by the host, and the room stays empty for good (0.8.0: "enemies are not spawning").
// So the host blocks the room's own event and spawns through the director's Spawn Character with these rules:
//   - PlanSpawns: which of a room's points to use: its Spawn Chance roll passed (rolled once per point, as the game does),
//     not within kSpawnClearCm of ANY player (host and clients alike), not in sight of any player, and no more than the
//     director's cap leaves room for;
//   - NextStep: a room whose points were all refused because a player was near or saw them (or the cap was full) is asked
//     again every kSpawnRetryMs, kSpawnTries times, then gets one point: the refused one farthest from every player;
//   - RoomTeam: the room's "Spawned Characters Team", the game's table from the tier (the boss arena's is fixed per tier);
//   - InFront: the game's own point filter (a point in the room's front half), used when no point lies in the room's boxes.
// Game-independent and unit-tested (session/test_hsm_crypt_spawn.cpp).
#pragma once
#include "session/hsm_spawn.h"
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace hsm::crypt {

constexpr double kSpawnClearCm = 500;     // Populate Level With Characters: a point this near a player is skipped
constexpr double kSpawnTraceUpCm = 90;    // its sight trace starts this far above the point
constexpr int kDirectorCap = 5;           // the director spawns while Spawned Willies holds 4 or fewer: 5 at most
constexpr uint32_t kSpawnRetryMs = 2000;
constexpr int kSpawnTries = 10;

inline double Dist(const Point3& a, const Point3& b) { return std::sqrt((a.x - b.x) * (a.x - b.x) + (a.y - b.y) * (a.y - b.y) + (a.z - b.z) * (a.z - b.z)); }

struct SpawnPlan {
    std::vector<size_t> use;       // indexes into the points, in order
    std::vector<size_t> refused;   // passed the roll, refused because a player was near or saw it
    int chance = 0, nearby = 0, seen = 0, capped = 0;
};

// `points`: the room's spawner points. `passed[i]`: point i's Spawn Chance roll (the same roll on every try). `players`: every
// player's body, the host's included. `living`: the director's Spawned Willies now. `sees(i, j)`: player j sees point i (a clear
// line from kSpawnTraceUpCm above it to the player; the engine's trace).
template <class Sees>
SpawnPlan PlanSpawns(const std::vector<Point3>& points, const std::vector<bool>& passed, const std::vector<Point3>& players, int living, Sees sees,
                     double clearCm = kSpawnClearCm, int cap = kDirectorCap) {
    SpawnPlan p;
    int room = cap - living;
    for (size_t i = 0; i < points.size(); ++i) {
        if (i >= passed.size() || !passed[i]) { ++p.chance; continue; }
        bool near = false, saw = false;
        for (size_t j = 0; j < players.size() && !near; ++j) near = Dist(points[i], players[j]) <= clearCm;
        for (size_t j = 0; j < players.size() && !near && !saw; ++j) saw = sees(i, j);
        if (near || saw) { ++(near ? p.nearby : p.seen); p.refused.push_back(i); continue; }
        if (room <= 0) { ++p.capped; continue; }
        p.use.push_back(i); --room;
    }
    return p;
}

// The refused point farthest from its nearest player (the last resort after kSpawnTries).
inline std::optional<size_t> FallbackPoint(const std::vector<Point3>& points, const SpawnPlan& p, const std::vector<Point3>& players) {
    std::optional<size_t> best;
    double bestD = -1;
    for (size_t i : p.refused) {
        if (i >= points.size()) continue;
        double d = 1e300;
        for (const Point3& q : players) d = std::fmin(d, Dist(points[i], q));
        if (d > bestD) { bestD = d; best = i; }
    }
    return best;
}

// One room's spawning: started when a player first stands in its SpawnCharacters box.
struct RoomSpawn {
    bool started = false, done = false;
    int tries = 0;
    uint32_t next = 0;
    bool Due(uint32_t now) const { return started && !done && int32_t(now - next) >= 0; }
};

enum class SpawnStep : uint8_t { Done = 0, Retry = 1, Fallback = 2, GiveUp = 3 };
inline const char* SpawnStepName(SpawnStep s) {
    switch (s) {
        case SpawnStep::Done: return "done";
        case SpawnStep::Retry: return "retry";
        case SpawnStep::Fallback: return "fallback";
        case SpawnStep::GiveUp: return "give up";
    }
    return "?";
}

// After a plan was tried and `spawned` fighters came of it: Done (one spawned, or nothing was waiting: every roll failed, as an
// empty room in the game), Retry (asked again at r.next; also when planned points spawned nothing: the director's own cap
// check or a failed call), Fallback (the tries are used up: spawn at FallbackPoint) or GiveUp (only the cap was in the way).
inline SpawnStep NextStep(RoomSpawn& r, const SpawnPlan& p, int spawned, uint32_t now) {
    ++r.tries;
    if (spawned > 0 || (p.use.empty() && p.refused.empty() && p.capped == 0)) { r.done = true; return SpawnStep::Done; }
    if (r.tries < kSpawnTries) { r.next = now + kSpawnRetryMs; return SpawnStep::Retry; }
    r.done = true;
    return p.refused.empty() && p.use.empty() ? SpawnStep::GiveUp : SpawnStep::Fallback;
}

// The room's "Spawned Characters Team" for a room whose door is not barricaded (BP_Procedural_Block_Master.cpp:617-623).
// `pick(n)` is a random index in [0, n). A tier the table does not list gives 0, as the Blueprint's default.
template <class Pick>
int RoomTeam(int tier, bool bossArena, Pick pick) {
    if (bossArena) return tier < 0 || tier > 8 ? 0 : tier <= 2 ? 2 : tier <= 5 ? 4 : 5;
    static constexpr int k0[] = {0, 0, 3}, k12[] = {0, 3}, k34[] = {0, 3, 2, 4}, k5[] = {3, 2, 5, 4}, k68[] = {2, 5, 4};
    auto from = [&](const int* t, int n) { int i = pick(n); return t[i < 0 || i >= n ? 0 : i]; };
    switch (tier) {
        case 0: return from(k0, 3);
        case 1: case 2: return from(k12, 2);
        case 3: case 4: return from(k34, 4);
        case 5: return from(k5, 4);
        case 6: case 7: case 8: return from(k68, 3);
        default: return 0;
    }
}

// The game's own point filter: in front of the room (local x > 0, the actor's yaw in degrees).
inline bool InFront(const Point3& roomAt, double yawDeg, const Point3& p) {
    const double r = yawDeg * 3.14159265358979323846 / 180.0;
    return (p.x - roomAt.x) * std::cos(r) + (p.y - roomAt.y) * std::sin(r) > 0;
}

}  // namespace hsm::crypt
