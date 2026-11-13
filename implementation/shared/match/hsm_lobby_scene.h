// Lobby page (MATCH_ROUNDS_UI_CHAT_PLAN 3.4): what the multiplayer page shows, as data. A header with the state pill
// and the version badge; left, the lobby browser (not in a lobby) or the players (in one); middle, the mode cards and
// the setting rows with their steppers and one-line hints for the current format; right, the map cards and START
// MATCH. Members see everything the host chose, disabled, except their own team, and mark themselves ready (the
// host starts the match instead). The page (ui/menu.cpp) fills a LobbyLocal from the Steam lobby and maps the scene
// onto widgets; the steppers' arithmetic is here too.
#pragma once
#include "match/hsm_crypts.h"
#include "match/hsm_hud_scene.h"
#include "match/hsm_kept_kit.h"
#include "match/hsm_map_list.h"
#include "match/hsm_win_by.h"
#include "session/hsm_build.h"
#include "session/hsm_player_limit.h"
#include "session/hsm_region.h"
#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

namespace hsm {

enum class LobbyPhase : uint8_t { Offline, Idle, Creating, Joining, InLobby };   // game::Lobby::State

struct LobbyPlayer {
    std::string name;
    bool host = false, ready = false, me = false;
    int team = kNoTeam;                 // their own pick (Auto: kNoTeam)
    int hostOverride = kNoTeam;         // where the host moved them
    std::string build;                  // their mod build (session/hsm_build.h), empty until their member data comes
    std::string steamName;              // their Steam name when it is not the name shown (a nickname), for the host's row; empty: nothing to add
};

// The public-lobby list's filters (the page's chips, in this order): mode, room, compatibility, sorting, passwords, region.
// All off: every lobby Steam returned, in its order.
struct BrowserFilter {
    uint8_t mode = 0;                   // 0: any; else the kBrowserModes entry shown
    bool hasRoom = false, compatibleOnly = false, mostPlayers = false;
    bool noPassword = false;            // hide the locked lobbies
    uint8_t region = 0;                 // 0: my region (what Steam finds nearby), 1: any region (asks Steam worldwide), 2+: one region (RegionFilterAt)
    Region mine = Region::Auto;         // this player's own region, set by the page; Auto: not known, so "my region" hides nothing
};

// What a locked lobby's name starts with in the browser: plain Latin-1/ASCII, the game's font has no padlock glyph.
// The one place to change it.
inline constexpr const char* kLockedMarker = "[locked] ";

struct LobbyFound {                     // a public lobby from Find
    std::string name, map, mode;
    int players = 0;
    int maxPlayers = 8;                 // the host's player limit (lobby data hsm_max; 8 when the lobby has none)
    bool locked = false;                // it asks for a password (lobby data hsm_lock = "1"); the password itself is never listed
    bool compatible = false;
    std::string reason;                 // why not, when !compatible
    std::string pack;                   // "pack ArenaPack" (session/hsm_modpack.h PackRowText), with a mark when this machine lacks it; empty: no pack
    std::string mods;                   // the lobby's mod rule when not the default, "mods: Maps only" (session/hsm_mod_rules.h LobbyRuleTag)
    bool switchable = false;            // !compatible only because of mods this machine can turn off for a restart (session/hsm_mod_switch.h)
    std::string switchTag;              // then its note, "Restart: 2 mods off" (SwitchTag)
    Region region = Region::Auto;       // the host's region (lobby data hsm_region); Auto: it did not say
};

struct LobbyModMap { std::string path, display, pak; };   // a custom map from a mod pak (session/hsm_modmaps.h); pak empty when unknown

struct LobbyLocal {
    LobbyPhase phase = LobbyPhase::Offline;
    bool host = false;                  // we own the lobby
    bool publicLobby = false;           // it is listed publicly (else friends only)
    bool locked = false;                // the lobby asks for a password (Lobby::Locked)
    std::string myName, status, version;
    std::string myBuild;                // this mod's build (ModBuildString)
    bool allowBuildMismatch = false;    // allow_build_mismatch=1: the host may start with other builds in
    MatchConfig config;
    DamageRange damage;                 // the game's own damage slider here (game::VanillaDamageRange)
    std::string mapPick;                // the host's pick (package path), empty until picked
    bool progression = false;           // the host chose the Progression card (LobbyData hsm_prog): the stock hub, no match
    std::vector<LobbyModMap> modMaps;   // the custom maps found on this machine (modded maps, after the stock ones)
    bool modPickPending = false;        // the pick is a modded map whose pak is still being hashed: START waits
    std::vector<LobbyPlayer> players;
    int playerCount = -1;               // the host's player count for the mode cards (game::Lobby::PlayerCount: a dedicated server's own seat is no player); -1: the roster's size
    std::vector<LobbyFound> found;
    BrowserFilter filter;
    int maxPlayers = 8;                 // in a lobby: the host's player limit (Lobby::MaxPlayers)
    std::string myClass;                // the class we fight as, set only where classes count (Lobby::InClassesLobby)
    Region regionPick = Region::Auto;   // the picker on the page (region= in HSMMod.ini); Auto: guess it
    Region regionGuess = Region::Auto;  // what the guess found; Auto: nothing reliable
    bool classBudget = false;           // the lobby uses classes with a budget (Classes, not Unrestricted): the Class power row shows
};

// Each section shows its rows in this order. Sync last: ui/menu.cpp makes one stepper per row up to it.
// Players is the lobby's own player limit, not a MatchConfig field: the scene offers it as LobbyScene::playerLimit (it is
// in no SettingsFor list) and the page applies it with Lobby::SetMaxPlayers. First, so it leads the MATCH section; the
// Crypts card's Battle (the format fought after the gather) and Gather time (#3199) follow it.
enum class LobbyRow : uint8_t { Players, Battle, Gather, Limit, Time, Respawn, FreeCam, Gear, Tier, Weapons, Teams, FriendlyFire, Damage, DeadWeight, WeaponWeight, WaveBreak, ClearGround, MyTeam, TeamKit, Crossbows, Fuse, Shuffle, TaggerSpeed, WinBy, ClassPower, Strip, KeepKit, Bodies, Props, TimeOfDay, Scenario, Sync };   // Sync last: the menu builds rows in this order

// A chip a row shows in place of its arrows (the Weapons row's tickable types): a click on chip `i` toggles it. The row's value
// still reads out the whole choice.
struct RowChip { std::string label; bool on = false; };
struct SettingRow { LobbyRow id = LobbyRow::Limit; std::string label, value, hint; bool enabled = false; std::vector<RowChip> chips; };
// The mode cards' groups on the MODE tab (Party joins them with the party modes).
enum class ModeGroup : uint8_t { Versus, Coop };
struct ModeCard { Format format = Format::FreeForAll; std::string name, description; bool selected = false, enabled = false; bool progression = false; bool brawl = false;
                  bool crypts = false; ModeGroup group = ModeGroup::Versus; std::string hint; };   // progression: the extra last card; brawl: free-for-all with gear = Fists; crypts: gear = Crypts, its Battle row picks the format; hint: why a card is disabled for the player count (ModeAvailable)
struct MapCard { size_t index = 0; std::string name, tag, reason; bool allowed = false, selected = false, enabled = false;
                 bool modded = false; std::string path; };   // modded: index is MapCount() + the custom map's place, path its package path
struct BrowserRow {
    std::string name, players, detail;
    Span note;
    bool joinable = false;
    bool restart = false;               // joinable, but only after a restart with some mods off: the note says so instead of JOIN
    bool locked = false;
    size_t source = 0;                  // its index in LobbyLocal::found
};
struct RosterRow {
    std::string name, team;             // team: the formats with teams only
    std::string steam;                  // the host sees a player's Steam name beside a nickname; empty: nothing to add
    Rgba swatch;                        // a == 0: none
    bool host = false, ready = false, moved = false, me = false, movable = false;
};

inline constexpr size_t kBrowserRows = 6;

// The RULES tab's sections, in page order; each row sits in one.
enum class LobbySection : uint8_t { Match, Teams, Gear, World, Count };
inline LobbySection SectionOf(LobbyRow r) {
    switch (r) {
        case LobbyRow::Teams: case LobbyRow::FriendlyFire: case LobbyRow::MyTeam: case LobbyRow::TeamKit: case LobbyRow::Shuffle: return LobbySection::Teams;
        case LobbyRow::Gear: case LobbyRow::Tier: case LobbyRow::Weapons: case LobbyRow::Crossbows: case LobbyRow::Strip: case LobbyRow::KeepKit: case LobbyRow::Bodies: return LobbySection::Gear;
        case LobbyRow::WaveBreak: case LobbyRow::ClearGround: case LobbyRow::Props: case LobbyRow::TimeOfDay: case LobbyRow::Scenario: case LobbyRow::Sync: return LobbySection::World;
        default: return LobbySection::Match;
    }
}
inline const char* SectionName(LobbySection s) {
    switch (s) {
        case LobbySection::Teams: return "TEAMS";
        case LobbySection::Gear: return "GEAR";
        case LobbySection::World: return "WORLD";
        default: return "MATCH";
    }
}

// The browser's mode chip steps through these (0: any).
inline const std::vector<std::string>& BrowserModes() {
    static const std::vector<std::string> m = {"Any mode", "Free-for-all", "Duel", "Last man standing", "Teams", "Team duel", "Riot", "Juggernaut", "Tag", "Infection", "Crossbow TDM", "Tournament", "Pit bridge duels", "Chaos", "One crossbow", "Roulette", "Hide and seek", "Brawl", "Crypts", "Survival", "Zombies"};
    return m;
}
// The region chip steps through my region, any region and the six regions (kRegionCount - 1 of them, Auto left out).
inline constexpr uint8_t kRegionFilterStates = uint8_t(2 + kRegionCount - 1);
inline const char* RegionFilterLabel(uint8_t state) {
    if (state == 0) return "My region";
    if (state == 1) return "Any region";
    return state < kRegionFilterStates ? RegionLabel(Region(state - 1)) : "My region";
}
// The region a chip state shows (Auto: none, for my region and any region).
inline Region RegionFilterAt(uint8_t state) { return state >= 2 && state < kRegionFilterStates ? Region(state - 1) : Region::Auto; }
// How far Steam is asked to look: only my region stays with what Steam finds nearby.
inline LobbyDistance DistanceFor(const BrowserFilter& f) { return f.region == 0 ? LobbyDistance::Default : LobbyDistance::Worldwide; }

inline constexpr size_t kFilterChips = 6;   // mode, has room, compatible only, most players first, no password, region
inline constexpr size_t kRegionChip = 5;    // the last one: the page puts it by REFRESH
struct FilterChip { std::string label; bool on = false; };
inline std::vector<FilterChip> FilterChips(const BrowserFilter& f) {
    const auto& modes = BrowserModes();
    return {{modes[f.mode < modes.size() ? f.mode : 0], f.mode != 0}, {"Has room", f.hasRoom},
            {"Compatible only", f.compatibleOnly}, {"Most players first", f.mostPlayers},
            {"No password", f.noPassword}, {RegionFilterLabel(f.region), f.region != 0}};
}
// A chip clicked: the mode steps on (wrapping to any), the others turn on or off.
inline BrowserFilter ToggleFilter(BrowserFilter f, size_t chip) {
    switch (chip) {
        case 0: f.mode = uint8_t((f.mode + 1) % BrowserModes().size()); break;
        case 1: f.hasRoom = !f.hasRoom; break;
        case 2: f.compatibleOnly = !f.compatibleOnly; break;
        case 3: f.mostPlayers = !f.mostPlayers; break;
        case 4: f.noPassword = !f.noPassword; break;
        case 5: f.region = uint8_t((f.region + 1) % kRegionFilterStates); break;
        default: break;
    }
    return f;
}
// A lobby's mode string against a browser entry: equal, or the entry plus the detail the host adds ("Teams (3)" is "Teams").
inline bool ModeMatches(const std::string& lobbyMode, const std::string& entry) {
    return lobbyMode == entry || (lobbyMode.size() > entry.size() && lobbyMode.compare(0, entry.size(), entry) == 0 && lobbyMode.compare(entry.size(), 2, " (") == 0);
}
inline bool PassesFilter(const LobbyFound& l, const BrowserFilter& f) {
    const auto& modes = BrowserModes();
    if (f.mode != 0 && f.mode < modes.size() && !ModeMatches(l.mode, modes[f.mode])) return false;
    if (f.hasRoom && LobbyIsFull(l.players, l.maxPlayers)) return false;   // each lobby's own limit
    if (f.compatibleOnly && !l.compatible && !l.switchable) return false;   // a restart away still counts
    if (f.noPassword && l.locked) return false;
    if (f.region == 0) return !RegionKnown(f.mine) || !RegionKnown(l.region) || l.region == f.mine;   // a lobby that did not say, or no region of mine: nothing to hide
    if (f.region >= 2) return l.region == RegionFilterAt(f.region);                                  // a region asked for: only lobbies that say they are in it
    return true;
}
// The format cards: every format but ThreeTeams (the old value of the Teams card's 3 teams) and CaptureTheFlag (removed);
// then Brawl, Crypts and Progression.
inline constexpr size_t kModeOrderCount = kFormatCount - 2;
inline constexpr size_t kModeCardCount = kModeOrderCount + 3;

struct LobbyScene {
    Span pill;
    std::string me, badge, status;      // status: not in a lobby only (in one, the pill says it)
    std::string regionButton;           // the region picker at the top right: "Auto (Europe)", "Auto (unknown)", "NA East"
    bool idle = false;                  // Host, Find, the browser and the idle hint
    std::vector<BrowserRow> browser;
    std::vector<FilterChip> filters;    // kFilterChips, while idle
    std::string browserEmpty;           // why the list is empty (nothing found, or the filters hide it all)
    std::string browserCount;           // "3 of 7 lobbies shown" (filters on), "7 lobbies", "" (none found)
    bool in = false;                    // the players, Invite and Leave, the modes, the settings, the maps
    std::string playersTitle;
    std::vector<RosterRow> roster;
    bool ready = false;                 // the Ready toggle: members (the host starts the match instead)
    bool readyOn = false;               // we are marked ready: the toggle is framed, our row shows the dot
    std::string readyLabel;
    bool hostPassword = false;          // the host's password box (SET and CLEAR), in a lobby
    std::string passwordNote;           // beside it: locked or open, and that changing it kicks nobody
    bool balance = false;               // Auto-balance: the host, in the formats with teams
    bool shuffle = false;               // SHUFFLE NOW: the host, in the formats with teams (deals everyone at random into the lobby's moves)
    std::string classLine;              // in a lobby that uses classes: which one we fight as, beside CHANGE; empty: no row
    std::vector<ModeCard> modes;        // the grid's reading order, two to a row
    std::vector<SettingRow> settings;
    SettingRow playerLimit;             // the Players stepper (LobbyRow::Players): the host steps it, members read it; in a lobby only
    bool hasPlayerLimit = false;
    SettingRow scenario;                // the Scenario stepper (LobbyRow::Scenario): only when the picked map offers more than Standard; kept apart from `settings` like the player limit
    bool hasScenario = false;
    SettingRow timeOfDay;               // the Time of day stepper (LobbyRow::TimeOfDay): only on an arena with a day cycle; apart from `settings` like the Scenario
    bool hasTimeOfDay = false;
    SettingRow winBy;                   // the Win by stepper (LobbyRow::WinBy): the round formats but Tag, per player sync; kept apart from `settings` like the scenario
    bool hasWinBy = false;
    SettingRow props;                   // the Props stepper (LobbyRow::Props): every lobby fight but Progression; kept apart from `settings` like the scenario
    bool hasProps = false;
    SettingRow classPower;              // the Class power stepper (LobbyRow::ClassPower): lobbies with a class budget; kept apart from `settings` like the scenario
    bool hasClassPower = false;
    SettingRow strip;                   // the Stripping stepper (LobbyRow::Strip): every lobby fight but Progression; kept apart from `settings` like the scenario
    bool hasStrip = false;
    SettingRow keepKit;                 // the Loadouts stepper (LobbyRow::KeepKit): a Tournament with random loadouts; kept apart from `settings` like the scenario
    bool hasKeepKit = false;
    std::string hostOnly;               // members: "Only the host can change these."
    std::vector<MapCard> maps;          // the pickable maps, kMaps order; none for Progression
    bool start = false;                 // START MATCH: the host, while every known build matches (or the override is on)
    std::string waiting;                // members: "Waiting for the host"; anyone: the build warning, which replaces it
    std::string summary;                // beside START: the mode, the map and the gear, one line
    bool buildMismatch = false;         // another player is on a different build (waiting says who)
    std::string scenarioMap;            // the picked map's package path for the Scenario row's stepper (StepScenario); empty: a modded map
};

// ------------------------------------------------------------------ steppers

// The option before `cur`, wrapping; an unknown value steps to the last (NextOf steps it to the first).
inline int PrevOf(const std::vector<int>& options, int cur) {
    if (options.empty()) return cur;
    auto it = std::find(options.begin(), options.end(), cur);
    return it == options.end() || it == options.begin() ? options.back() : *(it - 1);
}
inline int StepOf(const std::vector<int>& options, int cur, int dir) { return dir < 0 ? PrevOf(options, cur) : NextOf(options, cur); }

inline std::vector<int> LimitOptions(Format f) {
    if (f == Format::Tournament) return {1};                          // one round a fight (no Limit row)
    if (f == Format::PitBridgeDuel) return {3, 5, 7, 10, 15};         // fights won to take the match
    if (IsRoundFormat(f)) return {1, 2, 3, 5, 7, 10};                 // rounds to win
    return {3, 5, 10, 15, 20, 30, 50};
}
inline const std::vector<int> kTimeOptions = {0, 300, 600, 900, 1200};
inline const std::vector<int> kRespawnOptions = {3, 5, 10, 15, 20};
inline const std::vector<int> kInfectionTimeOptions = {300, 600, 900, 1200};   // Infection always has a clock: the survivors win by lasting
inline const std::vector<int> kFuseOptions = {10, 15, 20, 30, 45, 60};         // Tag's fuse, seconds
inline const std::vector<int> kHideTimeOptions = {15, 20, 30, 45, 60};         // Hide and Seek: the Hide time, seconds (the Fuse row there)
inline const std::vector<int> kSeekTimeOptions = {120, 180, 240, 300, 420, 600};   // Hide and Seek: a round's Seek time, seconds (the Time row there)
// The Crypts card's Battle row (#3199): the formats a gather ends in, in the row's order (Teams takes 2 to 4 on its own row).
inline constexpr Format kCryptsBattles[] = {Format::LastManStanding, Format::FreeForAll, Format::TwoTeams, Format::TeamDuel, Format::Tournament, Format::Duel, Format::PlayersVsBots};
inline constexpr size_t kCryptsBattleCount = sizeof(kCryptsBattles) / sizeof(kCryptsBattles[0]);
inline constexpr Format kCryptsDefaultBattle = Format::LastManStanding;
inline const std::vector<int> kTaggerSpeedStops(std::begin(kTaggerSpeedOptions), std::end(kTaggerSpeedOptions));   // Tag's tagger speed, percent
// The stops of the Shuffle teams row: Each round only where the format has rounds to deal for (Sanitize).
inline std::vector<int> ShuffleOptions(Format f) { return IsTeamRound(f) ? std::vector<int>{0, 1, 2} : std::vector<int>{0, 1}; }
// The wave break in seconds the match runs (0 = the format's own: Survival 60, Zombies 10).
inline int WaveBreakShown(const MatchConfig& c) { return c.waveBreakS ? c.waveBreakS : c.format == Format::Zombies ? int(Match::kZombieBreakMs / 1000) : int(Match::kWaveBreakMs / 1000); }

// The format and team pick go their own ways (SelectFormat, StepTeamPick); every other row steps here, by +1 or -1.
// A value out of range steps from where Sanitize would put it. damage: the game's own slider (the Damage row's range).
// The Dead weight or Weapon weight row's field (the menu's sliders); nullptr for another row.
inline uint8_t* WeightField(MatchConfig& c, LobbyRow r) {
    return r == LobbyRow::DeadWeight ? &c.deadWeightPct : r == LobbyRow::WeaponWeight ? &c.weaponWeightPct : nullptr;
}
inline const uint8_t* WeightField(const MatchConfig& c, LobbyRow r) { return WeightField(const_cast<MatchConfig&>(c), r); }
inline MatchConfig SelectFormat(MatchConfig c, Format f);
inline MatchConfig StepSetting(MatchConfig c, LobbyRow s, int dir, const DamageRange& damage = DamageRange{}) {
    if (IsHideSeek(c.format) && s == LobbyRow::Time) { c.timeLimitS = uint16_t(StepOf(kSeekTimeOptions, c.timeLimitS, dir)); return c; }   // a round's Seek time
    if (IsHideSeek(c.format) && s == LobbyRow::Fuse) { c.fuseS = uint8_t(StepOf(kHideTimeOptions, c.fuseS, dir)); return c; }             // the Hide time
    bool back = dir < 0;
    switch (s) {
        case LobbyRow::Battle: {                   // the Crypts card's format: the gear stays Crypts
            size_t at = 0;                         // ThreeTeams (the old 3 teams) or a stale format steps from Last man standing
            for (size_t i = 0; i < kCryptsBattleCount; ++i) if (kCryptsBattles[i] == c.format) at = i;
            c = SelectFormat(c, kCryptsBattles[(at + (back ? kCryptsBattleCount - 1 : 1)) % kCryptsBattleCount]);
            break;
        }
        case LobbyRow::Gather: c.gatherS = StepGather(c.gatherS, dir); break;
        case LobbyRow::Limit: c.scoreLimit = uint16_t(StepOf(LimitOptions(c.format), c.scoreLimit, dir)); break;
        case LobbyRow::Time: c.timeLimitS = uint16_t(StepOf(IsInfection(c.format) ? kInfectionTimeOptions : kTimeOptions, c.timeLimitS, dir)); break;
        case LobbyRow::Fuse: c.fuseS = uint8_t(StepOf(kFuseOptions, c.fuseS, dir)); break;
        case LobbyRow::TaggerSpeed: c.taggerSpeedPct = uint8_t(StepOf(kTaggerSpeedStops, c.taggerSpeedPct, dir)); break;
        case LobbyRow::Shuffle: c.shuffle = TeamShuffle(StepOf(ShuffleOptions(c.format), int(c.shuffle), dir)); break;
        case LobbyRow::WinBy: c.winBy = uint8_t(StepWinBy(c.winBy, dir)); break;
        case LobbyRow::Props: c.propsOff = !c.propsOff; break;
        case LobbyRow::Strip: c.stripOff = !c.stripOff; break;
        case LobbyRow::Bodies: c.ownBodies = !c.ownBodies; break;
        case LobbyRow::KeepKit: c.keepKit = !c.keepKit; break;
        case LobbyRow::Respawn: c.respawnDelayS = uint16_t(StepOf(kRespawnOptions, c.respawnDelayS, dir)); break;
        case LobbyRow::Gear: {
            size_t at = 0;                           // Fists (the Brawl card) is no stop of the row: it steps as Own does
            for (size_t i = 0; i < kGearRowCount; ++i) if (kGearRow[i] == c.gear) at = i;
            c.gear = kGearRow[(at + (back ? kGearRowCount - 1 : 1)) % kGearRowCount];
            break;
        }
        case LobbyRow::Tier: c.tier = uint8_t((std::min<uint8_t>(c.tier, 7) + (back ? 7 : 1)) % 8); break;
        case LobbyRow::Weapons: break;                           // the host ticks the types: ToggleWeaponType(SettingRow::chips)
        case LobbyRow::Crossbows:
            c.crossbows = uint8_t((ValidCrossbows(c.crossbows) ? c.crossbows : 0) + (back ? kCrossbowModeCount - 1 : 1)) % kCrossbowModeCount;
            break;
        case LobbyRow::Teams: c.teams = uint8_t(StepOf({2, 3, 4}, c.teams, dir)); break;
        case LobbyRow::FriendlyFire: c.friendlyFire = !c.friendlyFire; break;
        case LobbyRow::FreeCam: c.freeCam = !c.freeCam; break;
        case LobbyRow::Damage: c.damagePct = StepDamage(c.damagePct, dir, damage); break;
        case LobbyRow::DeadWeight: c.deadWeightPct = StepWeight(c.deadWeightPct, dir); break;
        case LobbyRow::WeaponWeight: c.weaponWeightPct = StepWeight(c.weaponWeightPct, dir); break;
        case LobbyRow::WaveBreak: {
            std::vector<int> o(std::begin(kWaveBreakOptions), std::end(kWaveBreakOptions));
            c.waveBreakS = uint16_t(StepOf(o, WaveBreakShown(c), dir)); break;
        }
        case LobbyRow::TimeOfDay: c.timeOfDay = uint8_t(StepOf({0, 1, 2, 3, 4}, c.timeOfDay, dir)); break;
        case LobbyRow::ClearGround: {
            std::vector<int> o(std::begin(kClearGroundOptions), std::end(kClearGroundOptions));
            c.clearGroundMin = uint8_t(StepOf(o, c.clearGroundMin, dir)); break;
        }
        case LobbyRow::TeamKit: {      // the row reads Tabard / Full kit / Off
            static constexpr hsm::TeamKit kOrder[3] = {hsm::TeamKit::Tabard, hsm::TeamKit::Full, hsm::TeamKit::Off};
            int at = 1;                                           // a value out of range steps from Full
            for (int i = 0; i < 3; ++i) if (kOrder[i] == c.teamKit) at = i;
            c.teamKit = kOrder[(at + (back ? 2 : 1)) % 3];
            break;
        }
        case LobbyRow::Players: case LobbyRow::MyTeam: break;   // the player limit is the lobby's (StepPlayerLimit); the team pick goes its own way
        case LobbyRow::ClassPower: c.classPower = StepClassPower(c.classPower, dir); break;
        case LobbyRow::Scenario: break;                          // which scenarios there are depends on the map: StepScenario(LobbyScene::scenarioMap, ...)
        case LobbyRow::Sync: c.hostPhysics = !c.hostPhysics; break;
    }
    return c;
}

// A new format starts at its own default limit (the other settings stay); the same format changes nothing. Tag is
// bare-fisted (gear Fists at kBrawlTier) and starts at kTagDamagePct, which the Damage row can still change.
inline MatchConfig LeaveTag(MatchConfig c) {           // gear and tier back at their defaults; Tag's light hits stay in Tag. One Crossbow, bare as well, is left the same way
    if (IsHideSeek(c.format)) { c.timeLimitS = MatchConfig{}.timeLimitS; c.fuseS = MatchConfig{}.fuseS; return c; }   // Hide and Seek: its Seek and Hide time are not the next mode's clock and fuse
    if (IsRoulette(c.format)) { c.gear = Gear::Own; c.tier = MatchConfig{}.tier; return c; }   // Roulette, bare as well: gear and tier back, nothing else to undo
    if (!IsTag(c.format) && !IsOneCrossbow(c.format)) return c;
    c.gear = Gear::Own;
    c.tier = MatchConfig{}.tier;
    if (IsOneCrossbow(c.format)) c.crossbows = 0;      // its one crossbow is the mode's own, not the next mode's Crossbows row
    else if (c.damagePct == kTagDamagePct) c.damagePct = kDamageStock;
    return c;
}
inline MatchConfig SelectFormat(MatchConfig c, Format f) {
    if (c.format == f) return c;
    c = LeaveTag(c);
    c.format = f;
    c.scoreLimit = DefaultScoreLimit(f);
    if (IsTag(f)) { c.gear = Gear::Fists; c.tier = kBrawlTier; c.damagePct = kTagDamagePct; }
    if (IsOneCrossbow(f)) { c.gear = Gear::Fists; c.tier = kOneCrossbowTier; c.crossbows = uint8_t(Crossbows::One); }   // unarmed peasants and the one crossbow
    if (IsRoulette(f)) { c.gear = Gear::Fists; c.tier = kOneCrossbowTier; c.crossbows = 0; }   // unarmed peasants; the mode hands out its own crossbow
    if (IsHideSeek(f)) c.timeLimitS = kSeekDefaultS;   // a round's Seek time, not the clock the last mode had
    return c;
}

// A mode card picked: the Brawl card is free-for-all with bare fists (gear Fists, kBrawlTier, free-for-all's default limit);
// the Crypts card is gear Crypts with Last man standing to start (its Battle row picks another); the format cards are
// SelectFormat, and leaving Brawl or Crypts puts the gear and tier back at their defaults (even for the card of the same
// format: other rules).
inline MatchConfig SelectMode(MatchConfig c, Format f, bool brawl, bool crypts = false) {
    if (crypts) {
        if (IsCrypts(c)) return c;
        c = LeaveTag(c);
        c.format = kCryptsDefaultBattle;
        c.scoreLimit = DefaultScoreLimit(c.format);
        c.gear = Gear::Crypts;
        c.tier = MatchConfig{}.tier;
        return c;
    }
    if (IsCrypts(c)) {                               // leaving Crypts: the same format's card keeps its settings
        c.gear = Gear::Own;
        if (c.format == f && !brawl) return c;
    }
    if (brawl) {
        if (IsBrawl(c)) return c;
        c = LeaveTag(c);
        c.format = Format::FreeForAll;
        c.scoreLimit = DefaultScoreLimit(c.format);
        c.gear = Gear::Fists;
        c.tier = kBrawlTier;
        return c;
    }
    if (IsBrawl(c)) {                                // leaving Brawl (leaving Tag is SelectFormat's)
        c.gear = Gear::Own;
        c.tier = MatchConfig{}.tier;
        if (c.format == f) { c.scoreLimit = DefaultScoreLimit(f); return c; }
    }
    return SelectFormat(c, f);
}

// Forward: Auto, team 0, ..., the last team, Auto (NextTeamPick). Back the other way. A pick the format has no team
// for counts as Auto, as ResolveTeams counts it.
inline int8_t StepTeamPick(int8_t cur, int teams, int dir) {
    if (dir >= 0) return NextTeamPick(cur, teams);
    if (teams <= 0) return kNoTeam;
    if (cur < 0 || cur >= teams) return int8_t(teams - 1);
    return cur == 0 ? kNoTeam : int8_t(cur - 1);
}

// ------------------------------------------------------------------ text

inline constexpr const char* kBrawlBlurb = "Free-for-all with bare fists: everyone spawns naked, no weapons. First to the limit wins.";
inline constexpr const char* kCryptsBlurb = "Everyone loots their own Crypts as a new random character of the tier for the Gather time, then fights in what they found. Die down there and you keep what you had.";

inline const char* FormatBlurb(Format f) {
    switch (f) {
        case Format::Duel: return "One on one, rounds to the last one standing. Everybody respawns together; new gear each round with random or identical gear.";
        case Format::LastManStanding: return "Rounds to the last one standing, for two or more players. Everybody respawns together; new gear each round with random or identical gear.";
        case Format::TwoTeams: case Format::ThreeTeams: case Format::CaptureTheFlag: return "Two to four teams (Red, Blue, Green, Gold). Every kill scores for the team; the first team to the limit wins.";
        case Format::TeamDuel: return "Teams in rounds: the last team standing takes the round. Two to four teams; everybody respawns together, new gear each round with random or identical gear.";
        case Format::Riot: return "One knight in the best kit against everyone else in lighter gear. The last side standing takes the round; the knight changes every match.";
        case Format::Juggernaut: return "Tall, heavy juggernauts against shorter, lighter fighters with more gear. The last side standing takes the round; the juggernauts change every match.";
        case Format::Tag: return "Bare fists and light hits. One fighter is it and carries a fuse: hit someone to pass it on, or burn out when it runs down. The last one standing takes the round. Two or more players.";
        case Format::Infection: return "Survivors against the infected. Patient zero starts infected; whoever an infected fighter kills is infected too and comes back. Survive the clock to win.";
        case Format::CrossbowTDM: return "Teams, with a crossbow for everyone as well as the usual kit. Every kill scores for the team; the first team to the limit wins. Experimental: needs the crossbow test.";
        case Format::Tournament: return "A knockout bracket, one on one: two fight, everyone else watches. One round a fight: the winner advances, the last one left takes the match. Three or more players.";
        case Format::PitBridgeDuel: return "On the Pit's bridge, the winner stays: the loser goes to the back of the line and the next player steps up. Most fights won takes the match.";
        case Format::OneCrossbow: return "Rounds to the last one standing between unarmed peasants. One of them starts each round with a crossbow that never runs out of bolts; it falls with its holder for the next one to pick up. Two or more players.";
        case Format::Roulette: return "Unarmed peasants pass one crossbow round: six chambers, one of them live. On your turn pull the trigger on yourself (X) for a point, or on the fighter you face (C) to hand it on. The live chamber puts you out, or loads a real bolt for the other. The last one standing scores too. Two or more players.";
        case Format::HideSeek: return "Rounds of seekers against hiders, dealt anew each round: one seeker, or two with six players or more. The hiders, unarmed peasants with no name tags, get the Hide time while the seekers stand still and see nothing. Then the seekers hunt with two-handed swords for the Seek time. Every hider down: the seekers take the round. Time up: the hiders still standing do. Three or more players, on Slums or a custom map.";
        case Format::Chaos: return "Rounds to the last one standing, and no two alike: every round each fighter gets a random rank of gear, weapon type, height and weight, and the round its own damage and weights. Two or more players.";
        case Format::PlayersVsBots: return "Everyone against waves of bots that rank up. The fallen return when a wave is cleared.";
        case Format::Zombies: return "Zombie waves in the Abyss, one zombie at a time per player. The fallen return when a wave is cleared.";
        case Format::FreeForAll: break;
    }
    return "Everyone for themselves. Every kill scores; the first to the limit wins.";
}

inline const char* MapTag(const MapInfo& m) {
    if (m.abyss) return "Large level \xC2\xB7 no spawn points: ring spawn";
    return m.bigArena ? "Large level" : "Arena";
}

inline const char* MapRefusal(Format f) {
    if (f == Format::Zombies) return "Zombies needs the Abyss";
    if (f == Format::Duel) return "Duel needs an arena";
    if (f == Format::Tournament) return "Tournament needs an arena";
    if (f == Format::PitBridgeDuel) return "Pit bridge duels need the Pit";
    return "Not for this mode";
}

inline const char* GearName(Gear g) {
    if (g == Gear::RandomTier) return "Random by tier";
    if (g == Gear::Mirror) return "Everyone identical";
    if (g == Gear::HostKit) return "Host's loadout";
    if (g == Gear::Crypts) return "Crypts loot";
    return "Own loadouts";
}

inline int TierBudget(int tier) { return kTierBudget[std::clamp(tier, 0, 7)]; }

namespace lobby_detail {
inline bool SamePath(std::string_view a, std::string_view pick) {
    const std::string_view b = map_detail::StripObjectName(pick);
    if (a.empty() || a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        char x = a[i], y = b[i];
        if (x >= 'A' && x <= 'Z') x = char(x + 32);
        if (y >= 'A' && y <= 'Z') y = char(y + 32);
        if (x != y) return false;
    }
    return true;
}

// The package path of the map a match of this lobby starts on; empty for a modded map (the scenarios offer Standard only).
inline std::string ScenarioMap(const LobbyLocal& l) {
    if (l.config.format != Format::Zombies)
        for (const LobbyModMap& m : l.modMaps) if (SamePath(m.path, l.mapPick)) return {};
    const size_t i = EffectiveMap(l.config.format, l.config.gear, l.mapPick);
    return i < MapCount() ? std::string(kMaps[i].path) : std::string();
}

inline bool ValidTeam(int t, Format f, int teams) { return PicksTeams(f) && t >= 0 && t < TeamCount(f, teams); }
inline int EffectiveTeam(const LobbyPlayer& p, Format f, int teams) {
    return ValidTeam(p.hostOverride, f, teams) ? p.hostOverride : ValidTeam(p.team, f, teams) ? p.team : kNoTeam;
}

inline SettingRow Row(const LobbyLocal& l, LobbyRow s) {
    const MatchConfig& c = l.config;
    Format f = c.format;
    bool duel = IsRoundFormat(f), bots = IsBotFormat(f);
    SettingRow r;
    r.id = s;
    r.enabled = l.host;
    switch (s) {
        case LobbyRow::Players:
            r.label = "Players";
            r.value = std::to_string(l.maxPlayers);
            r.hint = "How many players the lobby takes. It cannot go below the players already in; nobody is kicked.";
            break;
        case LobbyRow::Battle:
            r.label = "Battle";
            r.value = FormatName(f);
            r.hint = "What is fought once the gather is over, everyone in the kit they gathered. Every round and respawn keeps it; a rematch is a new gather.";
            break;
        case LobbyRow::Gather:
            r.label = "Gather time";
            r.value = std::to_string(c.gatherS / 60) + " min";
            r.hint = "How long everyone loots their own Crypts (click the time to type 2 to 60 min). Reach an exit to keep what you carry; die and you keep what you had. Then everyone waits for the battle.";
            break;
        case LobbyRow::Limit:
            r.label = f == Format::Tournament ? "Rounds per fight" : f == Format::PitBridgeDuel ? "Wins to take the match" : duel ? "Rounds to win" : "Score limit";
            r.value = std::to_string(LimitToKey(f, c.scoreLimit));
            r.hint = f == Format::Tournament ? "How many rounds a fight is (best of): the first to win " + std::to_string(c.scoreLimit) + " takes it and moves on."
                   : f == Format::PitBridgeDuel ? "The first to win this many fights takes the match. The winner of a fight stays on the bridge."
                   : duel ? "The first to win this many rounds takes the match."
                   : HasTeams(f) ? "The first team to this many kills wins." : "The first to this many kills wins.";
            if (IsRoulette(f)) { r.label = "Points to win"; r.hint = "The first to this many points takes the match: a point for every blank you pull on yourself, and one for the last one standing."; }
            break;
        case LobbyRow::Time:
            r.label = "Time limit";
            r.value = TimeLabel(c.timeLimitS);
            if (bots) r.hint = c.timeLimitS ? "Players win if they hold out to the end (one wave cleared)." : "No clock: hold out as long as you can.";
            else if (IsInfection(f)) r.hint = "The survivors win if any of them are still standing when it runs out.";
            else r.hint = c.timeLimitS ? "When it runs out the leader wins; a tie goes to sudden death." : "No clock: play to the limit.";
            if (IsHideSeek(f)) { r.label = "Seek time"; r.hint = "How long the seekers have each round. The hiders still standing when it runs out take the round."; }
            break;
        case LobbyRow::Respawn:
            r.label = duel ? "Round break" : "Respawn delay";
            r.value = std::to_string(c.respawnDelayS) + " s";
            r.hint = duel ? "The pause between the end of a round and the next." : IsInfection(f) ? "How long the infected wait to come back." : "How long the fallen wait to come back.";
            break;
        case LobbyRow::Gear:
            r.label = "Gear";
            r.value = GearName(c.gear);
            if (c.gear == Gear::RandomTier) r.hint = bots ? "A random loadout for each player at the start, kept for the whole run."
                                                    : duel ? "Fresh random gear for each fighter every round." : "Fresh random gear for each fighter every life.";
            else if (c.gear == Gear::Mirror) r.hint = bots ? "One random loadout for everyone, kept for the whole run."
                                                  : duel ? "One random loadout for everyone, new each round." : "One random loadout for everyone, until the rematch.";
            else if (c.gear == Gear::HostKit) r.hint = "Everyone fights in the host's own loadout.";
            else r.hint = "Everyone fights in their own loadout.";
            break;
        case LobbyRow::Tier:
            r.label = IsCrypts(c) ? "Character tier" : bots ? "Starting tier" : "Gear tier";
            r.value = TierName(c.tier);
            if (GearRolled(c.gear) || IsCrypts(c)) r.value += " \xC2\xB7 budget " + std::to_string(TierBudget(c.tier));
            // 0.8.0-pre20: the tier is every player's strength (muscle and grip, forced: ForcedWeight), with any gear.
            r.hint = f == Format::Zombies ? "Zombies start at this rank and rise one every 3 waves. "
                   : bots ? "Bots start at this rank and rise one every 2 waves. " : "";
            if (IsCrypts(c)) r.hint += "Everyone starts the Crypts as a new random character of this rank, made as the game makes its fighters: the rank's build (weight and fat) and a weapon and armour within its budget. ";
            r.hint += BodiesOwn(c) ? "The players' strength is their own (the Bodies row)." : !GearRolled(c.gear) ? (bots ? "Also every player's strength." : "Sets every player's strength (muscle and grip).")
                                          : (bots ? "Also every player's strength and the random gear's budget." : "Sets every player's strength; the random gear is rolled within this budget.");
            break;
        case LobbyRow::Weapons:
            r.label = "Weapons";
            r.value = WeaponMaskName(c.weapons);
            if (IsCrypts(c)) r.hint = c.weapons == 0 ? "The new character may start with any weapon. Tick the types it may start with."
                                                     : "The new character starts with a weapon of a ticked type (the game's own weapon filter). Untick them all for any.";
            else r.hint = c.weapons == 0 ? "The random gear may hold any weapon. Tick the types it may hold."
                   : "The random gear's weapons are only of the ticked types; each loadout holds one of them (the game's own weapon filter). Untick them all for any.";
            r.chips.push_back({"Any", c.weapons == 0});
            for (uint8_t t = 1; t <= kWeaponTypeCount; ++t) r.chips.push_back({WeaponTypeName(t), WeaponMaskHas(c.weapons, t)});
            break;
        case LobbyRow::Teams:
            r.label = "Teams";
            r.value = std::to_string(TeamCount(c.format, c.teams)) + " teams";
            r.hint = "Players without a pick go to the smallest team.";
            break;
        case LobbyRow::FriendlyFire:
            r.label = "Friendly fire";
            r.value = c.friendlyFire ? "On" : "Off";
            if (bots) r.hint = c.friendlyFire ? "Players can hurt each other." : "Players cannot hurt each other.";
            else r.hint = c.friendlyFire ? "Hits on teammates count." : "Teammates cannot hurt each other.";
            break;
        case LobbyRow::Damage:
            r.label = "Player damage";
            r.value = DamageLabel(c.damagePct, l.damage);
            r.hint = "How hard hits land on every player, the game's own slider for everyone. Click the number to type up to 1000%. Yours is back after the session.";
            break;
        case LobbyRow::DeadWeight:
            r.label = "Dead weight";
            r.value = WeightLabel(c.deadWeightPct);
            r.hint = "How heavy fallen bodies are, the game's own Dead Weight slider for everyone (the game's own is 50%). Yours is back after the session.";
            break;
        case LobbyRow::WeaponWeight:
            r.label = "Weapon weight";
            r.value = WeightLabel(c.weaponWeightPct);
            r.hint = "How heavy every weapon is, the game's own Weapon Weight slider for everyone (the game's own is 50%). Yours is back after the session.";
            break;
        case LobbyRow::WaveBreak: {
            const int s = WaveBreakShown(c);
            r.label = "Wave break";
            r.value = s == 120 ? "2 min" : std::to_string(s) + " s";
            r.hint = "The pause between waves. Hold G to vote to skip it.";
            break;
        }
        case LobbyRow::TimeOfDay:
            r.label = "Time of day";
            r.value = TimeOfDayName(c.timeOfDay);
            r.hint = c.timeOfDay == 0 ? "The arena's light is the host's own Free Mode time."
                                      : "The arena's light and sounds for everyone, as the game's own time of day.";
            break;
        case LobbyRow::ClearGround:
            r.label = "Clear ground";
            r.value = c.clearGroundMin ? "Every " + std::to_string(c.clearGroundMin) + " min" : "Off";
            r.hint = c.clearGroundMin ? "Dropped weapons and loose armour are cleared on this timer, to keep the lag down. Held and worn gear stays."
                                      : "Dropped weapons and loose armour stay until the next round.";
            break;
        case LobbyRow::MyTeam: {
            r.label = "My team";
            r.enabled = true;                                        // everyone picks their own
            const LobbyPlayer* me = nullptr;
            for (const LobbyPlayer& p : l.players) if (p.me) me = &p;
            r.value = TeamLabel(me && ValidTeam(me->team, f, c.teams) ? me->team : kNoTeam);
            r.hint = me && ValidTeam(me->hostOverride, f, c.teams) ? "The host moved you to " + TeamLabel(me->hostOverride) + "."
                   : c.shuffle != TeamShuffle::Off ? std::string("Your pick is ignored while Shuffle teams is on.")
                                                   : std::string("Auto puts you on the smallest team.");
            break;
        }
        case LobbyRow::TeamKit:
            r.label = "Team colours";
            r.value = c.teamKit == TeamKit::Off ? "Off" : c.teamKit == TeamKit::Tabard ? "Tabard" : "Full kit";
            r.hint = c.teamKit == TeamKit::Off ? "Everyone keeps their own colours."
                   : c.teamKit == TeamKit::Tabard ? "The tabard takes the team colour. Each player sees it on their own copy of every fighter."
                   : "Clothes, tabard, shield and grips take the team colour; blades stay steel. Each player sees it on their own copy of every fighter.";
            break;
        case LobbyRow::FreeCam:
            r.label = "Free camera";
            r.value = c.freeCam ? "On" : "Off";
            r.hint = c.freeCam ? "Dead players can press F to fly a camera round the map. It stays inside the level."
                               : "Dead players can only follow the living.";
            break;
        case LobbyRow::Crossbows:
            r.label = "Crossbows";
            r.value = CrossbowsName(c.crossbows);
            r.hint = "Experimental: needs the crossbow test. Random players spawn with a crossbow (a new draw each round).";
            break;
        case LobbyRow::Fuse:
            r.label = "Fuse time";
            r.value = std::to_string(c.fuseS) + " s";
            r.hint = "How long \"it\" has before the fuse runs out and it is out of the round. Hitting someone passes the fuse on; it keeps burning.";
            if (IsHideSeek(f)) { r.label = "Hide time"; r.hint = "How long the hiders have at the start of each round, while the seekers stand still and see nothing."; }
            break;
        case LobbyRow::Shuffle:
            r.label = "Shuffle teams";
            r.value = c.shuffle == TeamShuffle::EachRound ? "Each round" : c.shuffle == TeamShuffle::EachMatch ? "Each match" : "Off";
            r.hint = c.shuffle == TeamShuffle::Off ? "The teams are dealt once, when the match starts, from the picks and the host's moves. SHUFFLE NOW deals them at random at once."
                   : std::string(c.shuffle == TeamShuffle::EachRound ? "The teams are dealt again at random, balanced, at the start, at every rematch and every new round. "
                                                                     : "The teams are dealt again at random, balanced, at the start and at every rematch. ")
                     + "Picks are ignored. Moves made here before the first match stay for it; each later shuffle clears them.";
            break;
        case LobbyRow::TaggerSpeed:
            r.label = "Tagger speed";
            r.value = std::to_string(c.taggerSpeedPct) + "%";
            r.hint = c.taggerSpeedPct == 100 ? "\"It\" runs and walks at normal speed."
                   : "\"It\" runs and walks " + std::to_string(c.taggerSpeedPct) + "% as fast while carrying the fuse. Passing the fuse on takes the bonus off.";
            break;
        case LobbyRow::WinBy: {
            const WinBy w = EffectiveWinBy(c.format, c.hostPhysics, c.winBy);
            r.label = "Win by";
            r.value = WinByName(w);
            r.hint = WinByHint(w);
            r.enabled = l.host && WinByApplies(c);
            break;
        }
        case LobbyRow::Props:
            r.label = "Props";
            r.value = c.propsOff ? "Off" : "On";
            r.hint = c.propsOff ? "The map's loose props (barrels, crates, furniture) are removed when the match starts and while it runs, on every screen. Weapons, armour and the map's structures stay."
                                : "The map's barrels, crates and other loose props stay.";
            break;
        case LobbyRow::ClassPower: {
            const int p = SnapClassPower(c.classPower);
            r.label = "Class power";
            r.value = "+" + std::to_string(p);
            r.hint = p == kClassPowerDefault ? "How strong a class may be: +15 power, the usual limit. Raise it to let stronger classes fight."
                   : "Classes up to +" + std::to_string(p) + " power may fight here (the usual limit is +15). A player whose class is stronger is warned, then removed after a minute.";
            break;
        }
        case LobbyRow::Strip:
            r.label = "Stripping";
            r.value = c.stripOff ? "Off" : "On";
            r.hint = c.stripOff ? "A downed fighter's armour stays on: grabbing it and letting go takes nothing off. Weapons can still be taken."
                                : "Grab a downed fighter's armour and let go to pull it off, as in the game.";
            break;
        case LobbyRow::Bodies:
            r.label = "Bodies";
            r.value = c.ownBodies ? "Own" : "Same for all";
            r.hint = IsCrypts(c) ? (c.ownBodies ? "Every player keeps the build of the character they gathered with (rolled for the tier: its Weight)."
                                                : "Every player is built for the Character tier: the same muscle and grip.")
                   : c.ownBodies ? "Every player keeps the build of their own character (its Weight)."
                                 : "Every player is built for the Gear tier: the same muscle and grip.";
            break;
        case LobbyRow::KeepKit:
            r.label = "Loadouts";
            r.value = c.keepKit ? "Keep for the bracket" : "New each fight";
            r.hint = c.keepKit ? "Each player keeps the random loadout of their first fight for every fight of the bracket. A new tournament rolls new ones."
                               : "Both fighters get a new random loadout for every fight.";
            break;
        case LobbyRow::Scenario: {
            const std::string map = ScenarioMap(l);
            const Scenario sc = EffectiveScenario(map, c.scenario);   // a pick the map does not offer reads Standard
            r.label = "Scenario";
            r.value = ScenarioName(sc);
            r.hint = ScenarioHintFor(sc, ScenarioFormatOf(c.format));   // says so where the format does not use it
            r.enabled = l.host && ScenariosFor(map).size() > 1;
            break;
        }
        case LobbyRow::Sync:
            r.label = "Sync";
            r.value = c.hostPhysics ? "Host physics" : "Per player";
            r.hint = c.hostPhysics ? "The host runs every fighter's physics, so every screen shows the same fight. Your own moves show about one ping late. From the next map."
                                   : "Each player runs their own fighter: no delay on your own moves, but hits and grabs can differ between screens. From the next map.";
            break;
    }
    return r;
}

}  // namespace lobby_detail

// The rows the format uses, in page order. Every format has Damage, Dead weight and Weapon weight, in a row
// (Progression co-op shows them alone).
namespace lobby_detail {
inline std::vector<LobbyRow> FormatRows(const MatchConfig& c) {
    Format f = c.format;
    if (IsCrypts(c)) {                               // the Battle and the Gather time, then the battle format's own rows: the gathered kits are the gear
        MatchConfig own = c; own.gear = Gear::Own;
        std::vector<LobbyRow> v = {LobbyRow::Battle, LobbyRow::Gather};
        for (LobbyRow r : FormatRows(own)) {
            if (r == LobbyRow::Gear) continue;
            v.push_back(r);
            if (r == LobbyRow::Tier) v.push_back(LobbyRow::Weapons);   // the new character's rolled weapon (the Tier row: its rank)
        }
        return v;
    }
    const bool rolled = GearRolled(c.gear);          // the Weapons row filters generated gear only
    if (IsBotFormat(f)) {                            // endless, back at a wave clear in the run's loadout
        std::vector<LobbyRow> b = {LobbyRow::Time, LobbyRow::FreeCam, LobbyRow::Gear, LobbyRow::Tier};
        if (rolled) b.push_back(LobbyRow::Weapons);
        for (LobbyRow r : {LobbyRow::WaveBreak, LobbyRow::FriendlyFire, LobbyRow::Damage, LobbyRow::DeadWeight, LobbyRow::WeaponWeight, LobbyRow::Crossbows, LobbyRow::Sync}) b.push_back(r);
        return b;
    }
    if (IsBrawl(c)) return {LobbyRow::Limit, LobbyRow::Time, LobbyRow::Respawn, LobbyRow::FreeCam, LobbyRow::Damage, LobbyRow::DeadWeight, LobbyRow::WeaponWeight, LobbyRow::ClearGround, LobbyRow::Sync};   // everyone bare at kBrawlTier's strength
    if (IsRoulette(f)) return {LobbyRow::Limit, LobbyRow::Time, LobbyRow::Respawn, LobbyRow::FreeCam, LobbyRow::Damage, LobbyRow::DeadWeight, LobbyRow::WeaponWeight, LobbyRow::Sync};   // the same rows as One Crossbow
    if (IsOneCrossbow(f))                            // unarmed peasants and one crossbow: no gear rows, no Crossbows row
        return {LobbyRow::Limit, LobbyRow::Time, LobbyRow::Respawn, LobbyRow::FreeCam, LobbyRow::Damage, LobbyRow::DeadWeight, LobbyRow::WeaponWeight, LobbyRow::Sync};
    if (IsTag(f)) {                                  // rounds with a fuse: no clock, no teams
        return {LobbyRow::Limit, LobbyRow::Respawn, LobbyRow::Fuse, LobbyRow::TaggerSpeed, LobbyRow::FreeCam, LobbyRow::Damage, LobbyRow::DeadWeight, LobbyRow::WeaponWeight, LobbyRow::Sync};   // bare fists: no gear rows
    }
    if (IsInfection(f)) {                            // two fixed sides: the infection decides the teams, the colours and the crossbows
        std::vector<LobbyRow> t = {LobbyRow::Time, LobbyRow::Respawn, LobbyRow::FreeCam, LobbyRow::Gear, LobbyRow::Tier};
        if (rolled) t.push_back(LobbyRow::Weapons);
        for (LobbyRow r : {LobbyRow::Damage, LobbyRow::DeadWeight, LobbyRow::WeaponWeight, LobbyRow::Sync}) t.push_back(r);
        return t;
    }
    if (IsHideSeek(f)) return {LobbyRow::Limit, LobbyRow::Time, LobbyRow::Fuse, LobbyRow::Respawn, LobbyRow::FreeCam, LobbyRow::Damage, LobbyRow::DeadWeight, LobbyRow::WeaponWeight, LobbyRow::Sync};   // the Seek and the Hide time; the roles set the gear
    if (IsChaos(f))                                  // the rounds draw the gear, the builds, the damage and the weights: the match's own rules only
        return {LobbyRow::Limit, LobbyRow::Time, LobbyRow::Respawn, LobbyRow::FreeCam, LobbyRow::Crossbows, LobbyRow::Sync};
    if (HasRoles(f))                                 // the roles set the gear, tier, build and team: only the match's own rules are left
        return {LobbyRow::Limit, LobbyRow::Time, LobbyRow::Respawn, LobbyRow::FreeCam, LobbyRow::FriendlyFire, LobbyRow::Damage, LobbyRow::DeadWeight, LobbyRow::WeaponWeight, LobbyRow::TeamKit, LobbyRow::Crossbows, LobbyRow::Sync};
    std::vector<LobbyRow> v;
    if (f != Format::Tournament) { v.push_back(LobbyRow::Limit); v.push_back(LobbyRow::Time); }   // a bracket: one round a fight, no clock
    for (LobbyRow r : {LobbyRow::Respawn, LobbyRow::FreeCam, LobbyRow::Gear}) v.push_back(r);
    v.push_back(LobbyRow::Tier);                     // the tier is every player's strength, with own gear too
    if (rolled) v.push_back(LobbyRow::Weapons);
    if (HasTeams(f)) v.push_back(LobbyRow::Teams);
    if (HasTeams(f)) v.push_back(LobbyRow::FriendlyFire);
    for (LobbyRow r : {LobbyRow::Damage, LobbyRow::DeadWeight, LobbyRow::WeaponWeight}) v.push_back(r);
    if (!IsRoundFormat(f)) v.push_back(LobbyRow::ClearGround);
    if (HasTeams(f)) { v.push_back(LobbyRow::MyTeam); v.push_back(LobbyRow::TeamKit); }
    if (PicksTeams(f)) v.push_back(LobbyRow::Shuffle);
    if (!IsCrossbowTdm(f)) v.push_back(LobbyRow::Crossbows);    // Crossbow TDM hands everyone one: no row
    v.push_back(LobbyRow::Sync);
    return v;
}
}  // namespace lobby_detail
// The format's rows, and the Bodies row (#1410) after the Gear tier wherever the players fight in their own loadouts.
inline std::vector<LobbyRow> SettingsFor(const MatchConfig& c) {
    std::vector<LobbyRow> v = lobby_detail::FormatRows(c);
    const auto tier = std::find(v.begin(), v.end(), LobbyRow::Tier);
    if ((c.gear == Gear::Own || c.gear == Gear::Crypts) && tier != v.end()) v.insert(tier + 1, LobbyRow::Bodies);
    return v;
}

inline constexpr Format kModeOrder[kModeOrderCount] = {Format::FreeForAll, Format::Duel, Format::LastManStanding, Format::TwoTeams,
                                                       Format::TeamDuel, Format::Riot, Format::Juggernaut, Format::Tag, Format::Infection,
                                                       Format::CrossbowTDM, Format::Tournament, Format::PitBridgeDuel, Format::Chaos, Format::OneCrossbow, Format::Roulette, Format::HideSeek, Format::PlayersVsBots, Format::Zombies};

// Which formats a lobby with `players` players can run (the host's count, a dedicated server's own seat not counted).
// Duel is one on one; Last Man Standing and Team Duel need somebody to be the last of; Riot and Juggernaut need a side of
// two against the one; Tag, Infection and Crossbow TDM need a second player. `hint` says why not, for the card.
struct ModeAvail { bool ok = true; const char* hint = ""; };
inline ModeAvail ModeAvailable(Format f, int players) {
    if (f == Format::Duel && players != 2) return {false, "Duel is for 2 players"};
    if (IsRoulette(f) && players < 2) return {false, "Needs 2 or more players"};
    if (IsHideSeek(f) && players < 3) return {false, "Needs 3 or more players"};
    if ((f == Format::LastManStanding || IsChaos(f) || IsOneCrossbow(f) || f == Format::TeamDuel || IsTag(f) || IsInfection(f) || IsCrossbowTdm(f)) && players < 2) return {false, "Needs 2 or more players"};
    if ((HasRoleTeams(f) || f == Format::Tournament) && players < 3) return {false, "Needs 3 or more players"};
    if (f == Format::PitBridgeDuel && players < 2) return {false, "Needs 2 or more players"};
    return {};
}
// A third player joins a lobby set to Duel: it becomes Last Man Standing, keeping the round count and the gear rows.
inline bool DuelOutgrown(const MatchConfig& c, int players) { return c.format == Format::Duel && players > 2; }
inline MatchConfig AdjustForPlayers(MatchConfig c, int players) {
    if (DuelOutgrown(c, players)) c.format = Format::LastManStanding;
    return c;
}
inline constexpr const char* kDuelOutgrownLine = "Duel is for 2 players: switched to Last man standing";

// The group of the i-th mode card (kModeOrder, then Brawl, Crypts and Progression): known before any scene, so the page can
// build each card in its group's grid.
inline ModeGroup ModeGroupAt(size_t i) {
    if (i < kModeOrderCount) return IsBotFormat(kModeOrder[i]) ? ModeGroup::Coop : ModeGroup::Versus;
    return i <= kModeOrderCount + 1 ? ModeGroup::Versus : ModeGroup::Coop;   // Brawl, Crypts; Progression
}

inline LobbyScene BuildLobbyScene(const LobbyLocal& l) {
    using namespace lobby_detail;
    LobbyScene s;
    s.me = l.myName;
    s.regionButton = RegionPickLabel(l.regionPick, l.regionGuess);
    s.badge = "v" + l.version + " \xC2\xB7 protocol " + std::to_string(int(kProtocolRevision));
    switch (l.phase) {
        case LobbyPhase::Offline: s.pill = {"Steam not ready", style::kWarn}; break;
        case LobbyPhase::Idle: s.pill = {"Not in a lobby", style::kIron}; break;
        case LobbyPhase::Creating: s.pill = {"Creating a lobby", style::kWarn}; break;
        case LobbyPhase::Joining: s.pill = {"Joining a lobby", style::kWarn}; break;
        case LobbyPhase::InLobby: {
            if (l.host) { s.pill = {l.publicLobby ? "Hosting \xC2\xB7 public" : "Hosting \xC2\xB7 friends", style::kOk}; break; }
            std::string owner;
            for (const LobbyPlayer& p : l.players) if (p.host) owner = p.name;
            s.pill = {owner.empty() ? std::string("In a lobby") : "In " + owner + "'s lobby", style::kOk};
            break;
        }
    }
    s.idle = l.phase == LobbyPhase::Idle;
    s.in = l.phase == LobbyPhase::InLobby;
    if (s.in && !l.myClass.empty()) s.classLine = "YOUR CLASS: " + l.myClass;
    if (!s.in) s.status = l.status;
    if (s.idle) {
        s.filters = FilterChips(l.filter);
        std::vector<size_t> shown;
        for (size_t i = 0; i < l.found.size(); ++i) if (PassesFilter(l.found[i], l.filter)) shown.push_back(i);
        if (l.filter.mostPlayers)
            std::stable_sort(shown.begin(), shown.end(), [&](size_t a, size_t b) { return l.found[a].players > l.found[b].players; });
        if (!l.found.empty())
            s.browserCount = shown.size() == l.found.size() ? std::to_string(l.found.size()) + (l.found.size() == 1 ? " lobby" : " lobbies")
                           : std::to_string(shown.size()) + " of " + std::to_string(l.found.size()) + " lobbies shown";
        if (l.found.empty()) s.browserEmpty = "No public lobbies yet. REFRESH looks again.";
        else if (shown.empty()) s.browserEmpty = "No lobby matches these filters.";
        for (size_t at : shown) {
            if (s.browser.size() >= kBrowserRows) break;
            const LobbyFound& f = l.found[at];
            BrowserRow r;
            r.source = at;
            r.name = f.name.empty() ? "Unnamed lobby" : f.name;
            r.locked = f.locked;
            if (f.locked) r.name = kLockedMarker + r.name;
            r.players = std::to_string(f.players) + "/" + std::to_string(f.maxPlayers);
            if (RegionKnown(f.region)) r.detail = RegionLabel(f.region);
            if (!f.map.empty()) r.detail += (r.detail.empty() ? "" : " \xC2\xB7 ") + f.map;
            if (!f.mode.empty()) r.detail += (r.detail.empty() ? "" : " \xC2\xB7 ") + f.mode;
            if (!f.pack.empty()) r.detail += (r.detail.empty() ? "" : " \xC2\xB7 ") + f.pack;
            if (!f.mods.empty()) r.detail += (r.detail.empty() ? "" : " \xC2\xB7 ") + f.mods;
            if (!f.compatible && f.switchable && !LobbyIsFull(f.players, f.maxPlayers)) { r.note = {f.switchTag.empty() ? std::string("Restart") : f.switchTag, style::kWarn}; r.joinable = r.restart = true; }
            else if (!f.compatible) r.note = {f.reason.empty() ? std::string("Incompatible") : f.reason, style::kWarn};
            else if (LobbyIsFull(f.players, f.maxPlayers)) r.note = {"Full", style::kWarn};
            else { r.note = {"Join", style::kOk}; r.joinable = true; }
            s.browser.push_back(r);
        }
    }
    if (!s.in) return s;

    const MatchConfig& c = l.config;
    Format f = c.format;
    s.playersTitle = "PLAYERS " + std::to_string(l.playerCount >= 0 ? l.playerCount : int(l.players.size())) + "/" + std::to_string(l.maxPlayers);
    for (const LobbyPlayer& p : l.players) {
        RosterRow r;
        r.name = p.name.empty() ? "?" : p.name;
        r.host = p.host; r.ready = p.ready; r.me = p.me;
        if (l.host && !p.me && !p.steamName.empty() && p.steamName != p.name) r.steam = p.steamName;   // the host sees who is behind a nickname
        if (PicksTeams(f)) {
            int t = EffectiveTeam(p, f, c.teams);
            r.team = TeamLabel(t);
            r.swatch = TeamColor(t);                                // none for Auto
            r.moved = ValidTeam(p.hostOverride, f, c.teams);
            r.movable = l.host;
        }
        s.roster.push_back(r);
    }
    if (!l.host) {
        s.ready = true;
        for (const LobbyPlayer& p : l.players) if (p.me) s.readyOn = p.ready;
        s.readyLabel = s.readyOn ? "Ready \xC2\xB7 click to undo" : "Mark me ready";
    }
    s.hostPassword = l.host;
    if (l.host) s.passwordNote = l.locked ? "Locked: players need the password. Changing it kicks nobody."
                                          : "Open: type a password and press SET to lock the lobby.";
    s.balance = l.host && PicksTeams(f);
    s.shuffle = l.host && PicksTeams(f);
    const int players = l.playerCount >= 0 ? l.playerCount : int(l.players.size());
    for (Format m : kModeOrder) {
        const ModeAvail av = ModeAvailable(m, players);
        ModeCard mc;
        mc.format = m; mc.name = Upper(FormatName(m)); mc.description = FormatBlurb(m);
        mc.selected = m == f && !IsBrawl(c) && !IsCrypts(c) && !l.progression; mc.enabled = l.host && av.ok;
        if (!av.ok) mc.hint = av.hint;
        s.modes.push_back(mc);
    }
    {
        ModeCard b;
        b.format = Format::FreeForAll;
        b.name = Upper(ModeName(Format::FreeForAll, Gear::Fists));
        b.description = kBrawlBlurb;
        b.selected = IsBrawl(c) && !l.progression; b.enabled = l.host; b.brawl = true;
        s.modes.push_back(b);
    }
    {
        ModeCard k;
        k.format = IsCrypts(c) ? f : kCryptsDefaultBattle;
        k.name = "CRYPTS";
        k.description = kCryptsBlurb;
        k.selected = IsCrypts(c) && !l.progression; k.enabled = l.host; k.crypts = true;
        s.modes.push_back(k);
    }
    {
        ModeCard p;
        p.name = "PROGRESSION (CO-OP)";
        p.description = "Stock Progression in the Tavern, together. Free Mode off; your own save is untouched.";
        p.selected = l.progression; p.enabled = l.host; p.progression = true;
        s.modes.push_back(p);
    }
    for (size_t i = 0; i < s.modes.size(); ++i) s.modes[i].group = ModeGroupAt(i);
    s.playerLimit = Row(l, LobbyRow::Players);
    s.hasPlayerLimit = true;
    s.scenarioMap = lobby_detail::ScenarioMap(l);
    if (!l.progression) for (LobbyRow id : SettingsFor(c)) s.settings.push_back(Row(l, id));
    else for (LobbyRow id : {LobbyRow::Damage, LobbyRow::DeadWeight, LobbyRow::WeaponWeight}) s.settings.push_back(Row(l, id));   // co-op fights take them too
    if (!l.progression && MapHasDayTime(s.scenarioMap)) { s.timeOfDay = Row(l, LobbyRow::TimeOfDay); s.hasTimeOfDay = true; }   // only the arenas with a day cycle
    if (!l.progression && WinByApplies(c)) { s.winBy = Row(l, LobbyRow::WinBy); s.hasWinBy = true; }   // MATCH section, after the format's rows
    if (!l.progression) { s.props = Row(l, LobbyRow::Props); s.hasProps = true; }   // WORLD section, before the scenario
    if (!l.progression) { s.strip = Row(l, LobbyRow::Strip); s.hasStrip = true; }   // GEAR section
    if (!l.progression && c.format == Format::Tournament && c.gear == Gear::RandomTier) { s.keepKit = Row(l, LobbyRow::KeepKit); s.hasKeepKit = true; }   // GEAR section
    if (!l.progression && ScenariosFor(s.scenarioMap).size() > 1) { s.scenario = Row(l, LobbyRow::Scenario); s.hasScenario = true; }   // WORLD section, before Sync
    if (l.classBudget) { s.classPower = Row(l, LobbyRow::ClassPower); s.hasClassPower = true; }   // MATCH section
    if (!l.host) s.hostOnly = "Only the host can change these.";
    size_t customChosen = l.modMaps.size();   // the picked custom map (none: l.modMaps.size())
    if (f != Format::Zombies)
        for (size_t i = 0; i < l.modMaps.size(); ++i) if (lobby_detail::SamePath(l.modMaps[i].path, l.mapPick)) customChosen = i;
    const size_t chosen = customChosen < l.modMaps.size() ? MapCount() : EffectiveMap(f, c.gear, l.mapPick);
    for (size_t i = 0; i < MapCount() && !l.progression; ++i) {
        if (!kMaps[i].pick) continue;
        MapCard m;
        m.index = i;
        m.name = kMaps[i].display;
        m.tag = MapTag(kMaps[i]);
        m.allowed = MapAllowsMatch(kMaps[i], f, c.gear);
        if (!m.allowed) m.reason = MapRefusal(f);
        m.selected = i == chosen;
        m.enabled = l.host && m.allowed;
        s.maps.push_back(m);
    }
    for (size_t i = 0; i < l.modMaps.size() && !l.progression; ++i) {   // modded maps follow the stock ones
        MapCard m;
        m.index = MapCount() + i;
        m.modded = true;
        m.path = l.modMaps[i].path;
        m.name = l.modMaps[i].display;
        m.tag = l.modMaps[i].pak.empty() ? "Modded map" : "Modded map \xC2\xB7 " + l.modMaps[i].pak;
        m.allowed = f != Format::Zombies && f != Format::PitBridgeDuel;   // the Abyss is the only Zombies map, the Pit the only bridge
        if (!m.allowed) m.reason = MapRefusal(f);
        m.selected = i == customChosen;
        m.enabled = l.host && m.allowed;
        s.maps.push_back(m);
    }
    std::vector<PeerBuild> others;
    for (const LobbyPlayer& p : l.players) if (!p.me) others.push_back({p.name, p.build});
    const std::string warn = BuildWarning(l.myBuild, others);
    s.buildMismatch = !warn.empty();
    s.start = l.host && MayStartMatch(l.myBuild, others, l.allowBuildMismatch);
    const ModeAvail playable = l.progression ? ModeAvail{} : ModeAvailable(f, players);   // Duel needs exactly 2, Last Man Standing 2 or more
    const bool modWait = l.host && customChosen < l.modMaps.size() && l.modPickPending;
    if (modWait || !playable.ok) s.start = false;
    // The line beside START: what will be played.
    if (l.progression) s.summary = "PROGRESSION \xC2\xB7 the Tavern, together";
    else {
        s.summary = Upper(ModeName(f, c.gear, c.teams));
        for (const MapCard& m : s.maps) if (m.selected) s.summary += " \xC2\xB7 " + m.name;
        const bool gearRows = !IsBare(c) && !HasRoles(f);               // Riot and Juggernaut deal the gear by role; Brawl and Tag are bare
        if (gearRows) s.summary += " \xC2\xB7 " + std::string(GearName(c.gear));
        if (gearRows && GearRolled(c.gear)) s.summary += ", " + std::string(TierName(c.tier));
        if (gearRows && GearRolled(c.gear) && c.weapons != 0) s.summary += ", " + WeaponMaskName(c.weapons);
        if (IsCrypts(c)) s.summary += ", " + std::to_string(c.gatherS / 60) + " min";
    }
    if (!l.host) s.waiting = "Waiting for the host";
    if (modWait) s.waiting = "Checking the map pack";
    if (l.host && !playable.ok) s.waiting = playable.hint;
    if (s.buildMismatch) s.waiting = l.host && l.allowBuildMismatch ? warn + " (allowed: allow_build_mismatch=1)" : warn;
    return s;
}

}  // namespace hsm
