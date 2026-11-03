// Match rules. Game-independent, unit-tested; the host's session executes what these decide.
//   ResolveTeams  - lobby picks + host overrides + auto-balance -> team per roster slot
//   TeamColor     - tabard tint per team
//   KillLog       - last hostile contact per victim (kill attribution window)
//   Match         - phases, score limit, time limit + sudden death, players-vs-bots loss, respawn schedule, Duel rounds
//   RegearFor     - which respawns bring a new generated loadout
//   PartitionSides / PickRespawn - team spawn sides and respawn points
#pragma once
#include "match/hsm_bracket.h"
#include "match/hsm_crossbow.h"
#include "match/hsm_damage.h"
#include "match/hsm_kingqueue.h"
#include "match/hsm_scenarios.h"
#include "session/hsm_spawn.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace hsm {

// PlayersVsBots is "Survival": endless bot waves. Zombies: the same rules with the game's zombie Willies, one at a time
// per player. TwoTeams is "Teams": MatchConfig::teams (2..4) says how many; ThreeTeams is only the old value of 3 teams
// (Sanitize turns it into TwoTeams with teams = 3), never produced. LastManStanding: Duel's rounds for any number of
// fighters; Duel itself is for exactly 2. The team rounds (IsTeamRound) take Duel's rounds to teams: the last TEAM
// standing takes the round, scores are per team. TeamDuel: MatchConfig::teams (2..4) teams, picked in the lobby. Riot and
// Juggernaut: two fixed teams that the roles decide (match/hsm_roles.h). CaptureTheFlag: two fixed teams picked in the
// lobby, scored in captures (match/hsm_ctf.h); kills score nothing. Wave 3 (match/hsm_modes.h): Tag (rounds, one fighter
// is "it" and carries a fuse; no teams), Infection (survivors against the infected: two fixed teams, the infected respawn),
// Crossbow TDM (Teams with a crossbow for everyone). Tournament (14): a 1v1 bracket over everybody, the pairing being fought
// and the rest on the bench (match/hsm_bracket.h). PitBridgeDuel (15): the Pit's bridge, the winner stays and the loser goes
// to the back of the queue (match/hsm_kingqueue.h). Both are round formats with a bench: IsQueueFormat.
enum class Format : uint8_t { FreeForAll = 0, TwoTeams = 1, ThreeTeams = 2, PlayersVsBots = 3, Duel = 4, Zombies = 5, LastManStanding = 6,
                              TeamDuel = 7, Riot = 8, Juggernaut = 9, CaptureTheFlag = 10, Tag = 11, Infection = 12, CrossbowTDM = 13,
                              Tournament = 14, PitBridgeDuel = 15 };
constexpr uint8_t kFormatCount = 16;
constexpr int kMinTeams = 2, kMaxTeams = 4;
// PvP gear: own loadout / random by tier / everyone identical / Fists: nobody wears or holds anything (the Brawl card,
// free-for-all only). Fists is no choice of the Gear row (kGearRowCount): the mode card sets it.
enum class Gear : uint8_t { Own = 0, RandomTier = 1, Mirror = 2, Fists = 3 };
constexpr uint8_t kGearCount = 4, kGearRowCount = 3;
// The Weapons row: which weapons the generated gear (RandomTier, Mirror) may hold, by the game's own weapon-type filter
// (GameWeaponTypeByte). Any leaves the game's fields alone.
enum class WeaponFilter : uint8_t { Any = 0, Swords1H = 1, Swords2H = 2, Polearms = 3, AxesMaces = 4 };
constexpr uint8_t kWeaponFilterCount = 5;
// Our filter -> the byte the game's weapon-type fields take (GI "Free Mode Opponents Weapons Type", "NPC Weapons Type";
// Enum_WeaponType). ASSUMPTION pending the in-game test: Enum_WeaponType runs 0 Any, 1 1H swords, 2 2H swords, 3 Polearms,
// 4 Axes & maces. The enum's names are not known offline; this table is the one place to correct once they are.
inline constexpr uint8_t kGameWeaponTypeByte[kWeaponFilterCount] = {0, 1, 2, 3, 4};
inline uint8_t GameWeaponTypeByte(WeaponFilter w) { return kGameWeaponTypeByte[uint8_t(w) < kWeaponFilterCount ? uint8_t(w) : 0]; }
inline const char* WeaponFilterName(WeaponFilter w) {
    switch (w) {
        case WeaponFilter::Swords1H: return "1H swords";
        case WeaponFilter::Swords2H: return "2H swords";
        case WeaponFilter::Polearms: return "Polearms";
        case WeaponFilter::AxesMaces: return "Axes & maces";
        default: return "Any";
    }
}
// The Team colours row: what takes the team's colour on every machine's own copy of a fighter (match/hsm_team_kit.h).
// Tabard: the tabard only. Full: clothes, tabard, plate (darkened), shield and grips; blades stay steel. Off: nothing.
enum class TeamKit : uint8_t { Off = 0, Tabard = 1, Full = 2 };
constexpr uint8_t kTeamKitCount = 3;
// A team's name on the HUD (RED, BLUE, GREEN, GOLD; "-" for none). Here, not in hsm_hud_text.h, so the objective layer
// (match/hsm_objective.h, which the wire format includes) can name teams too.
inline std::string TeamName(int team) {
    static const char* kNames[] = {"RED", "BLUE", "GREEN", "GOLD"};
    return team >= 0 && team < 4 ? kNames[team] : "-";
}
constexpr int8_t kNoTeam = -1;        // free-for-all: everyone on their own
constexpr int8_t kHumanTeam = 0, kBotTeam = 1;   // players vs bots

struct MatchConfig {
    Format format = Format::FreeForAll;
    uint8_t teams = 2;                // the TwoTeams format: how many teams, kMinTeams..kMaxTeams (Sanitize)
    uint16_t scoreLimit = 5;
    uint16_t timeLimitS = 0;          // 0 = no time limit
    uint16_t respawnDelayS = 5;
    bool friendlyFire = false;
    Gear gear = Gear::Own;
    uint8_t tier = 2;                 // Enum_Ranks 0..7: random gear tier, and the starting bot tier
    uint8_t crossbows = 0;            // hsm::Crossbows: how many random players get a crossbow at each spawn (lobby key hsm_xbow; experimental)
    WeaponFilter weapons = WeaponFilter::Any;   // random / identical gear only: the weapon types it may roll (lobby key hsm_wpn); host-side, never on the wire
    TeamKit teamKit = TeamKit::Full;  // team formats: what takes the team colour (lobby key hsm_kit, MatchInfo.teamKit); every machine tints its own copies
    uint8_t fuseS = 30;               // Tag: the fuse "it" carries, seconds (kFuseMinS..kFuseMaxS, lobby key hsm_fuse); host-side, never on the wire
    uint16_t damagePct = kDamageStock;   // the Player Damage Rate every machine runs in the session (hsm_damage.h)
    uint16_t waveBreakS = 0;          // bot formats: the pause between waves (kWaveBreakOptions); 0 = the format's own (Match::kWaveBreakMs, kZombieBreakMs)
    uint8_t clearGroundMin = 0;       // free-for-all, teams, Brawl: every this many minutes each machine clears the dropped weapons and loose armour (kClearGroundOptions); 0 = never
    bool freeCam = true;              // dead players may fly a free camera (F, lobby key hsm_fcam); false: they only follow the living (MatchInfo.freeCam)
    uint8_t ctfCaptures = 3;          // Capture the flag: captures to win, 1..10 (lobby key hsm_ctfcap); Sanitize makes it scoreLimit
    uint8_t flagReturnS = 20;         // Capture the flag: seconds a dropped flag lies before it goes home, 10..60 (lobby key hsm_ctfret); host-side, never on the wire
    uint8_t scenario = 0;             // hsm::Scenario: the world settings for the picked map (lobby key hsm_scen); host-side, the clients get the settings with the Travel, never a wire field
    bool hostPhysics = false;         // 0.9.0: the host runs every fighter's physics and every screen shows its copy (SyncMode::Host); false: each player its own (SyncMode::Owner)
};
inline constexpr uint16_t kWaveBreakOptions[4] = {10, 30, 60, 120};   // seconds
inline constexpr uint8_t kClearGroundOptions[4] = {0, 2, 3, 5};       // minutes, 0 = off
inline bool ValidWaveBreakS(uint16_t s) { return s == 0 || s == 10 || s == 30 || s == 60 || s == 120; }
inline bool ValidClearGroundMin(uint8_t m) { return m == 0 || m == 2 || m == 3 || m == 5; }
// Brawl: free-for-all where every fighter spawns bare (no armour, no weapons) and fights with fists. A mode card, not a
// Format: the rules are free-for-all's.
inline bool IsBrawl(Format f, Gear g) { return f == Format::FreeForAll && g == Gear::Fists; }
inline bool IsBrawl(const MatchConfig& c) { return IsBrawl(c.format, c.gear); }
// Every body spawns bare (no armour, no weapons): Brawl, and Tag (always bare-fisted, 0.8.0-pre26).
inline bool IsBare(Gear g) { return g == Gear::Fists; }
inline bool IsBare(const MatchConfig& c) { return IsBare(c.gear); }
// Tag's Player damage when the Tag card is picked: light hits, the fuse decides the round (the host can change it).
inline constexpr uint16_t kTagDamagePct = 10;
// Equipment Budget per tier, from BP_GameManager.Generate_Random_Characters: random gear is rolled within it.
inline constexpr int kTierBudget[8] = {0, 15, 50, 100, 250, 444, 666, 888};
inline constexpr uint8_t kBrawlTier = 5;            // Brawl's fixed tier: every build Weight 0.75 (no Tier row, no gear)

// A fighter's build is the passport's Weight (0..1, like Height). The game derives the body from it (Willie_BP): the
// Muscle Rate is the Weight, the body's mass scale runs 1..1.25 over it and the grip's hold limit 1..1.5. So two players
// with different Weights are two different bodies, and a Free Mode passport can hold a Weight outside 0..1 (2.0 made one
// fighter 33% heavier with a 60% stronger grip than a 0.5 one; 0.8.0-pre19 reports #294 #295 #301).
// The game's own bot table (BP_Generator_Characters_Random), by Enum_Ranks tier: a player at tier N is built like a tier-N bot.
inline constexpr double kTierWeight[8] = {0.0, 0.15, 0.30, 0.45, 0.60, 0.75, 0.90, 1.0};
inline constexpr double kDefaultWeight = 0.5;       // the game's default passport (GI_Settings); a NaN Weight gets it
inline double TierWeight(int tier) { return kTierWeight[std::clamp(tier, 0, 7)]; }
// The Weight every player is forced to: the tier's (Brawl is always kBrawlTier).
inline double ForcedWeight(Format, Gear, int tier) { return TierWeight(tier); }
inline double ForcedWeight(const MatchConfig& c) { return ForcedWeight(c.format, c.gear, c.tier); }
// The Weight a fighter spawns with. on: exactly `forced` (up or down; whatever the passport holds, NaN too). off (a bot, or
// force_strength=0): their own, clamped to 0..1 so an out-of-range passport cannot spawn a stronger body; NaN: the default.
inline double BuildWeight(double own, double forced, bool on) {
    if (on) return std::isfinite(forced) ? std::clamp(forced, 0.0, 1.0) : kDefaultWeight;
    return std::isfinite(own) ? std::clamp(own, 0.0, 1.0) : kDefaultWeight;
}
// What a Weight does in the game (Willie_BP: MapRangeUnclamped over the Muscle Rate), for the log and the tests.
inline double MassScaleFor(double weight) { return 1.0 + 0.25 * weight; }
inline double GripScaleFor(double weight) { return 1.0 + 0.5 * weight; }

inline bool IsBotFormat(Format f) { return f == Format::PlayersVsBots || f == Format::Zombies; }
// The gear settings a format takes: own / random by tier / identical anywhere (Survival and Zombies too, 0.8.0-pre20);
// Fists only for the Brawl card (free-for-all) and Tag.
inline bool GearAllowed(Format f, Gear g) { return g == Gear::Fists ? f == Format::FreeForAll || f == Format::Tag : uint8_t(g) < kGearCount; }
// Tournament and Pit Bridge Duels: two fighters fight at a time, everyone else sits on the bench (no body, spectating).
inline bool IsQueueFormat(Format f) { return f == Format::Tournament || f == Format::PitBridgeDuel; }
// Rounds between teams: the last team standing takes the round (Team Duel, Riot, Juggernaut).
inline bool IsTeamRound(Format f) { return f == Format::TeamDuel || f == Format::Riot || f == Format::Juggernaut; }
// Wave 3: Tag is Duel's rounds for any number of fighters (the last one standing takes the round); Infection has two fixed teams
// (survivors 0, infected 1) that the mode moves fighters between; Crossbow TDM is Teams with a crossbow for everyone.
inline bool IsTag(Format f) { return f == Format::Tag; }
inline bool IsInfection(Format f) { return f == Format::Infection; }
inline bool IsCrossbowTdm(Format f) { return f == Format::CrossbowTDM; }
constexpr int8_t kSurvivorTeam = 0, kInfectedTeam = 1;   // Infection
constexpr int kInfectedKitTeam = 6;                      // TeamColor / the team kit's colour index of the infected: a dark green of their own (hsm_team_kit.h KitTeamFor)
// Riot and Juggernaut give each fighter a role (match/hsm_roles.h): the role sets its gear, tier, build and team.
inline bool HasRoles(Format f) { return f == Format::Riot || f == Format::Juggernaut; }
// Capture the flag: two teams take each other's flag home (match/hsm_ctf.h); a score is a capture, a kill scores nothing.
inline bool IsCtf(Format f) { return f == Format::CaptureTheFlag; }
constexpr int kCtfMinCaptures = 1, kCtfMaxCaptures = 10, kCtfDefaultCaptures = 3;
constexpr int kFlagReturnMinS = 10, kFlagReturnMaxS = 60, kFlagReturnDefaultS = 20;
// The formats with teams: Teams (ThreeTeams is the old value of 3 teams, which still counts so a stale one cannot lose its
// colours) and the team rounds.
inline bool HasTeams(Format f) { return f == Format::TwoTeams || f == Format::ThreeTeams || IsTeamRound(f) || IsCrossbowTdm(f) || IsInfection(f) || IsCtf(f); }
// The teams the lobby's Teams / My team rows and Auto-balance decide; Riot and Juggernaut's come from the roles, Infection's from the infection.
inline bool PicksTeams(Format f) { return HasTeams(f) && !HasRoles(f) && !IsInfection(f); }
// The Duel rules: rounds, the last one standing takes the round (Duel for two fighters, Last Man Standing for any number;
// the team rounds count the last team).
inline bool IsRoundFormat(Format f) { return f == Format::Duel || f == Format::LastManStanding || IsTag(f) || IsTeamRound(f) || IsQueueFormat(f); }
// Scores kept per fighter (free-for-all, Duel, Last Man Standing) rather than per team.
inline bool ScoresPerFighter(Format f) { return f == Format::FreeForAll || (IsRoundFormat(f) && !IsTeamRound(f)); }
// Teams in a match: `teams` (2..4) for Teams, Team Duel and Crossbow TDM, 3 for the old ThreeTeams value, 2 for Riot, Juggernaut,
// Infection and the bot formats, else 0.
inline int TeamCount(Format f, int teams) {
    return f == Format::TwoTeams || f == Format::TeamDuel || IsCrossbowTdm(f) ? std::clamp(teams, kMinTeams, kMaxTeams) : f == Format::ThreeTeams ? 3
         : HasRoles(f) || IsInfection(f) || IsBotFormat(f) || IsCtf(f) ? 2 : 0;
}
constexpr uint8_t kFuseMinS = 10, kFuseMaxS = 60, kFuseDefaultS = 30;   // Tag's fuse, seconds
constexpr uint16_t kInfectionDefaultTimeS = 300;                          // Infection: the survivors win when this runs out (0 is no choice)
// The score limit is the rounds to win a fight (Tournament: always 1, one round a fight since 0.8.0-pre26), the wins to take
// the match (Pit Bridge Duels), else rounds or kills.
inline uint16_t DefaultScoreLimit(Format f) { return f == Format::FreeForAll || f == Format::PitBridgeDuel ? 5 : f == Format::Tournament ? 1 : IsRoundFormat(f) || IsCtf(f) ? 3 : 10; }
// The lobby key a format keeps its limit under, and the value the key holds: Tournament's "Rounds per fight" (hsm_trnr: 1, 3 or
// 5; the limit is the rounds to win), Pit Bridge Duels' "Wins to take the match" (hsm_pbwin), else hsm_lim.
inline const char* LimitKeyFor(Format f) { return f == Format::Tournament ? "hsm_trnr" : f == Format::PitBridgeDuel ? "hsm_pbwin" : "hsm_lim"; }
inline int LimitToKey(Format f, int scoreLimit) { return f == Format::Tournament ? 2 * scoreLimit - 1 : scoreLimit; }
inline int LimitFromKey(Format f, int value) { return f == Format::Tournament ? std::clamp((value + 1) / 2, 1, 3) : value; }
MatchConfig Sanitize(MatchConfig c);

// Generated gear at a respawn: None keeps the slot's loadout, Each gives every respawning fighter a fresh one, Shared
// gives them all one fresh loadout. Own gear and Brawl never regear; Mirror rolls only when everybody is reset
// together, so in free-for-all and teams the shared loadout lasts until the rematch. The bot formats roll at the run's
// start (and a rematch) only: a player keeps that loadout through every wave and respawn.
enum class Regear : uint8_t { None = 0, Each = 1, Shared = 2 };
enum class RespawnCause : uint8_t { Individual = 0, RoundReset = 1, WaveCleared = 2, Rematch = 3 };
Regear RegearFor(const MatchConfig& c, RespawnCause cause);
// A fighter joining a running match: RandomTier gets a fresh loadout of its own, whatever the format (Mirror takes the
// shared one, if there is one). The bot formats' RegearFor rolls at the start only.
Regear JoinRegearFor(const MatchConfig& c);

// Owner authority: a machine processes a hit only on a body it owns, and between teammates only with friendly fire.
inline bool HitAllowed(bool victimOwnedHere, int8_t attackerTeam, int8_t victimTeam, bool friendlyFire) {
    return victimOwnedHere && !(attackerTeam != kNoTeam && attackerTeam == victimTeam && !friendlyFire);
}

struct TeamRequest { uint8_t slot = 0; int8_t pick = kNoTeam; int8_t hostOverride = kNoTeam; };
std::map<uint8_t, int8_t> ResolveTeams(const std::vector<TeamRequest>& members, Format f, int teams);

struct Rgba { float r = 0, g = 0, b = 0, a = 0; };
Rgba TeamColor(int team);             // a == 0: no tint (free-for-all keeps players' own colours)

class KillLog {
public:
    static constexpr uint32_t kWindowMs = 60000;   // a bleed-out often comes long after the last blow (CSLewin's PR #1: 60 s lost kills)
    void OnHit(uint8_t victim, uint8_t attacker, uint32_t nowMs);
    uint8_t KillerOf(uint8_t victim, uint32_t nowMs) const;   // 0 = nobody within the window
    void Forget(uint8_t entity);
    void ForgetAttacker(uint8_t entity);   // K3: hits it landed are not credited to whoever takes its slot next
private:
    struct Hit { uint8_t attacker = 0; uint32_t at = 0; };
    std::map<uint8_t, Hit> last_;
};

// A free-for-all or team fighter held G while downed (HandleGiveUp marks the time); the death it then causes lands
// within this window and is scored as a give-up: the last attacker (KillLog) gets the kill, nobody else scores.
inline constexpr uint32_t kGaveUpWindowMs = 3000;
inline bool GaveUpDeath(uint32_t markedAtMs, uint32_t nowMs) {
    const uint32_t d = nowMs - markedAtMs;                 // wrap-safe; a mark ahead of now (clock stepped back) is stale
    return d <= kGaveUpWindowMs;
}

enum class MatchPhase : uint8_t { Idle = 0, Countdown = 1, Live = 2, SuddenDeath = 3, Results = 4 };
inline bool CombatPhase(MatchPhase phase) { return phase == MatchPhase::Live || phase == MatchPhase::SuddenDeath; }   // blows count

// id: the player's stable identity (SteamID; 0 = none: bots, direct-IP peers), which keeps their score across a leave and
// rejoin and across a host migration. The entity is only the slot they hold now.
struct FighterInfo { uint8_t entity = 0; int8_t team = kNoTeam; bool human = true; uint64_t id = 0; };

// A match's scores by identity, for the next host after a host migration (session/hsm_migration.h builds it from the last
// MatchInfo): each player's score (free-for-all / Duel), kills and deaths by SteamID, the team scores by team. Match::Start
// takes it in and gives each player theirs back as they come in; it applies only to the same format.
struct PlayerScore { uint64_t id = 0; int score = 0, kills = 0, deaths = 0; };
struct ScoreCarry {
    Format format = Format::FreeForAll;
    std::vector<PlayerScore> players;
    std::map<int, int> teams;
    bool empty() const { return players.empty() && teams.empty(); }
};

class Match {
public:
    static constexpr uint32_t kCountdownMs = 5000, kResultsMs = 10000;
    static constexpr uint32_t kWaveBreakMs = 60000, kZombieBreakMs = 10000, kCorpseMs = 45000;   // Duel's gap: respawnDelayS
    static constexpr uint32_t kLootDelayMs = 2000;   // Survival: a fallen bot sheds its loot this long after it fell
    static constexpr int kMaxBotsAlive = 10;   // Zombies: one per player (MaxBotsAlive), at most this
    static constexpr int kBotsPerTick = 1;   // the host ticks once a frame; a whole horde in one frame took seconds (test 5)
    struct Events {
        bool live = false, ended = false, rematch = false;
        bool waveStarted = false, waveCleared = false, roundReset = false;
        int spawnBots = 0, botRank = 0;  // bot formats: how many bots to spawn now (at most kBotsPerTick), and their Enum_Ranks tier
        std::vector<uint8_t> respawn;   // entities to respawn now
        std::vector<uint8_t> bench;     // Tournament, Pit Bridge Duels: with roundReset, the fighters who go to the bench now (their body is taken away, they spectate)
        Regear regear = Regear::None;   // with respawn or rematch: the gear they come back with (RegearFor)
    };

    void Start(const MatchConfig& cfg, const std::vector<FighterInfo>& fighters, uint32_t nowMs, const ScoreCarry& carry = {});
    // Zombies: how many zombies may be alive at once, one per player in the session (the host's own fighter too), at
    // least one and at most kMaxBotsAlive; the host sets the count every tick from its roster. Other formats: kMaxBotsAlive.
    void SetPlayers(int players) { players_ = players; }
    void SetResultsMs(uint32_t ms) { resultsMs_ = ms; }   // how long the results show before the rematch (Progression: 3 s, then the Tavern)
    int MaxBotsAlive() const;
    // Joins mid-match: alive; a player with an id comes back with the score they left with. Duel is for two: a third human
    // joining a fought Duel is down and out of every round (AwaitsRespawn, so the host spawns no body for it) until the match
    // ends, and the rematch is Last Man Standing.
    void AddFighter(const FighterInfo& f);
    // A fighter leaves for good. Its score goes with the slot (the slot's next player starts at 0), but a player with an id has
    // it kept under that id until the rematch (AddFighter), so a rejoin on any slot gets it back. Team scores stay with the team.
    void RemoveFighter(uint8_t entity);
    // K1/K2: a fighter who left (or is held for a rejoin) in a fought phase falls with no kill credited (a Duel round goes to
    // whoever is left; Survival: the last human gone ends it) and is neither respawned nor counted until Return/RemoveFighter.
    // False, and no effect, outside a fought phase, for an unknown fighter or one already down (it is still marked gone).
    bool Leave(uint8_t entity, uint32_t nowMs);
    void SetTeam(uint8_t entity, int8_t team);          // Riot / Juggernaut: the roles are dealt again at a rematch, so a fighter changes sides
    void Return(uint8_t entity);                        // a leaver is back: it stays down until the next respawn / round
    // K1/K2: a human the match counts down in a fought phase (a leaver back within its reservation, or any fighter that fell)
    // has no body in the round: a rejoiner is not spawned until the match respawns it (Duel and wave formats: the next
    // round or wave; free-for-all and teams: the respawn delay), and spectates meanwhile. False for a fighter still gone.
    bool AwaitsRespawn(uint8_t e) const;
    bool Gone(uint8_t e) const { return gone_.count(e) != 0; }
    // Tournament, Pit Bridge Duels: on the bench (not in the pairing being fought, or a late joiner): no body, spectating. True in
    // the countdown too, where AwaitsRespawn is not.
    bool Benched(uint8_t e) const { return benched_.count(e) != 0; }
    // Tournament, Pit Bridge Duels: the two fighting now (0: none), what the bracket and the queue hold, and the rounds the pairing
    // has given each (Tournament). SetBracketSeed: the seed of the bracket or queue order, set before Start (a rematch draws another).
    uint8_t PairA() const { return pair_[0]; }
    uint8_t PairB() const { return pair_[1]; }
    int PairingWins(uint8_t e) const { auto it = pairWins_.find(e); return pairingOver_ || it == pairWins_.end() ? 0 : it->second; }   // 0 once the fight is decided (the bracket shows the next one)
    const Bracket& bracket() const { return bracket_; }
    const KingQueue& queue() const { return queue_; }
    void SetBracketSeed(uint32_t seed) { bracketSeed_ = seed; }
    void OnDeath(uint8_t victim, uint8_t killer, uint32_t nowMs);   // killer 0 = no attributable attacker
    // Capture the flag: `team` took a flag home. One point for the team; at the limit the match is over, and in sudden death the
    // team ahead wins at once. False, and no effect, outside CTF, outside a fought phase or for a team the match has not.
    bool AddCapture(int8_t team, uint32_t nowMs);
    // Duel: the fighter gives up the round being fought (held G); it goes as if they had fallen, with no kill credited.
    // False, and no effect, outside Duel, outside a fought round (countdown, round gap, results) or for a downed fighter.
    bool Forfeit(uint8_t entity, uint32_t nowMs);
    // Players vs bots (Progression co-op): a living human gives up the fight (held G), as if they had fallen. With every
    // human down that way the bots win and the match ends. False, and no effect, in a PvP format, outside a fought phase,
    // for a bot or for a fighter already dead.
    bool GiveUpFight(uint8_t entity, uint32_t nowMs);
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
    uint8_t roundWinner() const { return roundWinner_; }   // Duel's gap: who took the round, 0 = drawn / not in the gap; a team round: its highest-numbered survivor
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
    struct F { int8_t team = kNoTeam; bool human = true, alive = true; uint32_t diedAt = 0; int kills = 0, deaths = 0; uint64_t id = 0; };
    void AddPoints(int key, int points);
    void RefreshInfectionScores();                      // Infection: score_ = the survivors alive and the infected
    void CheckInfectionEnd(uint32_t nowMs);             // Infection: no survivor left alive ends it
    void ResolveDuelRound(uint32_t nowMs);              // Duel: the round goes to the last one standing (team rounds: the last team)
    // Tournament, Pit Bridge Duels
    void StartQueue();                                  // the bracket / queue over the humans now in, the first pair seated
    void SeatPair(Events* ev);                          // the pair to fight is the bracket's current game / the queue's front two; the rest are benched
    void ResolveQueueRound(uint8_t winner, uint32_t nowMs);   // a round ended: 0 = drawn
    void FinishPairing(uint8_t winner, uint32_t nowMs); // Tournament: the winner takes the fight
    void QueueLeft(uint8_t entity, uint32_t nowMs);     // a fighter left: out of the bracket (a forfeit in the fight) / the queue
    void EndQueueMatch(uint32_t nowMs);                 // nobody left to fight (the bracket decided, the queue down to one)
    bool PairMissing() const;
    void End(int winnerKey, uint32_t nowMs);
    Events Advance(uint32_t nowMs);                     // Tick without the regear decision
    int UniqueLeader() const;                           // kNoWinner when tied or empty
    MatchConfig cfg_;
    MatchPhase phase_ = MatchPhase::Idle;
    uint32_t phaseStart_ = 0;
    uint32_t resultsMs_ = kResultsMs;
    std::map<uint8_t, F> fighters_;
    std::map<int, int> score_;
    std::map<uint64_t, PlayerScore> bank_;              // by id: scores of players not in the match now (left, or not back since a migration)
    std::set<uint8_t> gone_;                            // K1: left mid-match, down until Return
    std::set<uint8_t> benched_;                         // Duel: a third player who joined a fought match: down and out of every round until it ends
    int winner_ = kNoWinner;
    bool endedPending_ = false;
    // bot formats
    void StartWave(uint32_t nowMs, Events& ev);
    int AliveBots() const;
    int wave_ = 0, remaining_ = 0, waveRank_ = 0, spawnOwed_ = 0;   // owed: requested, not yet added
    int players_ = 0;
    bool inBreak_ = false, clearedPending_ = false;
    uint32_t breakStart_ = 0;
    std::set<uint8_t> skipVotes_;
    uint32_t BreakMs() const { return cfg_.waveBreakS ? uint32_t(cfg_.waveBreakS) * 1000u : cfg_.format == Format::Zombies ? kZombieBreakMs : kWaveBreakMs; }
    std::vector<uint8_t> revive_;
    void ResetModeState();
    // duel
    bool roundOver_ = false;
    uint32_t roundEnd_ = 0;
    int round_ = 0;
    uint8_t roundWinner_ = 0;
    // queue formats
    Bracket bracket_;
    KingQueue queue_;
    uint32_t bracketSeed_ = 1;
    uint8_t pair_[2] = {0, 0};
    std::map<uint8_t, int> pairWins_;                   // Tournament: rounds each fighter of the pairing has won
    bool pairingOver_ = false;                          // Tournament: the fight is decided; the next round reset seats the next pairing
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

// Generated loadouts made ahead, for one (tier, weapon filter) at a time: another tier or filter empties it.
// Before the match starts, while its fighters come in: how many loadouts its start will generate for `humans` players
// (the start regears like a rematch, RegearFor), so the host can make them ahead in that wait.
size_t StartLoadoutsAhead(const MatchConfig& c, size_t humans);

class GearPool {
public:
    bool Wants(uint8_t tier, WeaponFilter weapons, size_t ahead);                 // true while it holds fewer than `ahead` for `tier` under `weapons`
    void Put(uint8_t tier, WeaponFilter weapons, std::vector<uint8_t> blob);      // an empty blob (a failed generation) is not kept
    std::vector<uint8_t> Take(uint8_t tier, WeaponFilter weapons);                // the oldest one made for them; empty when none
    size_t size() const { return blobs_.size(); }
private:
    void Use(uint8_t tier, WeaponFilter weapons);
    uint8_t tier_ = 0xFF;
    WeaponFilter weapons_ = WeaponFilter::Any;
    std::vector<std::vector<uint8_t>> blobs_;
};

// What clears the dropped weapons and loose armour: Duel each new round, free-for-all and teams each rematch, the bot
// formats (fought over the fallen bots' loot) a minute after each piece was dropped (session/hsm_lease.h).
enum class DropClear : uint8_t { EachRound = 0, EachMatch = 1, AfterIdle = 2 };
inline DropClear DropClearFor(Format f) {
    return IsRoundFormat(f) ? DropClear::EachRound : IsBotFormat(f) ? DropClear::AfterIdle : DropClear::EachMatch;
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
// Zombies: three per player plus two per wave (max 40), tier + wave/3. Ranks cap at 7.
struct Wave { int count = 0; int rank = 0; };
Wave WaveFor(Format f, int waveIndex, int humans, int tier);

// Team spawn sides: one contiguous angular arc of points per team around the points' centroid.
std::vector<int8_t> PartitionSides(const std::vector<Point3>& points, int teams);
// Index of the point (on `team`'s side when it has any) farthest from the nearest living enemy.
size_t PickRespawn(const std::vector<Point3>& points, const std::vector<int8_t>& sides, int team, const std::vector<Point3>& enemies);

}  // namespace hsm

#include "match/hsm_match_impl.h"
