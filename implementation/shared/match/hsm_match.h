// Match rules. Game-independent, unit-tested; the host's session executes what these decide.
//   ResolveTeams  - lobby picks + host overrides + auto-balance -> team per roster slot
//   TeamColor     - tabard tint per team
//   KillLog       - last hostile contact per victim (kill attribution window)
//   Match         - phases, score limit, time limit + sudden death, players-vs-bots loss, respawn schedule, Duel rounds
//   RegearFor     - which respawns bring a new generated loadout
//   PartitionSides / PickRespawn - team spawn sides and respawn points
#pragma once
#include "session/hsm_spawn.h"
#include <cmath>
#include <cstdint>
#include <map>
#include <set>
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
// Equipment Budget per tier, from BP_GameManager.Generate_Random_Characters: random gear is rolled within it.
inline constexpr int kTierBudget[8] = {0, 15, 50, 100, 250, 444, 666, 888};

inline bool IsBotFormat(Format f) { return f == Format::PlayersVsBots || f == Format::Zombies; }
inline int TeamCount(Format f) { return f == Format::TwoTeams ? 2 : f == Format::ThreeTeams ? 3 : IsBotFormat(f) ? 2 : 0; }
inline uint16_t DefaultScoreLimit(Format f) { return f == Format::FreeForAll ? 5 : f == Format::Duel ? 3 : 10; }
MatchConfig Sanitize(MatchConfig c);

// Generated gear at a respawn: None keeps the slot's loadout, Each gives every respawning fighter a fresh one, Shared
// gives them all one fresh loadout. Own gear and the bot formats never regear; Mirror rolls only when everybody is reset
// together, so in free-for-all and teams the shared loadout lasts until the rematch.
enum class Regear : uint8_t { None = 0, Each = 1, Shared = 2 };
enum class RespawnCause : uint8_t { Individual = 0, RoundReset = 1, WaveCleared = 2, Rematch = 3 };
Regear RegearFor(const MatchConfig& c, RespawnCause cause);

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
    static constexpr uint32_t kWaveBreakMs = 60000, kZombieBreakMs = 10000, kCorpseMs = 45000;   // Duel's gap: respawnDelayS
    static constexpr uint32_t kLootDelayMs = 2000;   // Survival: a fallen bot sheds its loot this long after it fell
    static constexpr int kMaxBotsAlive = 10;
    static constexpr int kBotsPerTick = 1;   // the host ticks once a frame; a whole horde in one frame took seconds (test 5)
    struct Events {
        bool live = false, ended = false, rematch = false;
        bool waveStarted = false, waveCleared = false, roundReset = false;
        int spawnBots = 0, botRank = 0;  // bot formats: how many bots to spawn now (at most kBotsPerTick), and their Enum_Ranks tier
        std::vector<uint8_t> respawn;   // entities to respawn now
        Regear regear = Regear::None;   // with respawn or rematch: the gear they come back with (RegearFor)
    };

    void Start(const MatchConfig& cfg, const std::vector<FighterInfo>& fighters, uint32_t nowMs);
    void AddFighter(const FighterInfo& f);              // joins mid-match: alive, no score change
    void RemoveFighter(uint8_t entity);
    void OnDeath(uint8_t victim, uint8_t killer, uint32_t nowMs);   // killer 0 = no attributable attacker
    // Duel: the fighter gives up the round being fought (held G); it goes as if they had fallen, with no kill credited.
    // False, and no effect, outside Duel, outside a fought round (countdown, round gap, results) or for a downed fighter.
    bool Forfeit(uint8_t entity, uint32_t nowMs);
    // Duel: the round being fought is drawn (nobody can finish it, hsm_draw.h): over now with no point to anyone, and the
    // usual gap before every fighter respawns. False, and no effect, outside Duel or outside a fought round.
    bool Draw(uint32_t nowMs);
    bool RoundInPlay() const;                           // Duel: a round is being fought (Live or SuddenDeath, not the gap)
    Events Tick(uint32_t nowMs);
    void MarkRespawned(uint8_t entity);
    void Finish(int winnerKey, uint32_t nowMs);          // ends a running match now (no effect once over)
    bool Has(uint8_t e) const { return fighters_.count(e) != 0; }
    int wave() const { return wave_; }
    int round() const { return round_; }                // Duel: 1 at the start and the rematch, +1 per reset; else 0
    // How many generated loadouts the coming respawns take, to be made ahead while nobody fights (the Duel round gap,
    // the results before a rematch): the game's generator stalls the frame about 110 ms a loadout (test 13). Else 0.
    size_t LoadoutsAhead() const;
    uint8_t roundWinner() const { return roundWinner_; }   // Duel's gap: who took the round, 0 = drawn / not in the gap
    uint32_t RoundGapLeftMs(uint32_t nowMs) const;      // Duel: time left before the next round, 0 outside the gap
    // Bot formats' break between waves: time left (0 outside a break), and the players' votes to skip it (held G). Once
    // every player in the match voted, the next Tick starts the wave. VoteSkip: false, and no effect, outside a break,
    // for a bot or a stranger, or a second time.
    uint32_t BreakLeftMs(uint32_t nowMs) const;
    bool VoteSkip(uint8_t entity, uint32_t nowMs);
    size_t SkipVotes() const { return skipVotes_.size(); }
    size_t SkipVotesNeeded() const;
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
    Events Advance(uint32_t nowMs);                     // Tick without the regear decision
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
    std::set<uint8_t> skipVotes_;
    uint32_t BreakMs() const { return cfg_.format == Format::Zombies ? kZombieBreakMs : kWaveBreakMs; }
    std::vector<uint8_t> revive_;
    void ResetModeState();
    // duel
    bool roundOver_ = false;
    uint32_t roundEnd_ = 0;
    int round_ = 0;
    uint8_t roundWinner_ = 0;
};

// Hold-to-give-up: Update with whether the key is down each frame; true once, on the frame the hold reaches kHoldMs.
// Another give-up needs the key released first. progress() is 0..1 for the HUD while held, 0 once fired or released.
class GiveUpHold {
public:
    static constexpr uint32_t kHoldMs = 1500;
    bool Update(bool down, uint32_t nowMs);
    float progress() const { return progress_; }
private:
    bool holding_ = false, fired_ = false;
    uint32_t since_ = 0;
    float progress_ = 0.0f;
};

// Generated loadouts made ahead, for one tier at a time: another tier empties it.
// Before the match starts, while its fighters come in: how many loadouts its start will generate for `humans` players
// (the start regears like a rematch, RegearFor), so the host can make them ahead in that wait.
size_t StartLoadoutsAhead(const MatchConfig& c, size_t humans);

class GearPool {
public:
    bool Wants(uint8_t tier, size_t ahead);                 // true while it holds fewer than `ahead` for `tier`
    void Put(uint8_t tier, std::vector<uint8_t> blob);      // an empty blob (a failed generation) is not kept
    std::vector<uint8_t> Take(uint8_t tier);                // the oldest one made for `tier`; empty when none
    size_t size() const { return blobs_.size(); }
private:
    void Use(uint8_t tier);
    uint8_t tier_ = 0xFF;
    std::vector<std::vector<uint8_t>> blobs_;
};

// What clears the dropped weapons and loose armour: Duel each new round, free-for-all and teams each rematch, the bot
// formats (fought over the fallen bots' loot) a minute after each piece was dropped (session/hsm_lease.h).
enum class DropClear : uint8_t { EachRound = 0, EachMatch = 1, AfterIdle = 2 };
inline DropClear DropClearFor(Format f) {
    return f == Format::Duel ? DropClear::EachRound : IsBotFormat(f) ? DropClear::AfterIdle : DropClear::EachMatch;
}
// True once at the moment each machine clears (from the MatchInfo it sees): Duel's round moving on from one already seen
// (the next round, or the rematch's round 1); free-for-all and teams' results giving way to the rematch. Never in the
// bot formats. Another format, or Duel's round 0 (no match), forgets what was seen.
class DropClearWatch {
public:
    bool Observe(Format f, uint8_t phase, uint16_t round);
private:
    bool any_ = false;
    Format format_ = Format::FreeForAll;
    uint8_t phase_ = 0;
    uint16_t seen_ = 0;
};

// Survival loot: which of a fallen bot's `pieces` worn armour pieces it sheds, at most `max` distinct indices, picked
// by `seed` (the same seed, the same picks).
constexpr size_t kLootArmorPieces = 2;
std::vector<size_t> LootPicks(size_t pieces, size_t max, uint32_t seed);

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
