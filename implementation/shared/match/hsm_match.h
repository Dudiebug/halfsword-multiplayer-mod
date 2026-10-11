// Match rules. Game-independent, unit-tested; the host's session executes what these decide.
//   ResolveTeams  - lobby picks + host overrides + auto-balance -> team per roster slot
//   TeamColor     - tabard tint per team
//   KillLog       - last hostile contact per victim (kill attribution window)
//   Match         - phases, score limit, time limit + sudden death, players-vs-bots loss, respawn schedule
//   PartitionSides / PickRespawn - team spawn sides and respawn points
#pragma once
#include "session/hsm_spawn.h"
#include <cmath>
#include <cstdint>
#include <map>
#include <vector>

namespace hsm {

// PlayersVsBots is "Survival": endless bot waves. Zombies: the same rules with hordes of the game's zombie Willies.
enum class Format : uint8_t { FreeForAll = 0, TwoTeams = 1, ThreeTeams = 2, PlayersVsBots = 3, Duel = 4, Zombies = 5 };
constexpr uint8_t kFormatCount = 6;
enum class Gear : uint8_t { Own = 0, RandomTier = 1, Mirror = 2 };   // PvP gear: own loadout / random by tier / everyone identical
constexpr uint8_t kGearCount = 3;
constexpr int8_t kNoTeam = -1;        // free-for-all: everyone on their own
constexpr int8_t kHumanTeam = 0, kBotTeam = 1;   // players vs bots

struct MatchConfig {
    Format format = Format::FreeForAll;
    uint16_t scoreLimit = 5;
    uint16_t timeLimitS = 0;          // 0 = no time limit
    uint16_t respawnDelayS = 5;
    bool friendlyFire = false;
    Gear gear = Gear::Own;
    uint8_t tier = 2;                 // Enum_Ranks 0..7: random gear tier, and the starting bot tier
};

inline bool IsBotFormat(Format f) { return f == Format::PlayersVsBots || f == Format::Zombies; }
inline int TeamCount(Format f) { return f == Format::TwoTeams ? 2 : f == Format::ThreeTeams ? 3 : IsBotFormat(f) ? 2 : 0; }
inline uint16_t DefaultScoreLimit(Format f) { return f == Format::FreeForAll ? 5 : f == Format::Duel ? 3 : 10; }
MatchConfig Sanitize(MatchConfig c);

// Owner authority: a machine processes a hit only on a body it owns, and between teammates only with friendly fire.
inline bool HitAllowed(bool victimOwnedHere, int8_t attackerTeam, int8_t victimTeam, bool friendlyFire) {
    return victimOwnedHere && !(attackerTeam != kNoTeam && attackerTeam == victimTeam && !friendlyFire);
}

struct TeamRequest { uint8_t slot = 0; int8_t pick = kNoTeam; int8_t hostOverride = kNoTeam; };
std::map<uint8_t, int8_t> ResolveTeams(const std::vector<TeamRequest>& members, Format f);

struct Rgba { float r = 0, g = 0, b = 0, a = 0; };
Rgba TeamColor(int team);             // a == 0: no tint (free-for-all keeps players' own colours)

class KillLog {
public:
    static constexpr uint32_t kWindowMs = 10000;
    void OnHit(uint8_t victim, uint8_t attacker, uint32_t nowMs);
    uint8_t KillerOf(uint8_t victim, uint32_t nowMs) const;   // 0 = nobody within the window
    void Forget(uint8_t entity);
private:
    struct Hit { uint8_t attacker = 0; uint32_t at = 0; };
    std::map<uint8_t, Hit> last_;
};

enum class MatchPhase : uint8_t { Idle = 0, Countdown = 1, Live = 2, SuddenDeath = 3, Results = 4 };

struct FighterInfo { uint8_t entity = 0; int8_t team = kNoTeam; bool human = true; };

class Match {
public:
    static constexpr uint32_t kCountdownMs = 5000, kResultsMs = 10000;
    static constexpr uint32_t kWaveBreakMs = 20000, kZombieBreakMs = 10000, kRoundGapMs = 4000, kCorpseMs = 45000;
    static constexpr int kMaxBotsAlive = 10;
    struct Events {
        bool live = false, ended = false, rematch = false;
        bool waveStarted = false, waveCleared = false, roundReset = false;
        int spawnBots = 0, botRank = 0;  // bot formats: how many bots to spawn now, and their Enum_Ranks tier
        std::vector<uint8_t> respawn;   // entities to respawn now
    };

    void Start(const MatchConfig& cfg, const std::vector<FighterInfo>& fighters, uint32_t nowMs);
    void AddFighter(const FighterInfo& f);              // joins mid-match: alive, no score change
    void RemoveFighter(uint8_t entity);
    void OnDeath(uint8_t victim, uint8_t killer, uint32_t nowMs);   // killer 0 = no attributable attacker
    Events Tick(uint32_t nowMs);
    void MarkRespawned(uint8_t entity);
    void Finish(int winnerKey, uint32_t nowMs);          // ends a running match now (no effect once over)
    bool Has(uint8_t e) const { return fighters_.count(e) != 0; }
    int wave() const { return wave_; }
    int botsLeft() const;
    void SpawnFailed(int n);                            // requested bots that could not be spawned go back to the wave                               // not yet spawned + alive, in the current wave
    std::vector<uint8_t> StaleBotCorpses(uint32_t nowMs, uint32_t ageMs) const;

    MatchPhase phase() const { return phase_; }
    const MatchConfig& config() const { return cfg_; }
    uint32_t TimeLeftMs(uint32_t nowMs) const;          // of the current phase (Live: time limit; 0 = unlimited)
    int ScoreKeyOf(uint8_t entity) const;               // FFA: the entity; teams: its team
    int Score(int key) const { auto it = score_.find(key); return it == score_.end() ? 0 : it->second; }
    const std::map<int, int>& scores() const { return score_; }
    int Kills(uint8_t e) const { auto it = fighters_.find(e); return it == fighters_.end() ? 0 : it->second.kills; }
    int Deaths(uint8_t e) const { auto it = fighters_.find(e); return it == fighters_.end() ? 0 : it->second.deaths; }
    bool Alive(uint8_t e) const { auto it = fighters_.find(e); return it != fighters_.end() && it->second.alive; }
    int8_t TeamOf(uint8_t e) const { auto it = fighters_.find(e); return it == fighters_.end() ? kNoTeam : it->second.team; }
    int winner() const { return winner_; }              // score key of the winner, kNoWinner while undecided
    static constexpr int kNoWinner = -1000;
    size_t size() const { return fighters_.size(); }

private:
    struct F { int8_t team = kNoTeam; bool human = true, alive = true; uint32_t diedAt = 0; int kills = 0, deaths = 0; };
    void AddPoints(int key, int points);
    void End(int winnerKey, uint32_t nowMs);
    int UniqueLeader() const;                           // kNoWinner when tied or empty
    MatchConfig cfg_;
    MatchPhase phase_ = MatchPhase::Idle;
    uint32_t phaseStart_ = 0;
    std::map<uint8_t, F> fighters_;
    std::map<int, int> score_;
    int winner_ = kNoWinner;
    bool endedPending_ = false;
    // bot formats
    void StartWave(uint32_t nowMs, Events& ev);
    int AliveBots() const;
    int wave_ = 0, remaining_ = 0, waveRank_ = 0, spawnOwed_ = 0;   // owed: requested, not yet added
    bool inBreak_ = false, clearedPending_ = false;
    uint32_t breakStart_ = 0;
    std::vector<uint8_t> revive_;
    void ResetModeState();
    // duel
    bool roundOver_ = false;
    uint32_t roundEnd_ = 0;
};

// Wave sizes. Survival: one bot per player plus one per wave (max 30), tier + wave/2.
// Zombies: six per player plus four per wave (max 60), tier + wave/3. Ranks cap at 7.
struct Wave { int count = 0; int rank = 0; };
Wave WaveFor(Format f, int waveIndex, int humans, int tier);

// Team spawn sides: one contiguous angular arc of points per team around the points' centroid.
std::vector<int8_t> PartitionSides(const std::vector<Point3>& points, int teams);
// Index of the point (on `team`'s side when it has any) farthest from the nearest living enemy.
size_t PickRespawn(const std::vector<Point3>& points, const std::vector<int8_t>& sides, int team, const std::vector<Point3>& enemies);

}  // namespace hsm

#include "match/hsm_match_impl.h"
