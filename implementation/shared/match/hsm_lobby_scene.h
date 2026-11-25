// Lobby page (MATCH_ROUNDS_UI_CHAT_PLAN 3.4): what the multiplayer page shows, as data. A header with the state pill
// and the version badge; left, the lobby browser (not in a lobby) or the players (in one); middle, the mode cards and
// the setting rows with their steppers and one-line hints for the current format; right, the map cards and START
// MATCH. Members see everything the host chose, disabled, except their own team, and mark themselves ready (the
// host starts the match instead). The page (ui/menu.cpp) fills a LobbyLocal from the Steam lobby and maps the scene
// onto widgets; the steppers' arithmetic is here too.
#pragma once
#include "match/hsm_crypts.h"
#include "match/hsm_dropdown.h"
#include "match/hsm_hud_scene.h"
#include "match/hsm_kept_kit.h"
#include "match/hsm_map_list.h"
#include "match/hsm_mode_merge.h"
#include "match/hsm_start_gate.h"
#include "match/hsm_ui_text.h"
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
    bool joined = false;                // the host has this player's Hello (the page's host side only): an empty build is then an older HSMMod
    std::string steamName;              // their Steam name when it is not the name shown (a nickname), for the host's row; empty: nothing to add
};

// The public-lobby list's filters (the page's chips, in this order): region, mode, room, compatibility, sorting, passwords.
// All off: every lobby Steam returned, in its order.
struct BrowserFilter {
    uint8_t mode = 0;                   // 0: any; else the kBrowserModes entry shown
    bool hasRoom = false, compatibleOnly = false, mostPlayers = false;
    bool noPassword = false;            // hide the locked lobbies
    uint8_t region = 0;                 // 0: any region (asks Steam worldwide), 1: my region (what Steam finds nearby), 2+: one region (RegionFilterAt)
    Region mine = Region::Auto;         // this player's own region, set by the page; Auto: not known, so "my region" hides nothing
};

// What a locked lobby's name starts with in the browser: plain Latin-1/ASCII, the game's font has no padlock glyph.
// The one place to change it.
inline constexpr const char* kLockedMarker = "[locked] ";

struct LobbyFound {                     // a public lobby from Find
    std::string name, map, mode;
    uint8_t scenario = 0;               // the lobby data hsm_scen: with the advertised mode it tells an older host's Brawling or Blossfechten (BrowserModeFor)
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
    std::string fightPick;              // the host's Fight map pick (lobby key hsm_fmap): where a Crypt Royale's Battle is fought; empty: CryptsDefaultMap
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

// The rows, in the order a section shows them: the RULES tab lists each sub-tab's rows by this order, and ui/menu.cpp makes one
// stepper per row up to Sync in it. A row's sub-tab is SectionOf(), the one place that classifies them; the groups below
// follow the sections (MATCH, COMBAT, TEAMS, GEAR, WORLD). Players is the lobby's own player limit, not a MatchConfig field
// (the page applies it with Lobby::SetMaxPlayers); the Crypts modes' Battle (the format fought after the loot run), Gather time
// (#3199; Grave Robbers calls it Run time), Crypts run, Dungeon tier (#3350) and Fight map (the map the Battle is fought on, lobby key
// hsm_fmap, not a MatchConfig field) follow it. Tier is the Crypts card's "Starting tier": everywhere else the tier is the chips
// under Gear (SettingRow::tierChips).
enum class LobbyRow : uint8_t {
    Players, Battle, Gather, CryptsRun, CryptsTier, FightMap, Limit, Time, WinBy, Respawn, Fuse, TaggerSpeed, ClassPower,            // MATCH
    Damage, DeadWeight, WeaponWeight, Bite,                                                                              // COMBAT
    Teams, FriendlyFire, MyTeam, TeamKit, Shuffle,                                                                       // TEAMS
    Gear, Tier, Bodies, Weapons, Crossbows, Strip, KeepKit,                                                              // GEAR
    TimeOfDay, WaveBreak, ClearGround, Props, Sync                                                                       // WORLD; Sync last: the menu builds rows up to it
};

// How a row is changed: a stepper (a number, stepped with - and + and typed into), a choice (a name picked from a dropdown),
// a toggle (On and Off), or chips (the Weapons row's tickable types; the Starting tier's eight). RowKindOf says which.
enum class RowKind : uint8_t { Stepper, Choice, Toggle, Chips };
// A chip a row shows in place of its arrows (the Weapons row's tickable types): a click on chip `i` toggles it. The row's value
// still reads out the whole choice.
struct RowChip { std::string label; bool on = false; };
// tierChips: the eight tiers, on the row that has a tier to pick (Gear while the tier is used, the Crypts card's Starting tier):
// a click on chip `i` sets the tier to i. Under Gear the row keeps its arrows; the Starting tier row has chips alone
// (RowHasArrows). Empty: the row has no tier.
// choices / choice: a Choice row's dropdown, every option the row can take in the order of the list, and the one now picked
// (choices[choice].label is value). Options a click cannot pick yet are disabled with the reason.
struct SettingRow { LobbyRow id = LobbyRow::Limit; std::string label, value, hint; bool enabled = false; std::vector<RowChip> chips; std::vector<RowChip> tierChips;
                    RowKind kind = RowKind::Stepper; std::vector<DropOption> choices; size_t choice = 0; };
// Whether the row shows its arrows: not the Weapons row (its chips replace them), nor the Starting tier (its chips are the choice),
// nor a Choice row (its value opens a list).
inline bool RowHasArrows(const SettingRow& r) {
    return r.chips.empty() && r.kind != RowKind::Choice && !(r.id == LobbyRow::Tier && !r.tierChips.empty());
}
// The mode cards' groups on the MODE tab (Party joins them with the party modes).
enum class ModeGroup : uint8_t { Versus, Coop };
struct ModeCard { Format format = Format::FreeForAll; std::string name, description; bool selected = false, enabled = false; bool progression = false; bool brawl = false;
                  bool crypts = false; bool blossfechten = false; ModeGroup group = ModeGroup::Versus; std::string hint;
                  bool graveRobbers = false; bool shown = true;
                  bool coopCrypts = false; };   // coopCrypts: the CRYPTS (CO-OP) card, the game's own Crypts together (IsCoopCrypts); shown on every map with g_cryptsCoopOffered   // progression: the extra last card; brawl: free-for-all with gear = Fists; crypts: gear = Crypts (the CRYPTS card picks the Crypts map and Crypt Royale; Crypt Royale's Battle row picks the format; graveRobbers: Grave Robbers); blossfechten: Last man standing with the Blossfechten scenario; hint: why a card is disabled for the player count (ModeAvailable); shown: the card is on the page (the CRYPTS card only off the Crypts map, Crypt Royale and Grave Robbers on it; Grave Robbers also needs g_cryptsCoopOffered)
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

// The RULES tab's sub-tabs, in page order; each row sits in one.
enum class LobbySection : uint8_t { Match, Combat, Teams, Gear, World, Count };
inline constexpr size_t kSectionCount = size_t(LobbySection::Count);
// No `default:` on purpose: a new row is a build error here until it has a sub-tab.
inline LobbySection SectionOf(LobbyRow r) {
    switch (r) {
        case LobbyRow::Players: case LobbyRow::Battle: case LobbyRow::Gather: case LobbyRow::CryptsRun: case LobbyRow::CryptsTier:
        case LobbyRow::FightMap: case LobbyRow::Limit: case LobbyRow::Time: case LobbyRow::WinBy: case LobbyRow::Respawn:
        case LobbyRow::Fuse: case LobbyRow::TaggerSpeed: case LobbyRow::ClassPower: return LobbySection::Match;
        case LobbyRow::Damage: case LobbyRow::DeadWeight: case LobbyRow::WeaponWeight: case LobbyRow::Bite: return LobbySection::Combat;
        case LobbyRow::Teams: case LobbyRow::FriendlyFire: case LobbyRow::MyTeam: case LobbyRow::TeamKit: case LobbyRow::Shuffle: return LobbySection::Teams;
        case LobbyRow::Gear: case LobbyRow::Tier: case LobbyRow::Bodies: case LobbyRow::Weapons: case LobbyRow::Crossbows:
        case LobbyRow::Strip: case LobbyRow::KeepKit: return LobbySection::Gear;
        case LobbyRow::TimeOfDay: case LobbyRow::WaveBreak: case LobbyRow::ClearGround: case LobbyRow::Props:
        case LobbyRow::Sync: return LobbySection::World;
    }
    return LobbySection::Match;
}
inline const char* SectionName(LobbySection s) {
    switch (s) {
        case LobbySection::Match: return "MATCH";
        case LobbySection::Combat: return "COMBAT";
        case LobbySection::Teams: return "TEAMS";
        case LobbySection::Gear: return "GEAR";
        case LobbySection::World: return "WORLD";
        case LobbySection::Count: break;
    }
    return "MATCH";
}

// The browser's mode chip steps through these (0: any).
inline const std::vector<std::string>& BrowserModes() {
    static const std::vector<std::string> m = {"Any mode", kCarnageName, kDuelCardChip, "Teams", "Team duel", "Riot", "Juggernaut", "Tag", "Infection", "Crossbow TDM", "Tournament", kBlossfechtenName, "Chaos", "One crossbow", "Roulette", "Hide and seek", kBrawlName, kCryptRoyaleName, kGraveRobbersName, "Survival", "Zombies"};
    return m;
}
// The region chip steps through any region, my region and the six regions (kRegionCount - 1 of them, Auto left out).
inline constexpr uint8_t kRegionFilterStates = uint8_t(2 + kRegionCount - 1);
inline constexpr uint8_t kRegionAny = 0, kRegionMine = 1;   // the first two stops; the regions follow
// The chip's text. My region names the region when it is known: "My region (NA East)".
inline std::string RegionFilterLabel(uint8_t state, Region mine = Region::Auto) {
    if (state == kRegionMine) return RegionKnown(mine) ? std::string("My region (") + RegionLabel(mine) + ")" : std::string("My region");
    if (state > kRegionMine && state < kRegionFilterStates) return RegionLabel(Region(state - 1));
    return "Any region";
}
// The region a chip state shows (Auto: none, for any region and my region).
inline Region RegionFilterAt(uint8_t state) { return state > kRegionMine && state < kRegionFilterStates ? Region(state - 1) : Region::Auto; }
// How far Steam is asked to look: only my region stays with what Steam finds nearby.
inline LobbyDistance DistanceFor(const BrowserFilter& f) { return f.region == kRegionMine ? LobbyDistance::Default : LobbyDistance::Worldwide; }

// The filter chips, in the order the page draws them. The region leads the row.
inline constexpr size_t kRegionChip = 0, kModeChip = 1, kRoomChip = 2, kCompatibleChip = 3, kMostPlayersChip = 4, kPasswordChip = 5;
inline constexpr size_t kFilterChips = 6;
struct FilterChip { std::string label; bool on = false; bool choice = false; };   // choice: the chip opens a list (the region, the mode)
inline std::vector<FilterChip> FilterChips(const BrowserFilter& f) {
    const auto& modes = BrowserModes();
    std::vector<FilterChip> v(kFilterChips);
    v[kRegionChip] = {RegionFilterLabel(f.region, f.mine), f.region != kRegionAny, true};
    v[kModeChip] = {modes[f.mode < modes.size() ? f.mode : 0], f.mode != 0, true};
    v[kRoomChip] = {"Has room", f.hasRoom};
    v[kCompatibleChip] = {"Compatible only", f.compatibleOnly};
    v[kMostPlayersChip] = {"Most players first", f.mostPlayers};
    v[kPasswordChip] = {"No password", f.noPassword};
    return v;
}
// The region and the mode pick from a list (FilterChoices); the other chips are toggles.
inline bool FilterChipIsChoice(size_t chip) { return chip == kRegionChip || chip == kModeChip; }
// The list a choice chip opens: any region, my region and the six regions; any mode and every mode. Empty for a toggle.
inline std::vector<DropOption> FilterChoices(const BrowserFilter& f, size_t chip) {
    std::vector<DropOption> v;
    if (chip == kRegionChip) for (uint8_t i = 0; i < kRegionFilterStates; ++i) v.push_back({RegionFilterLabel(i, f.mine), true, ""});
    else if (chip == kModeChip) for (const std::string& m : BrowserModes()) v.push_back({m, true, ""});
    return v;
}
// The option of that list the filter shows now (a stale value reads the first option, as the chip's text does).
inline size_t FilterChoiceAt(const BrowserFilter& f, size_t chip) {
    return chip == kRegionChip ? (f.region < kRegionFilterStates ? size_t(f.region) : 0) : chip == kModeChip ? (f.mode < BrowserModes().size() ? size_t(f.mode) : 0) : 0;
}
// Option `i` of a choice chip's list picked.
inline BrowserFilter WithFilterChoice(BrowserFilter f, size_t chip, size_t i) {
    if (chip == kRegionChip && i < kRegionFilterStates) f.region = uint8_t(i);
    else if (chip == kModeChip && i < BrowserModes().size()) f.mode = uint8_t(i);
    return f;
}
// A chip clicked: the four toggles turn on or off. The region and the mode also step on here (the mode wraps to any); the page
// opens their lists instead (FilterChoices).
inline BrowserFilter ToggleFilter(BrowserFilter f, size_t chip) {
    switch (chip) {
        case kRegionChip: f.region = uint8_t((f.region + 1) % kRegionFilterStates); break;
        case kModeChip: f.mode = uint8_t((f.mode + 1) % BrowserModes().size()); break;
        case kRoomChip: f.hasRoom = !f.hasRoom; break;
        case kCompatibleChip: f.compatibleOnly = !f.compatibleOnly; break;
        case kMostPlayersChip: f.mostPlayers = !f.mostPlayers; break;
        case kPasswordChip: f.noPassword = !f.noPassword; break;
        default: break;
    }
    return f;
}
// The mode a listed lobby shows and is filtered by (hsm_mode_merge.h BrowserModeFor): an older host's "Free-for-all" and Pit bridge
// duels are Carnage, its Brawling scenario Brawl, and Duel and Last man standing are one, read by the row's player count.
inline std::string BrowserModeShown(const LobbyFound& l) { return BrowserModeFor(l.mode, l.scenario, l.players, l.maxPlayers); }
// A lobby against a browser entry: equal, or the entry plus the detail the host adds ("Teams (3)" is "Teams").
inline bool ModeMatches(const LobbyFound& l, const std::string& entry) { return BrowserModeMatches(BrowserModeShown(l), entry); }
inline bool PassesFilter(const LobbyFound& l, const BrowserFilter& f) {
    const auto& modes = BrowserModes();
    if (f.mode != 0 && f.mode < modes.size() && !ModeMatches(l, modes[f.mode])) return false;
    if (f.hasRoom && LobbyIsFull(l.players, l.maxPlayers)) return false;   // each lobby's own limit
    if (f.compatibleOnly && !l.compatible && !l.switchable) return false;   // a restart away still counts
    if (f.noPassword && l.locked) return false;
    if (f.region == kRegionMine) return !RegionKnown(f.mine) || !RegionKnown(l.region) || l.region == f.mine;   // a lobby that did not say, or no region of mine: nothing to hide
    if (f.region > kRegionMine) return l.region == RegionFilterAt(f.region);                                      // a region asked for: only lobbies that say they are in it
    return true;
}
// The chips that hide a lobby, each checked on its own: bit i is FilterChips(f)[i]; 0 when no chip hides it.
inline uint32_t HidingChips(const LobbyFound& l, const BrowserFilter& f) {
    BrowserFilter one[kFilterChips] = {};   // the default filter hides nothing (any region)
    one[kRegionChip].region = f.region;
    one[kRegionChip].mine = f.mine;
    one[kModeChip].mode = f.mode;
    one[kRoomChip].hasRoom = f.hasRoom;
    one[kCompatibleChip].compatibleOnly = f.compatibleOnly;
    one[kPasswordChip].noPassword = f.noPassword;
    uint32_t bits = 0;
    for (size_t i = 0; i < kFilterChips; ++i) if (!PassesFilter(l, one[i])) bits |= 1u << i;
    return bits;
}
// The empty list's line when lobbies were found but the filters hide them all: how many, and the chips that do it.
inline std::string HiddenByFiltersText(const std::vector<LobbyFound>& found, const BrowserFilter& f) {
    uint32_t bits = 0;
    for (const LobbyFound& l : found) bits |= HidingChips(l, f);
    const std::vector<FilterChip> chips = FilterChips(f);
    std::string names;
    for (size_t i = 0; i < chips.size(); ++i) {
        if (!(bits & (1u << i))) continue;
        names += (names.empty() ? "" : ", ") + (i == kRegionChip ? RegionFilterLabel(f.region) : chips[i].label);   // plain words: no region in brackets inside the brackets
    }
    std::string s = std::to_string(found.size()) + (found.size() == 1 ? " lobby" : " lobbies") + " hidden by your filters";
    return names.empty() ? s + "." : s + " (" + names + ").";
}
// The format cards, in page order: Duel is no card of its own (Last man standing reads Duel with two players, ModeCardName), nor are
// Pit bridge duels (gone from the lobby), ThreeTeams (the old value of the Teams card's 3 teams) and CaptureTheFlag (removed);
// Blossfechten is a card of Last man standing's rounds. Then Brawl, the CRYPTS card (off the Crypts map; it picks that map), Progression,
// and on the Crypts map Crypt Royale and Grave Robbers (the Crypts modes, picked there).
struct ModeSlot { Format format; bool blossfechten; };
inline constexpr ModeSlot kModeOrder[] = {
    {Format::FreeForAll, false}, {Format::LastManStanding, false}, {Format::TwoTeams, false}, {Format::TeamDuel, false}, {Format::Riot, false},
    {Format::Juggernaut, false}, {Format::Tag, false}, {Format::Infection, false}, {Format::CrossbowTDM, false}, {Format::Tournament, false},
    {Format::LastManStanding, true}, {Format::Chaos, false}, {Format::OneCrossbow, false}, {Format::Roulette, false}, {Format::HideSeek, false},
    {Format::PlayersVsBots, false}, {Format::Zombies, false}};
inline constexpr size_t kModeOrderCount = sizeof(kModeOrder) / sizeof(kModeOrder[0]);
inline constexpr size_t kModeCardCount = kModeOrderCount + 6;   // + Brawl, CRYPTS, Progression, Crypt Royale, Grave Robbers, Crypts (co-op)

struct LobbyScene {
    Span pill;
    std::string me, badge, status;      // status: not in a lobby only (in one, the pill says it)
    std::string regionButton;           // my region as SETTINGS shows it: "Auto (Europe)", "Auto (unknown)", "NA East"
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
    bool balance = false;               // Auto-balance: the host, in the formats with teams
    bool shuffle = false;               // SHUFFLE NOW: the host, in the formats with teams (deals everyone at random into the lobby's moves)
    std::string classLine;              // in a lobby that uses classes: which one we fight as, beside CHANGE; empty: no row
    std::vector<ModeCard> modes;        // the grid's reading order, two to a row
    std::vector<SettingRow> settings;   // every row of the RULES tab, in LobbyRow order (SceneRows): the Players limit and the map's own rows among them
    std::string rulesNote;              // Progression: why RULES holds only three rows ("The game's own rules, in the Tavern."); empty in every other mode
    std::string hostOnly;               // members: "Only the host can change these."
    std::vector<MapCard> maps;          // the pickable maps, kMaps order; none for Progression
    bool start = false;                 // START MATCH may be clicked (match/hsm_start_gate.h). The button shows in every lobby, dimmed when this is false
    std::string startWhy;               // when !start: what a click on the dimmed button says (the gate's reason)
    std::string waiting;                // the line under START: why it is dimmed, or what the host overrode; a member reads "Waiting for the host to start."
    std::string summary;                // beside START: the mode, the map and the gear, one line
    bool buildMismatch = false;         // another player is on a different build (waiting says who, in the warning colour)
    bool waitingWarn = false;           // `waiting` is in the warning colour: the host is blocked, or another player is on another build
};

// ------------------------------------------------------------------ steppers

// The option before `cur`, wrapping; an unknown value steps to the last (NextOf steps it to the first).
inline int PrevOf(const std::vector<int>& options, int cur) {
    if (options.empty()) return cur;
    auto it = std::find(options.begin(), options.end(), cur);
    return it == options.end() || it == options.begin() ? options.back() : *(it - 1);
}
// The Dungeon tier row's value (#3350): the rank it holds the Crypts at, or the game's own.
inline std::string CryptsTierLabel(uint8_t t) { return t >= kCryptsTierTop ? "Game's own" : TierName(t); }
inline int StepOf(const std::vector<int>& options, int cur, int dir) { return dir < 0 ? PrevOf(options, cur) : NextOf(options, cur); }

inline std::vector<int> LimitOptions(Format f) {
    if (f == Format::Tournament) return {1};                          // one round a fight (no Limit row)
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
inline constexpr Format kCryptsBattles[] = {Format::LastManStanding, Format::FreeForAll, Format::TwoTeams, Format::TeamDuel, Format::Tournament, Format::PlayersVsBots};   // Duel is Last man standing's two-player name
inline constexpr size_t kCryptsBattleCount = sizeof(kCryptsBattles) / sizeof(kCryptsBattles[0]);
inline constexpr Format kCryptsDefaultBattle = Format::LastManStanding;
inline const std::vector<int> kTaggerSpeedStops(std::begin(kTaggerSpeedOptions), std::end(kTaggerSpeedOptions));   // Tag's tagger speed, percent
// The stops of the Shuffle teams row: Each round only where the format has rounds to deal for (Sanitize).
inline std::vector<int> ShuffleOptions(Format f) { return IsTeamRound(f) ? std::vector<int>{0, 1, 2} : std::vector<int>{0, 1}; }
// The wave break in seconds the match runs (0 = the format's own: Survival 60, Zombies 10).
// The Clear ground row's value: "Every 30 s", "Every 1 min", "Every 2 min" .. "Every 10 min"; "Never" for the 0 that no step reaches (an older host's Off).
inline std::string ClearGroundLabel(uint16_t s) {
    if (s == 0) return "Never";
    return "Every " + (s < 60 ? std::to_string(s) + " s" : std::to_string(s / 60) + " min");
}
inline int WaveBreakShown(const MatchConfig& c) { return c.waveBreakS ? c.waveBreakS : c.format == Format::Zombies ? int(Match::kZombieBreakMs / 1000) : int(Match::kWaveBreakMs / 1000); }

// Whether the lobby uses a tier: the bots start at it, the Crypts character starts at it, and random or identical gear is
// rolled at it. Then the tier chips show (under Gear, or as the Crypts card's Starting tier) and Bodies can read By tier.
inline bool TierInUse(const MatchConfig& c) { return IsCrypts(c) || IsBotFormat(c.format) || GearRolled(c.gear); }

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
        case LobbyRow::FightMap: break;                         // the Fight map is the lobby's own pick (hsm_fmap), not a config field: LobbyLocal::fightPick
        case LobbyRow::CryptsRun: c.cryptsTogether = !c.cryptsTogether; break;
        case LobbyRow::CryptsTier: c.cryptsTier = StepCryptsTier(c.cryptsTier, dir); break;
        case LobbyRow::Limit: c.scoreLimit = uint16_t(StepOf(LimitOptions(c.format), c.scoreLimit, dir)); break;
        case LobbyRow::Time: c.timeLimitS = uint16_t(StepOf(IsInfection(c.format) ? kInfectionTimeOptions : kTimeOptions, c.timeLimitS, dir)); break;
        case LobbyRow::Fuse: c.fuseS = uint8_t(StepOf(kFuseOptions, c.fuseS, dir)); break;
        case LobbyRow::TaggerSpeed: c.taggerSpeedPct = uint8_t(StepOf(kTaggerSpeedStops, c.taggerSpeedPct, dir)); break;
        case LobbyRow::Shuffle: c.shuffle = TeamShuffle(StepOf(ShuffleOptions(c.format), int(c.shuffle), dir)); break;
        case LobbyRow::WinBy: c.winBy = uint8_t(StepWinBy(c.winBy, dir)); break;
        case LobbyRow::Props: c.propsOff = !c.propsOff; break;
        case LobbyRow::Strip: c.stripOff = !c.stripOff; break;
        case LobbyRow::Bite: c.biteOff = !c.biteOff; break;
        case LobbyRow::Bodies: c.ownBodies = !c.ownBodies; break;
        case LobbyRow::KeepKit: c.keepKit = !c.keepKit; break;
        case LobbyRow::Respawn: c.respawnDelayS = uint16_t(StepOf(kRespawnOptions, c.respawnDelayS, dir)); break;
        case LobbyRow::Gear: {
            size_t at = 0;                           // Fists (the Brawl card) is no stop of the row: it steps as Own does
            for (size_t i = 0; i < kGearRowCount; ++i) if (kGearRow[i] == c.gear) at = i;
            c.gear = kGearRow[(at + (back ? kGearRowCount - 1 : 1)) % kGearRowCount];
            if (!TierInUse(c)) c.tier = MatchConfig{}.tier;   // the chips are gone: a tier picked before must not go on setting the Same for all build unseen
            break;
        }
        case LobbyRow::Tier: c.tier = uint8_t((std::min<uint8_t>(c.tier, 7) + (back ? 7 : 1)) % 8); break;
        case LobbyRow::Weapons: break;                           // the host ticks the types: ToggleWeaponType(SettingRow::chips)
        case LobbyRow::Crossbows:
            c.crossbows = uint8_t((ValidCrossbows(c.crossbows) ? c.crossbows : 0) + (back ? kCrossbowModeCount - 1 : 1)) % kCrossbowModeCount;
            break;
        case LobbyRow::Teams: c.teams = uint8_t(StepOf({2, 3, 4}, c.teams, dir)); break;
        case LobbyRow::FriendlyFire: c.friendlyFire = !c.friendlyFire; break;
        case LobbyRow::Damage: c.damagePct = StepDamage(c.damagePct, dir, damage); break;
        case LobbyRow::DeadWeight: c.deadWeightPct = StepWeight(c.deadWeightPct, dir); break;
        case LobbyRow::WeaponWeight: c.weaponWeightPct = StepWeight(c.weaponWeightPct, dir); break;
        case LobbyRow::WaveBreak: {
            std::vector<int> o(std::begin(kWaveBreakOptions), std::end(kWaveBreakOptions));
            c.waveBreakS = uint16_t(StepOf(o, WaveBreakShown(c), dir)); break;
        }
        case LobbyRow::TimeOfDay: c.timeOfDay = uint8_t(StepOf({1, 2, 3, 4}, TimeOfDayRead(c.timeOfDay), dir)); break;   // no Map's own: Morning, Midday, Evening, Night
        case LobbyRow::ClearGround: {                          // 30 s .. 10 min, no Off
            std::vector<int> o(std::begin(kClearGroundOptionsS), std::end(kClearGroundOptionsS));
            c.clearGroundS = uint16_t(StepOf(o, c.clearGroundS, dir)); break;
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
// A format that hides the Clear ground row has it at 0 (Sanitize): coming back to one that shows it, the row starts at its default, not at "Never".
inline MatchConfig ClearGroundBack(MatchConfig c) {
    if (c.clearGroundS == 0 && ClearGroundApplies(c.format)) c.clearGroundS = kClearGroundDefaultS;
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
    return ClearGroundBack(c);
}

// A mode card picked: the Brawl card is free-for-all with bare fists (gear Fists, kBrawlTier, free-for-all's default limit);
// the Crypts cards are gear Crypts with Last man standing to start (Crypt Royale's Battle row picks another): Crypt Royale, or
// Grave Robbers (`graveRobbers`, no fight after), or Co-op Crypts (`coopCrypts`: Grave Robbers with the game's own rules); the Blossfechten card is Last
// man standing's rounds with the Blossfechten scenario and its fixed kit (BlossfechtenKit: random swords, no armour); the
// format cards are SelectFormat, and leaving Brawl, Crypts or Blossfechten puts the gear, tier and weapons back at their
// defaults (even for the card of the same format: other rules). The Duel card is Last man standing (a Duel config reads as it).
inline MatchConfig SelectMode(MatchConfig c, Format f, bool brawl, bool crypts = false, bool blossfechten = false, bool graveRobbers = false,
                              bool coopCrypts = false) {
    if (coopCrypts) { crypts = true; graveRobbers = true; }
    if (f == Format::Duel) f = Format::LastManStanding;
    const bool wasBlossfechten = IsBlossfechten(c);
    if (wasBlossfechten && blossfechten && !brawl && !crypts) return c;
    if (wasBlossfechten && !blossfechten) {          // leaving Blossfechten: its kit and scenario go (the Brawl and Crypts cards below start from Own gear as well)
        c.scenario = uint8_t(Scenario::Standard);
        c.gear = Gear::Own;
        c.tier = MatchConfig{}.tier;
        c.weapons = 0;
        if (!brawl && !crypts && c.format == f) return c;
    }
    if (crypts) {
        if (IsCrypts(c)) {                           // the other Crypts mode: the run's rows stay
            if (c.coopCrypts && !coopCrypts) c.cryptsTier = MatchConfig{}.cryptsTier;   // Co-op Crypts set the game's tiers: back to the row's default
            c.graveRobbers = graveRobbers; c.coopCrypts = coopCrypts;
            if (coopCrypts) c.cryptsTier = kCryptsTierTop;
            if (graveRobbers) c.cryptsTogether = false;   // Grave Robbers is one shared crypt: no Apart or Together row
            return c;
        }
        c = LeaveTag(c);
        c.graveRobbers = graveRobbers; c.coopCrypts = coopCrypts;
        if (coopCrypts) c.cryptsTier = kCryptsTierTop;
        c.format = kCryptsDefaultBattle;
        c.scoreLimit = DefaultScoreLimit(c.format);
        c.gear = Gear::Crypts;
        c.tier = MatchConfig{}.tier;
        return c;
    }
    if (IsCrypts(c)) {                               // leaving Crypts: the same format's card keeps its settings
        c.gear = Gear::Own;
        if (c.coopCrypts) c.cryptsTier = MatchConfig{}.cryptsTier;
        c.graveRobbers = false; c.cryptsTogether = false; c.coopCrypts = false;
        if (c.format == f && !brawl && !blossfechten) return c;
    }
    if (brawl) {
        if (IsBrawl(c)) return c;
        c = LeaveTag(c);
        c.format = Format::FreeForAll;
        c.scoreLimit = DefaultScoreLimit(c.format);
        c.gear = Gear::Fists;
        c.tier = kBrawlTier;
        return ClearGroundBack(c);
    }
    if (IsBrawl(c)) {                                // leaving Brawl (leaving Tag is SelectFormat's)
        c.gear = Gear::Own;
        c.tier = MatchConfig{}.tier;
        if (c.format == f) { c.scoreLimit = DefaultScoreLimit(f); return c; }
    }
    if (blossfechten) {                              // Last man standing's rounds, swords only: the scenario value and the kit
        c = SelectFormat(c, Format::LastManStanding);
        c.scenario = uint8_t(Scenario::Blossfechten);
        c = BlossfechtenKit(c);
        c.crossbows = 0;                             // the card has no Crossbows row (Sanitize does the same)
        return c;
    }
    return SelectFormat(c, f);
}

// A map card picked. The Crypts map is where the Crypts modes are played, so picking it from any other mode enters Crypts mode
// (Crypt Royale, as the CRYPTS card does), and picking any other map while in a Crypts mode leaves it (the Battle format stays, with
// the player's own gear). Every other pick leaves the config alone.
inline MatchConfig MapPicked(MatchConfig c, bool cryptsMap) {
    if (cryptsMap && !IsCrypts(c)) return SelectMode(c, c.format, false, true);
    if (!cryptsMap && IsCrypts(c)) return SelectMode(c, c.format, false);
    return c;
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

// A mode card's one line (docs/UI_STYLE.md): what sets the mode apart, nothing the name and the rules already say.
inline constexpr const char* kBrawlBlurb = "Bare fists, everyone for themselves.";
inline constexpr const char* kCryptsBlurb = "The whole dungeon. Pick a mode on it.";       // the CRYPTS card: it picks the Crypts map
inline constexpr const char* kCryptRoyaleBlurb = "Loot the Crypts, then fight.";
inline constexpr const char* kGraveRobbersBlurb = "Loot the same crypt, together.";
inline constexpr const char* kCoopCryptsBlurb = "The game's Crypts, together.";
inline constexpr const char* kProgressionBlurb = "The game's Progression, together.";
inline constexpr const char* kBlossfechtenBlurb = "Swords only, no armour.";

inline const char* FormatBlurb(Format f) {
    switch (f) {
        case Format::Duel:
        case Format::LastManStanding: return "Rounds, last one standing.";   // one card (the Duel card is this one)
        case Format::TwoTeams: case Format::ThreeTeams: case Format::CaptureTheFlag: return "Two to four teams.";
        case Format::TeamDuel: return "Teams, in rounds.";
        case Format::Riot: return "One knight against the rest.";
        case Format::Juggernaut: return "Big and heavy against small and light.";
        case Format::Tag: return "Tag.";
        case Format::Infection: return "The fallen join the infected.";
        case Format::CrossbowTDM: return "Teams, a crossbow each.";
        case Format::Tournament: return "A one-on-one bracket.";
        case Format::OneCrossbow: return "Peasants and one crossbow.";
        case Format::Roulette: return "One crossbow, six chambers.";
        case Format::HideSeek: return "Seekers against hiders.";
        case Format::Chaos: return "Random everything, every round.";
        case Format::PlayersVsBots: return "Waves of bots.";
        case Format::Zombies: return "Zombie waves in the Abyss.";
        case Format::PitBridgeDuel:   // no card since 0.9.0 (LobbyConfigOf reads it as Carnage)
        case Format::FreeForAll: break;
    }
    return "Everyone for themselves.";
}

inline const char* MapTag(const MapInfo& m) {
    if (m.crypts) return "Large level \xC2\xB7 the whole dungeon";
    if (m.abyss) return "Large level \xC2\xB7 no spawn points: ring spawn";
    return m.bigArena ? "Large level" : "Arena";
}

inline const char* MapRefusal(Format f) {
    if (f == Format::Zombies) return "Zombies needs the Abyss";
    if (f == Format::Tournament) return "Tournament needs an arena";
    return "Not for this mode";
}

inline const char* GearName(Gear g) {
    if (g == Gear::RandomTier) return "Random";
    if (g == Gear::Mirror) return "Identical";
    if (g == Gear::HostKit) return "Host's kit";
    if (g == Gear::Crypts) return "Crypts loot";
    return "Own kits";
}

inline int TierBudget(int tier) { return kTierBudget[std::clamp(tier, 0, 7)]; }

// The tier row's eight chips, one per tier; the chip of the current tier (a stale value reads as the top one) is on.
inline std::vector<RowChip> TierChips(uint8_t tier) {
    std::vector<RowChip> v;
    for (int i = 0; i < 8; ++i) v.push_back({TierName(i), std::min<int>(tier, 7) == i});
    return v;
}
// A click on tier chip `chip` sets the tier to it (a chip past the last is the top one).
inline MatchConfig WithTier(MatchConfig c, size_t chip) { c.tier = uint8_t(std::min<size_t>(chip, 7)); return c; }
// The Bodies row's value: Own, or the same build for everyone: By tier while the tier chips show (the strength is the tier's),
// Same for all when the tier is hidden. The row is the one setting: it steps between Own and the other.
inline const char* BodiesName(const MatchConfig& c) { return c.ownBodies ? "Own" : TierInUse(c) ? "By tier" : "Same for all"; }

// The Score limit row keeps one label in every mode; the unit is in the value: "10 kills", "3 rounds", "6 points", "5 wins".
inline std::string LimitValue(Format f, int n) {
    if (IsRoulette(f)) return CountOf(n, "point", "points");
    if (IsRoundFormat(f)) return CountOf(n, "round", "rounds");
    return CountOf(n, "kill", "kills");
}
// The Time limit row: "No limit", "10 min", and in Hide and seek "5 min a round" (the clock restarts every round).
inline std::string TimeValue(Format f, uint16_t seconds) {
    if (seconds == 0) return "No limit";
    return std::to_string(seconds / 60) + " min" + (IsHideSeek(f) ? " a round" : "");
}

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

// The package path of the map a match of this lobby starts on; empty for a modded map (the day cycle is the stock arenas' only).
inline std::string MapPathOf(const LobbyLocal& l) {
    if (l.config.format != Format::Zombies)
        for (const LobbyModMap& m : l.modMaps) if (SamePath(m.path, l.mapPick)) return {};
    const size_t i = EffectiveMap(l.config.format, l.config.gear, l.mapPick, IsBlossfechten(l.config));
    return i < MapCount() ? std::string(kMaps[i].path) : std::string();
}
// The players the mode cards and names read: the host's count (a dedicated server's own seat is no player), else the roster's size.
inline int PlayersOf(const LobbyLocal& l) { return l.playerCount >= 0 ? l.playerCount : int(l.players.size()); }

inline bool ValidTeam(int t, Format f, int teams) { return PicksTeams(f) && t >= 0 && t < TeamCount(f, teams); }
inline int EffectiveTeam(const LobbyPlayer& p, Format f, int teams) {
    return ValidTeam(p.hostOverride, f, teams) ? p.hostOverride : ValidTeam(p.team, f, teams) ? p.team : kNoTeam;
}

inline SettingRow Row(const LobbyLocal& l, LobbyRow s) {
    const MatchConfig& c = l.config;
    Format f = c.format;
    bool bots = IsBotFormat(f);
    SettingRow r;
    r.id = s;
    r.enabled = l.host;
    switch (s) {
        case LobbyRow::Players:
            r.label = "Player limit";
            r.value = std::to_string(l.maxPlayers);
            break;
        case LobbyRow::Battle:
            r.label = "Battle";
            r.value = IsDuelCard(f) ? DuelCardName(PlayersOf(l)) : FormatName(f);
            break;
        case LobbyRow::Gather:
            r.label = c.graveRobbers ? "Run time" : "Gather time";
            r.value = std::to_string(c.gatherS / 60) + " min";
            break;
        case LobbyRow::FightMap: {
            const size_t at = EffectiveFightMap(f, l.fightPick);
            r.label = "Fight map";
            r.value = at < MapCount() ? kMaps[at].display : "None";
            r.hint = "Where the Battle is fought.";
            break;
        }
        case LobbyRow::CryptsRun:
            r.label = "Crypts run";
            r.value = c.cryptsTogether ? "Together" : "Apart";
            r.hint = c.cryptsTogether ? "Everyone loots the same crypt." : "Each player loots their own Crypts.";
            break;
        case LobbyRow::CryptsTier:
            r.label = "Dungeon tier";
            r.value = CryptsTierLabel(c.cryptsTier);
            break;
        case LobbyRow::Limit:
            r.label = "Score limit";
            r.value = LimitValue(f, LimitToKey(f, c.scoreLimit));
            break;
        case LobbyRow::Time:
            r.label = "Time limit";
            r.value = TimeValue(f, c.timeLimitS);
            break;
        case LobbyRow::Respawn:
            r.label = "Respawn delay";
            r.value = std::to_string(c.respawnDelayS) + " s";
            break;
        case LobbyRow::Gear:
            r.label = "Gear";
            r.value = GearName(c.gear);
            if (bots) r.hint = "Bots start at the tier picked here, and rise.";
            if (TierInUse(c)) r.tierChips = TierChips(c.tier);
            break;
        case LobbyRow::Tier:                                           // the Crypts card only: the other cards pick the tier under Gear
            r.label = "Starting tier";
            r.value = TierName(c.tier);
            if (bots) r.hint = "Bots start at the tier picked here, and rise.";
            r.tierChips = TierChips(c.tier);
            break;
        case LobbyRow::Weapons:
            r.label = "Weapons";
            r.value = WeaponMaskName(c.weapons);
            r.chips.push_back({"Any", c.weapons == 0});
            for (uint8_t t = 1; t <= kWeaponTypeCount; ++t) r.chips.push_back({WeaponTypeName(t), WeaponMaskHas(c.weapons, t)});
            break;
        case LobbyRow::Teams:
            r.label = "Teams";
            r.value = std::to_string(TeamCount(c.format, c.teams)) + " teams";
            break;
        case LobbyRow::FriendlyFire:
            r.label = "Friendly fire";
            r.value = c.friendlyFire ? "On" : "Off";
            break;
        case LobbyRow::Damage:
            r.label = "Player damage";
            r.value = DamageLabel(c.damagePct, l.damage);
            break;
        case LobbyRow::DeadWeight:
            r.label = "Dead weight";
            r.value = WeightLabel(c.deadWeightPct);
            break;
        case LobbyRow::WeaponWeight:
            r.label = "Weapon weight";
            r.value = WeightLabel(c.weaponWeightPct);
            break;
        case LobbyRow::WaveBreak: {
            const int s = WaveBreakShown(c);
            r.label = "Wave break";
            r.value = s == 120 ? "2 min" : std::to_string(s) + " s";
            r.hint = "Hold G to vote to skip it.";
            break;
        }
        case LobbyRow::TimeOfDay:
            r.label = "Time of day";
            r.value = TimeOfDayName(c.timeOfDay);
            break;
        case LobbyRow::ClearGround:
            r.label = "Clear ground";
            r.value = ClearGroundLabel(c.clearGroundS);
            break;
        case LobbyRow::MyTeam: {
            r.label = "My team";
            r.enabled = true;                                        // everyone picks their own
            const LobbyPlayer* me = nullptr;
            for (const LobbyPlayer& p : l.players) if (p.me) me = &p;
            r.value = TeamLabel(me && ValidTeam(me->team, f, c.teams) ? me->team : kNoTeam);
            r.hint = me && ValidTeam(me->hostOverride, f, c.teams) ? "The host moved you to " + TeamLabel(me->hostOverride) + "."
                   : c.shuffle != TeamShuffle::Off ? std::string("Ignored while Shuffle teams is on.") : std::string();
            break;
        }
        case LobbyRow::TeamKit:
            r.label = "Team colours";
            r.value = c.teamKit == TeamKit::Off ? "Off" : c.teamKit == TeamKit::Tabard ? "Tabard" : "Full kit";
            break;
        case LobbyRow::Crossbows:
            r.label = "Crossbows";
            r.value = CrossbowsName(c.crossbows);
            break;
        case LobbyRow::Fuse:
            r.label = "Fuse time";
            r.value = std::to_string(c.fuseS) + " s";
            if (IsHideSeek(f)) r.label = "Hide time";
            break;
        case LobbyRow::Shuffle:
            r.label = "Shuffle teams";
            r.value = c.shuffle == TeamShuffle::EachRound ? "Each round" : c.shuffle == TeamShuffle::EachMatch ? "Each match" : "Off";
            if (c.shuffle != TeamShuffle::Off) r.hint = "Teams are dealt again at random. Picks are ignored.";
            break;
        case LobbyRow::TaggerSpeed:
            r.label = "Tagger speed";
            r.value = std::to_string(c.taggerSpeedPct) + "%";
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
            break;
        case LobbyRow::ClassPower: {
            const int p = SnapClassPower(c.classPower);
            r.label = "Class power";
            r.value = "+" + std::to_string(p);
            break;
        }
        case LobbyRow::Strip:
            r.label = "Stripping";
            r.value = c.stripOff ? "Off" : "On";
            break;
        case LobbyRow::Bite:
            r.label = "Biting";
            r.value = c.biteOff ? "Off" : "On";
            if (c.biteOff) r.hint = "Players can't bite fighters; eating still works.";
            break;
        case LobbyRow::Bodies:
            r.label = "Bodies";
            r.value = BodiesName(c);
            r.hint = c.ownBodies ? "Everyone keeps their own build." : TierInUse(c) ? "Strength from the tier." : "Everyone gets the same build.";
            break;
        case LobbyRow::KeepKit:
            r.label = "Keep kit";
            r.value = c.keepKit ? "Keep for the bracket" : "New each fight";
            break;
        case LobbyRow::Sync:
            r.label = "Sync";
            r.value = c.hostPhysics ? "Host physics" : "Per player";
            r.hint = c.hostPhysics ? "Every screen shows the same fight; your moves lag by your ping."
                                   : "No delay for you; hits can differ between screens.";
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
    if (IsCoopCrypts(c)) return {};                  // Co-op Crypts: the game's own rules, no clock, the game's own tiers
    if (IsGraveRobbers(c)) return {LobbyRow::Gather, LobbyRow::CryptsTier};   // Run time and Dungeon tier: a shared crypt, no fight after
    if (IsCrypts(c)) {                               // Crypt Royale: the Battle and the Gather time, then the battle format's own rows: the gathered kits are the gear
        MatchConfig own = c; own.gear = Gear::Own;
        std::vector<LobbyRow> v = {LobbyRow::Battle, LobbyRow::Gather};
        if (g_cryptsCoopOffered) v.push_back(LobbyRow::CryptsRun);   // Together: hidden by [mod] crypts_coop=0
        v.push_back(LobbyRow::CryptsTier);
        v.push_back(LobbyRow::FightMap);
        for (LobbyRow r : FormatRows(own)) {
            if (r == LobbyRow::Gear) continue;
            v.push_back(r);
            if (r == LobbyRow::Tier) v.push_back(LobbyRow::Weapons);   // the new character's rolled weapon (the Tier row: its rank)
        }
        return v;
    }
    const bool rolled = GearRolled(c.gear);          // the Weapons row filters generated gear only
    if (IsBotFormat(f)) {                            // endless, back at a wave clear in the run's loadout
        std::vector<LobbyRow> b = {LobbyRow::Time, LobbyRow::Gear, LobbyRow::Tier};
        if (rolled) b.push_back(LobbyRow::Weapons);
        for (LobbyRow r : {LobbyRow::WaveBreak, LobbyRow::FriendlyFire, LobbyRow::Damage, LobbyRow::DeadWeight, LobbyRow::WeaponWeight, LobbyRow::Crossbows, LobbyRow::Sync}) b.push_back(r);
        return b;
    }
    if (IsBlossfechten(c)) return {LobbyRow::Limit, LobbyRow::Time, LobbyRow::Respawn, LobbyRow::Damage, LobbyRow::DeadWeight, LobbyRow::WeaponWeight, LobbyRow::Sync};   // swords only, no armour: the kit is the mode's own, no gear rows
    if (IsBrawl(c)) return {LobbyRow::Limit, LobbyRow::Time, LobbyRow::Respawn, LobbyRow::Damage, LobbyRow::DeadWeight, LobbyRow::WeaponWeight, LobbyRow::ClearGround, LobbyRow::Sync};   // everyone bare at kBrawlTier's strength
    if (IsRoulette(f)) return {LobbyRow::Limit, LobbyRow::Time, LobbyRow::Respawn, LobbyRow::Damage, LobbyRow::DeadWeight, LobbyRow::WeaponWeight, LobbyRow::Sync};   // the same rows as One Crossbow
    if (IsOneCrossbow(f))                            // unarmed peasants and one crossbow: no gear rows, no Crossbows row
        return {LobbyRow::Limit, LobbyRow::Time, LobbyRow::Respawn, LobbyRow::Damage, LobbyRow::DeadWeight, LobbyRow::WeaponWeight, LobbyRow::Sync};
    if (IsTag(f)) {                                  // rounds with a fuse: no clock, no teams
        return {LobbyRow::Limit, LobbyRow::Respawn, LobbyRow::Fuse, LobbyRow::TaggerSpeed, LobbyRow::Damage, LobbyRow::DeadWeight, LobbyRow::WeaponWeight, LobbyRow::Sync};   // bare fists: no gear rows
    }
    if (IsInfection(f)) {                            // two fixed sides: the infection decides the teams, the colours and the crossbows
        std::vector<LobbyRow> t = {LobbyRow::Time, LobbyRow::Respawn, LobbyRow::Gear, LobbyRow::Tier};
        if (rolled) t.push_back(LobbyRow::Weapons);
        for (LobbyRow r : {LobbyRow::Damage, LobbyRow::DeadWeight, LobbyRow::WeaponWeight, LobbyRow::Sync}) t.push_back(r);
        return t;
    }
    if (IsHideSeek(f)) return {LobbyRow::Limit, LobbyRow::Time, LobbyRow::Fuse, LobbyRow::Respawn, LobbyRow::Damage, LobbyRow::DeadWeight, LobbyRow::WeaponWeight, LobbyRow::Sync};   // the Seek and the Hide time; the roles set the gear
    if (IsChaos(f))                                  // the rounds draw the gear, the builds, the damage and the weights: the match's own rules only
        return {LobbyRow::Limit, LobbyRow::Time, LobbyRow::Respawn, LobbyRow::Crossbows, LobbyRow::Sync};
    if (HasRoles(f))                                 // the roles set the gear, tier, build and team: only the match's own rules are left
        return {LobbyRow::Limit, LobbyRow::Time, LobbyRow::Respawn, LobbyRow::FriendlyFire, LobbyRow::Damage, LobbyRow::DeadWeight, LobbyRow::WeaponWeight, LobbyRow::TeamKit, LobbyRow::Crossbows, LobbyRow::Sync};
    std::vector<LobbyRow> v;
    if (f != Format::Tournament) { v.push_back(LobbyRow::Limit); v.push_back(LobbyRow::Time); }   // a bracket: one round a fight, no clock
    for (LobbyRow r : {LobbyRow::Respawn, LobbyRow::Gear}) v.push_back(r);
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
// The format's rows in LobbyRow order, and the Bodies row (#1410) after the gear wherever the players fight in their own
// loadouts. The Gear row carries the tier chips, so the Tier row stays only where there is no Gear row: the Crypts card.
inline std::vector<LobbyRow> SettingsFor(const MatchConfig& c) {
    std::vector<LobbyRow> v = lobby_detail::FormatRows(c);
    if (IsGraveRobbers(c)) { std::stable_sort(v.begin(), v.end()); return v; }   // two rows, no Bodies
    if (!IsCrypts(c)) v.erase(std::remove(v.begin(), v.end(), LobbyRow::Tier), v.end());
    const auto gear = std::find(v.begin(), v.end(), IsCrypts(c) ? LobbyRow::Tier : LobbyRow::Gear);
    if ((c.gear == Gear::Own || c.gear == Gear::Crypts) && gear != v.end()) v.insert(gear + 1, LobbyRow::Bodies);
    std::stable_sort(v.begin(), v.end());
    return v;
}

// Every row the lobby's RULES tab lists, in LobbyRow order: the format's own rows (SettingsFor) and the ones that depend on more
// than the format: the player limit, the map's day cycle, Win by, Props, Stripping, Biting, Keep kit and the class budget's Class
// power. Progression co-op keeps the three combat rows (and the player limit) alone. `mapPath`: lobby_detail::MapPathOf.
inline std::vector<LobbyRow> SceneRows(const LobbyLocal& l, const std::string& mapPath) {
    const MatchConfig& c = l.config;
    std::vector<LobbyRow> v;
    if (l.progression) v = {LobbyRow::Damage, LobbyRow::DeadWeight, LobbyRow::WeaponWeight};   // co-op fights take them too
    else if (IsGraveRobbers(c)) v = SettingsFor(c);                                            // no fight: Run time and Dungeon tier alone
    else {
        v = SettingsFor(c);
        if (MapHasDayTime(mapPath)) v.push_back(LobbyRow::TimeOfDay);                           // only the arenas with a day cycle
        if (WinByApplies(c)) v.push_back(LobbyRow::WinBy);                                      // the round formats but Tag, per player sync
        v.push_back(LobbyRow::Props);                                                           // every lobby fight but Progression
        v.push_back(LobbyRow::Strip);
        v.push_back(LobbyRow::Bite);                                                            // the Biting row, beside Stripping
        if (c.format == Format::Tournament && c.gear == Gear::RandomTier) v.push_back(LobbyRow::KeepKit);   // a bracket with random loadouts
    }
    v.push_back(LobbyRow::Players);                                                             // the lobby's own limit, in every lobby
    if (l.classBudget) v.push_back(LobbyRow::ClassPower);                                       // the Classes preset with a power budget
    std::stable_sort(v.begin(), v.end());
    return v;
}

// Which formats a lobby with `players` players can run (the host's count, a dedicated server's own seat not counted).
// Last Man Standing (the Duel card with two) and Team Duel need somebody to be the last of; Riot and Juggernaut need a side of
// two against the one; Tag, Infection and Crossbow TDM need a second player. `hint` says why not, for the card.
struct ModeAvail { bool ok = true; const char* hint = ""; };
inline ModeAvail ModeAvailable(Format f, int players) {
    if (IsRoulette(f) && players < 2) return {false, "Needs 2 or more players"};
    if (IsHideSeek(f) && players < 3) return {false, "Needs 3 or more players"};
    if ((IsDuelCard(f) || IsChaos(f) || IsOneCrossbow(f) || f == Format::TeamDuel || IsTag(f) || IsInfection(f) || IsCrossbowTdm(f)) && players < 2) return {false, "Needs 2 or more players"};
    if ((HasRoleTeams(f) || f == Format::Tournament) && players < 3) return {false, "Needs 3 or more players"};
    return {};
}
// The same for a lobby's whole config: the co-op half of the Crypts (Grave Robbers, the Crypts run's Together) is playable only with
// on by default; [mod] crypts_coop=0 (docs/PLAN_CRYPTS_COOP.md) turns them off, and then START is refused with a plain notice.
inline ModeAvail ModeAvailable(const MatchConfig& c, int players) {
    if (!g_cryptsCoopOffered) {
        if (IsCoopCrypts(c) || (IsCrypts(c) && c.coopCrypts)) return {false, "Co-op Crypts is turned off (crypts_coop=0)"};
        if (IsGraveRobbers(c)) return {false, "Grave Robbers is turned off (crypts_coop=0)"};
        if (IsCrypts(c) && c.cryptsTogether) return {false, "Crypts run Together is turned off (crypts_coop=0)"};
    }
    return ModeAvailable(c.format, players);
}
// The card's name for `players`: the Duel card reads Duel with two and Last man standing otherwise (hsm_mode_merge.h), the
// others their format's. The card keeps one id, so the players decide only what it says and what the match starts as.
inline std::string ModeCardName(Format f, int players) { return IsDuelCard(f) ? std::string(DuelCardName(players)) : FormatName(f); }

namespace lobby_detail {
// ------------------------------------------------------------------ the dropdown rows
// Rows that step through named values are dropdowns (docs/UI_STYLE.md): the value opens a list of every option. Numbers keep
// - and + and click-to-type, On and Off rows stay toggles, the tier and weapon types stay chips. A row not named here is a
// stepper (another change retires two of them), so test_hsm_dropdown_rows.cpp lists every row of every mode with its kind.
inline RowKind RowKindOf(LobbyRow s) {
    switch (s) {
        case LobbyRow::Battle: case LobbyRow::CryptsRun: case LobbyRow::CryptsTier: case LobbyRow::FightMap: case LobbyRow::WinBy: case LobbyRow::Shuffle:
        case LobbyRow::Gear: case LobbyRow::Bodies: case LobbyRow::Crossbows: case LobbyRow::KeepKit: case LobbyRow::TimeOfDay:
        case LobbyRow::TeamKit: case LobbyRow::Sync: case LobbyRow::MyTeam: return RowKind::Choice;
        case LobbyRow::FriendlyFire: case LobbyRow::Props: case LobbyRow::Strip: return RowKind::Toggle;
        case LobbyRow::Bite: return RowKind::Toggle;
        case LobbyRow::Weapons: case LobbyRow::Tier: return RowKind::Chips;
        default: break;
    }
    return RowKind::Stepper;
}
// The Team colours row's options, in the order StepSetting steps them.
inline constexpr hsm::TeamKit kTeamKitOrder[3] = {hsm::TeamKit::Tabard, hsm::TeamKit::Full, hsm::TeamKit::Off};
// The Fight map row's maps, as kMaps indexes in catalog order: the picker's maps the Battle format fights on (MapAllowsFight).
inline std::vector<size_t> FightMaps(Format f) {
    std::vector<size_t> v;
    for (size_t i = 0; i < MapCount(); ++i) if (kMaps[i].pick && MapAllowsFight(kMaps[i], f)) v.push_back(i);
    return v;
}
// How many options a Choice row has for this config (0: not a Choice row). A two-valued row lists its default (off) state first.
inline size_t ChoiceCount(const MatchConfig& c, LobbyRow s) {
    switch (s) {
        case LobbyRow::Battle: return kCryptsBattleCount;
        case LobbyRow::CryptsRun: case LobbyRow::Bodies: case LobbyRow::KeepKit: case LobbyRow::Sync: return 2;
        case LobbyRow::CryptsTier: return size_t(kCryptsTierTop) + 1;
        case LobbyRow::FightMap: return FightMaps(c.format).size();
        case LobbyRow::WinBy: return kWinByCount;
        case LobbyRow::Shuffle: return ShuffleOptions(c.format).size();
        case LobbyRow::Gear: return kGearRowCount;
        case LobbyRow::Crossbows: return kCrossbowModeCount;
        case LobbyRow::TimeOfDay: return kTimeOfDayPicks;                     // Morning, Midday, Evening, Night
        case LobbyRow::TeamKit: return 3;
        case LobbyRow::MyTeam: return size_t(1 + TeamCount(c.format, c.teams));   // Auto, then the teams
        default: break;
    }
    return 0;
}
// The option the config shows now (the stop StepSetting would step from: a stale value counts as the first).
inline size_t ChoiceAt(const MatchConfig& c, LobbyRow s) {
    switch (s) {
        case LobbyRow::Battle: {
            size_t at = 0;
            for (size_t i = 0; i < kCryptsBattleCount; ++i) if (kCryptsBattles[i] == c.format) at = i;
            return at;
        }
        case LobbyRow::CryptsRun: return c.cryptsTogether ? 1 : 0;
        case LobbyRow::CryptsTier: return std::min<size_t>(c.cryptsTier, kCryptsTierTop);
        case LobbyRow::WinBy: return size_t(EffectiveWinBy(c.format, c.hostPhysics, c.winBy));
        case LobbyRow::Shuffle: return size_t(c.shuffle) < ShuffleOptions(c.format).size() ? size_t(c.shuffle) : 0;
        case LobbyRow::Gear: {
            size_t at = 0;
            for (size_t i = 0; i < kGearRowCount; ++i) if (kGearRow[i] == c.gear) at = i;
            return at;
        }
        case LobbyRow::Bodies: return c.ownBodies ? 1 : 0;
        case LobbyRow::Crossbows: return ValidCrossbows(c.crossbows) ? c.crossbows : 0;
        case LobbyRow::KeepKit: return c.keepKit ? 1 : 0;
        case LobbyRow::TimeOfDay: return TimeOfDayPickAt(c.timeOfDay);          // an older host's Map's own (0) and a stale value read Midday
        case LobbyRow::TeamKit: {
            size_t at = 1;                                        // a value out of range reads Full kit, as StepSetting steps it
            for (size_t i = 0; i < 3; ++i) if (kTeamKitOrder[i] == c.teamKit) at = i;
            return at;
        }
        case LobbyRow::Sync: return c.hostPhysics ? 1 : 0;
        default: break;
    }
    return 0;
}
// The config after option `i` of a Choice row is picked: what StepSetting reaches by stepping to it. The team pick (My team)
// is the player's own, not the config's: MyTeamPick.
inline MatchConfig WithChoice(MatchConfig c, LobbyRow s, size_t i) {
    if (i >= ChoiceCount(c, s)) return c;
    switch (s) {
        case LobbyRow::Battle: return SelectFormat(c, kCryptsBattles[i]);         // the gear stays Crypts
        case LobbyRow::CryptsRun: c.cryptsTogether = i == 1; break;
        case LobbyRow::CryptsTier: c.cryptsTier = uint8_t(i); break;
        case LobbyRow::WinBy: c.winBy = uint8_t(i); break;
        case LobbyRow::Shuffle: c.shuffle = TeamShuffle(i); break;
        case LobbyRow::Gear:
            c.gear = kGearRow[i];
            if (!TierInUse(c)) c.tier = MatchConfig{}.tier;                       // as StepSetting: the chips are gone, a tier picked before must not stay
            break;
        case LobbyRow::Bodies: c.ownBodies = i == 1; break;
        case LobbyRow::Crossbows: c.crossbows = uint8_t(i); break;
        case LobbyRow::KeepKit: c.keepKit = i == 1; break;
        case LobbyRow::TimeOfDay: c.timeOfDay = TimeOfDayOfPick(i); break;
        case LobbyRow::TeamKit: c.teamKit = kTeamKitOrder[i]; break;
        case LobbyRow::Sync: c.hostPhysics = i == 1; break;
        default: break;
    }
    return c;
}
// My team's option `i`: Auto, then team 0, 1, ...
inline int8_t MyTeamPick(size_t i) { return i == 0 ? kNoTeam : int8_t(i - 1); }
// The options of a Choice row in list order. Each label is the row's own value text with that option picked, so the list and
// the value cannot disagree. The Battle row's formats the player count cannot run are disabled with the mode card's reason.
inline std::vector<DropOption> ChoicesFor(const LobbyLocal& l, LobbyRow s) {
    const MatchConfig& c = l.config;
    std::vector<DropOption> v;
    if (s == LobbyRow::FightMap) {
        for (size_t i : FightMaps(c.format)) v.push_back({kMaps[i].display, true, ""});
        return v;
    }
    if (s == LobbyRow::MyTeam) {
        for (size_t i = 0; i < ChoiceCount(c, s); ++i) v.push_back({TeamLabel(MyTeamPick(i)), true, ""});
        return v;
    }
    const int players = PlayersOf(l);
    LobbyLocal probe;                                                             // the config and the player count are all a value text reads
    probe.playerCount = players;                                                  // (the Battle row's Last man standing reads Duel with two)
    for (size_t i = 0; i < ChoiceCount(c, s); ++i) {
        probe.config = WithChoice(c, s, i);
        v.push_back({Row(probe, s).value, true, ""});
        if (s != LobbyRow::Battle) continue;
        const ModeAvail av = ModeAvailable(kCryptsBattles[i], players);
        if (!av.ok) { v.back().enabled = false; v.back().why = av.hint; }
    }
    return v;
}
// The option the lobby shows now for a Choice row (My team: the player's own pick, Auto when it is not valid in this format).
inline size_t ChoiceIndex(const LobbyLocal& l, LobbyRow s) {
    if (s == LobbyRow::FightMap) {                                                // the pick, as the match would take it (a stale one reads the default)
        const std::vector<size_t> maps = FightMaps(l.config.format);
        const size_t at = EffectiveFightMap(l.config.format, l.fightPick);
        for (size_t i = 0; i < maps.size(); ++i) if (maps[i] == at) return i;
        return 0;
    }
    if (s != LobbyRow::MyTeam) return ChoiceAt(l.config, s);
    for (const LobbyPlayer& p : l.players) if (p.me && ValidTeam(p.team, l.config.format, l.config.teams)) return size_t(p.team) + 1;
    return 0;
}
// A row as the scene lists it: the value and hint, how it is changed, and for a Choice row its dropdown.
inline SettingRow SceneRow(const LobbyLocal& l, LobbyRow s) {
    SettingRow r = Row(l, s);
    r.kind = RowKindOf(s);
    if (r.kind == RowKind::Choice) { r.choices = ChoicesFor(l, s); r.choice = ChoiceIndex(l, s); }
    return r;
}

}  // namespace lobby_detail

// The group of the i-th mode card (kModeOrder, then Brawl, Crypts and Progression): known before any scene, so the page can
// build each card in its group's grid.
inline ModeGroup ModeGroupAt(size_t i) {
    if (i < kModeOrderCount) return IsBotFormat(kModeOrder[i].format) ? ModeGroup::Coop : ModeGroup::Versus;
    return i <= kModeOrderCount + 1 ? ModeGroup::Versus : ModeGroup::Coop;   // Brawl, CRYPTS; Progression, Crypt Royale, Grave Robbers, Crypts (co-op)
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
            if (l.host) {                                                       // and where: the region the lobby is listed under
                const Region at = EffectiveRegion(l.regionPick, l.regionGuess);
                s.pill = {std::string(l.publicLobby ? "Hosting \xC2\xB7 public" : "Hosting \xC2\xB7 friends") + (RegionKnown(at) ? std::string(" \xC2\xB7 ") + RegionLabel(at) : std::string()), style::kOk};
                break;
            }
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
        else if (shown.empty()) s.browserEmpty = HiddenByFiltersText(l.found, l.filter);
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
            if (!f.mode.empty()) r.detail += (r.detail.empty() ? "" : " \xC2\xB7 ") + BrowserModeShown(f);
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
    s.balance = l.host && PicksTeams(f);
    s.shuffle = l.host && PicksTeams(f);
    const int players = PlayersOf(l);
    const bool blossfechten = IsBlossfechten(c);
    for (const ModeSlot& slot : kModeOrder) {
        const Format m = slot.format;
        const ModeAvail av = ModeAvailable(m, players);
        ModeCard mc;
        mc.format = m; mc.blossfechten = slot.blossfechten;
        mc.name = Upper(slot.blossfechten ? std::string(kBlossfechtenName) : ModeCardName(m, players));
        mc.description = slot.blossfechten ? kBlossfechtenBlurb : FormatBlurb(m);
        const bool formatCard = IsDuelCard(m) ? IsDuelCard(f) : m == f;                      // a Duel config sits on the Last man standing card
        mc.selected = formatCard && blossfechten == slot.blossfechten && !IsBrawl(c) && !IsCrypts(c) && !l.progression; mc.enabled = l.host && av.ok;
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
    {                                                // CRYPTS: picks the Crypts map (and Crypt Royale on it); on the map the two modes below take its place
        ModeCard k;
        k.format = kCryptsDefaultBattle;
        k.name = "CRYPTS";
        k.description = kCryptsBlurb;
        k.enabled = l.host; k.crypts = true; k.shown = !IsCrypts(c) || l.progression;
        s.modes.push_back(k);
    }
    {
        ModeCard p;
        p.name = "PROGRESSION (CO-OP)";
        p.description = kProgressionBlurb;
        p.selected = l.progression; p.enabled = l.host; p.progression = true;
        s.modes.push_back(p);
    }
    {                                                // the Crypts modes, on the Crypts map
        ModeCard r;
        r.format = IsCrypts(c) ? f : kCryptsDefaultBattle;
        r.name = "CRYPT ROYALE";
        r.description = kCryptRoyaleBlurb;
        r.selected = IsCryptRoyale(c) && !l.progression; r.enabled = l.host; r.crypts = true; r.shown = IsCrypts(c) && !l.progression;
        s.modes.push_back(r);
        ModeCard g;
        g.format = r.format;
        g.name = "GRAVE ROBBERS";
        g.description = kGraveRobbersBlurb;
        g.selected = IsGraveRobbers(c) && !c.coopCrypts && !l.progression; g.enabled = l.host; g.crypts = true; g.graveRobbers = true;
        g.shown = IsCrypts(c) && !l.progression && g_cryptsCoopOffered;   // hidden by [mod] crypts_coop=0
        s.modes.push_back(g);
    }
    {                                                // Co-op Crypts: a mode of its own, on every map (picking it enters the Crypts map)
        ModeCard k;
        k.format = kCryptsDefaultBattle;
        k.name = "CRYPTS (CO-OP)";
        k.description = kCoopCryptsBlurb;
        k.selected = IsCoopCrypts(c) && !l.progression; k.enabled = l.host; k.crypts = true; k.graveRobbers = true; k.coopCrypts = true;
        k.shown = g_cryptsCoopOffered;                // hidden by [mod] crypts_coop=0
        s.modes.push_back(k);
    }
    for (size_t i = 0; i < s.modes.size(); ++i) s.modes[i].group = ModeGroupAt(i);
    if (l.progression) s.rulesNote = "The game's own rules, in the Tavern.";
    const std::string mapPath = lobby_detail::MapPathOf(l);
    for (LobbyRow id : SceneRows(l, mapPath)) s.settings.push_back(SceneRow(l, id));
    if (!l.host) s.hostOnly = "Only the host can change these.";
    size_t customChosen = l.modMaps.size();   // the picked custom map (none: l.modMaps.size())
    if (f != Format::Zombies)
        for (size_t i = 0; i < l.modMaps.size(); ++i) if (lobby_detail::SamePath(l.modMaps[i].path, l.mapPick)) customChosen = i;
    const size_t chosen = customChosen < l.modMaps.size() ? MapCount() : EffectiveMap(f, c.gear, l.mapPick, blossfechten);
    for (size_t i = 0; i < MapCount() && !l.progression; ++i) {
        if (!kMaps[i].pick) continue;
        MapCard m;
        m.index = i;
        m.name = kMaps[i].display;
        m.tag = MapTag(kMaps[i]);
        // The Crypts map is picked from any mode (it enters Crypts mode); while in Crypts mode another map is picked as the Battle format's
        // own (MapPicked leaves Crypts mode).
        m.allowed = kMaps[i].crypts ? true : IsCrypts(c) ? MapAllowsMatch(kMaps[i], f, Gear::Own, false) : MapAllowsMatch(kMaps[i], f, c.gear, blossfechten);
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
        m.allowed = f != Format::Zombies && !blossfechten;   // the Abyss is the only Zombies map; Blossfechten fights in its arenas only
        if (!m.allowed) m.reason = MapRefusal(f);
        m.selected = i == customChosen;
        m.enabled = l.host && m.allowed;
        s.maps.push_back(m);
    }
    const ModeAvail playable = l.progression ? ModeAvail{} : ModeAvailable(c, players);   // Last Man Standing (the Duel card with two) needs 2 or more
    const bool modWait = l.host && customChosen < l.modMaps.size() && l.modPickPending;
    StartInputs gate;                                                                   // one rule with the host's session (match/hsm_start_gate.h)
    gate.host = l.host; gate.progression = l.progression; gate.allowMismatch = l.allowBuildMismatch; gate.modsChecking = modWait;
    gate.modeOk = playable.ok; gate.modeHint = playable.hint; gate.mine = l.myBuild;
    for (const LobbyPlayer& p : l.players) if (!p.me) gate.peers.push_back({p.name, p.build, p.joined});
    const StartVerdict verdict = EvaluateStart(gate);
    s.start = verdict.ok;
    s.startWhy = verdict.text;
    s.buildMismatch = verdict.warn;
    s.waitingWarn = verdict.warn || (l.host && !verdict.ok);
    // The line beside START: what will be played.
    if (l.progression) s.summary = "PROGRESSION \xC2\xB7 the Tavern, together";
    else {
        s.summary = Upper(ModeNameFor(c, players));
        for (const MapCard& m : s.maps) if (m.selected) s.summary += " \xC2\xB7 " + m.name;
        const bool gearRows = !IsBare(c) && !HasRoles(f) && !blossfechten && !IsGraveRobbers(c);   // Riot and Juggernaut deal the gear by role; Brawl and Tag are bare; Blossfechten's kit is the mode's
        if (gearRows) s.summary += " \xC2\xB7 " + std::string(GearName(c.gear));
        if (gearRows && GearRolled(c.gear)) s.summary += ", " + std::string(TierName(c.tier));
        if (gearRows && GearRolled(c.gear) && c.weapons != 0) s.summary += ", " + WeaponMaskName(c.weapons);
        if (IsCoopCrypts(c)) s.summary += ", your own crypt character";
        else if (IsCrypts(c)) s.summary += ", " + std::to_string(c.gatherS / 60) + " min";
        if (IsCrypts(c) && c.cryptsTogether) s.summary += ", together";
        if (IsCryptRoyale(c)) { const size_t fm = EffectiveFightMap(f, l.fightPick); if (fm < MapCount()) s.summary += std::string(", fight on ") + kMaps[fm].display; }
        if (IsCrypts(c) && !IsCoopCrypts(c) && c.cryptsTier != kCryptsTierCap)   // the Dungeon tier when the host moved it off Peasant
            s.summary += c.cryptsTier >= kCryptsTierTop ? std::string(", the game's own dungeon") : ", dungeon up to " + std::string(TierName(c.cryptsTier));
    }
    s.waiting = verdict.line;
    return s;
}

}  // namespace hsm
