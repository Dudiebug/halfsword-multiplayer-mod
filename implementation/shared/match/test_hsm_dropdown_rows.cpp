// Which controls are dropdowns (docs/UI_STYLE.md "Dropdowns"): walks every RULES row of every mode, the lobby browser's filter
// chips, the MODS rows, the SETTINGS choices and the voice rows, and checks each one's kind, its list, and that picking an option
// from the list reaches exactly what the arrows used to step to. Run via implementation/shared/run_tests.sh test_hsm_dropdown_rows.
#include "match/hsm_hud_style.h"
#include "match/hsm_lobby_mods.h"
#include "match/hsm_lobby_nav.h"
#include "match/hsm_lobby_scene.h"
#include "match/hsm_match_impl.h"
#include "match/hsm_mp_scene.h"
#include <algorithm>
#include <cstdio>
#include <set>
#include <string>

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++g_fail; } } while (0)
#define CHECKW(c, what) do { if (!(c)) { std::printf("FAIL %s:%d: %s [%s]\n", __FILE__, __LINE__, #c, (what).c_str()); ++g_fail; } } while (0)

using namespace hsm;

// ------------------------------------------------------------------ the rows: what each one is
// The rule, written out once more here so a row that moves to another kind is a failing test, not a quiet change. A row not in
// the three lists is a stepper: a number, stepped with - and + and typed into.
static RowKind Expected(LobbyRow id) {
    for (LobbyRow r : {LobbyRow::Battle, LobbyRow::CryptsRun, LobbyRow::CryptsTier, LobbyRow::FightMap, LobbyRow::WinBy, LobbyRow::Shuffle, LobbyRow::Gear,
                       LobbyRow::Bodies, LobbyRow::Crossbows, LobbyRow::KeepKit, LobbyRow::TimeOfDay, LobbyRow::TeamKit, LobbyRow::Sync, LobbyRow::MyTeam})
        if (r == id) return RowKind::Choice;
    for (LobbyRow r : {LobbyRow::FriendlyFire, LobbyRow::Props, LobbyRow::Strip, LobbyRow::Bite}) if (r == id) return RowKind::Toggle;
    for (LobbyRow r : {LobbyRow::Weapons, LobbyRow::Tier}) if (r == id) return RowKind::Chips;
    return RowKind::Stepper;
}

static void TestKinds() {
    int choices = 0, toggles = 0, chips = 0, steppers = 0;
    for (int i = 0; i <= int(LobbyRow::Sync); ++i) {
        const LobbyRow id = LobbyRow(i);
        const RowKind k = lobby_detail::RowKindOf(id);
        CHECK(k == Expected(id));
        choices += k == RowKind::Choice; toggles += k == RowKind::Toggle; chips += k == RowKind::Chips; steppers += k == RowKind::Stepper;
    }
    CHECK(choices == 14 && toggles == 4 && chips == 2);
    CHECK(steppers >= 14);                                         // the numbers (and the two rows another change retires)
    for (LobbyRow id : {LobbyRow::Players, LobbyRow::Gather, LobbyRow::Limit, LobbyRow::Time, LobbyRow::Respawn, LobbyRow::Fuse, LobbyRow::TaggerSpeed,
                        LobbyRow::ClassPower, LobbyRow::Damage, LobbyRow::DeadWeight, LobbyRow::WeaponWeight, LobbyRow::Teams, LobbyRow::WaveBreak, LobbyRow::ClearGround})
        CHECK(lobby_detail::RowKindOf(id) == RowKind::Stepper);
    // Only a Choice row has no arrows because of its list; the toggles and numbers keep them.
    SettingRow r; r.id = LobbyRow::Gear; r.kind = RowKind::Choice;
    CHECK(!RowHasArrows(r));
    r.kind = RowKind::Stepper; CHECK(RowHasArrows(r));
    r.kind = RowKind::Toggle; CHECK(RowHasArrows(r));
}

// ------------------------------------------------------------------ the walk
static LobbyPlayer Player(const char* name, bool host, bool me) { LobbyPlayer p; p.name = name; p.host = host; p.me = me; return p; }
static LobbyLocal InLobby(bool host, MatchConfig c, int players) {
    static const char* kNames[8] = {"Ann", "Bob", "Cy", "Dee", "Eli", "Fay", "Gus", "Hal"};
    LobbyLocal l;
    l.phase = LobbyPhase::InLobby; l.host = host; l.version = "0.8.0"; l.myName = host ? "Ann" : "Bob";
    l.config = c;
    l.damage = DamageRange{0, 200, 10, 100};
    for (int i = 0; i < players; ++i) l.players.push_back(Player(kNames[i], i == 0, i == (host ? 0 : 1)));
    l.playerCount = players;
    l.mapPick = "/Game/Maps/Arenas/Map_Arena_Yard";               // an arena with a day cycle: the Time of day row shows
    l.classBudget = true;                                         // and the Class power row
    return l;
}

// The fields a row can change, as one string: two configs with the same text are the same pick.
static std::string Fp(const MatchConfig& c) {
    char b[256];
    std::snprintf(b, sizeof b, "f%d g%d t%d ct%d cT%d w%d s%d b%d x%d k%d d%d tk%d hp%d l%d dm%d", int(c.format), int(c.gear), int(c.tier), int(c.cryptsTogether),
                  int(c.cryptsTier), int(c.winBy), int(c.shuffle), int(c.ownBodies), int(c.crossbows), int(c.keepKit), int(c.timeOfDay), int(c.teamKit),
                  int(c.hostPhysics), int(c.scoreLimit), int(c.damagePct));
    return b;
}

static std::set<LobbyRow> gSeen;                                   // the Choice rows some scene listed

// One scene: every row's kind, and every Choice row's list against the stepper.
static void WalkScene(const LobbyLocal& l, const std::string& what) {
    const LobbyScene s = BuildLobbyScene(l);
    const MatchConfig& c = l.config;
    for (const SettingRow& r : s.settings) {
        CHECKW(r.kind == Expected(r.id), what);
        if (r.kind != RowKind::Choice) { CHECKW(r.choices.empty(), what); continue; }
        gSeen.insert(r.id);
        CHECKW(!RowHasArrows(r), what);
        CHECKW(r.choices.size() >= 2 && r.choices.size() <= kDropdownPool, what);
        CHECKW(r.choices.size() == lobby_detail::ChoiceCount(c, r.id), what);
        CHECKW(r.choice < r.choices.size(), what);
        if (r.choice >= r.choices.size()) continue;
        CHECKW(r.choices[r.choice].label == r.value, what + " " + r.label + ": the list's picked option reads " + r.choices[r.choice].label + ", the row " + r.value);
        std::set<std::string> labels;
        for (const DropOption& o : r.choices) { CHECKW(!o.label.empty(), what); CHECKW(labels.insert(o.label).second, what + " " + r.label + " lists " + o.label + " twice"); }
        if (r.id == LobbyRow::MyTeam) continue;                    // the player's own pick, not the config's (below)
        if (r.id == LobbyRow::FightMap) {                          // the lobby's own pick (hsm_fmap), not a config field: the list, not the config
            const std::vector<size_t> maps = lobby_detail::FightMaps(c.format);
            CHECKW(maps.size() == r.choices.size(), what);
            for (size_t i = 0; i < maps.size() && i < r.choices.size(); ++i) {
                CHECKW(r.choices[i].label == kMaps[maps[i]].display && kMaps[maps[i]].pick && !kMaps[maps[i]].crypts, what);
                LobbyLocal picked = l; picked.fightPick = kMaps[maps[i]].path;
                CHECKW(lobby_detail::ChoiceIndex(picked, r.id) == i, what + " Fight map option " + std::to_string(i));
            }
            LobbyLocal stale = l; stale.fightPick = "nonsense";
            CHECKW(lobby_detail::ChoiceIndex(stale, r.id) == lobby_detail::ChoiceIndex(InLobby(true, c, 2), r.id), what);   // a stale pick reads the default
            continue;
        }

        // Picking option i reaches the stop the arrows reach, and shows as option i.
        for (size_t i = 0; i < r.choices.size(); ++i) {
            const MatchConfig got = lobby_detail::WithChoice(c, r.id, i);
            CHECKW(lobby_detail::ChoiceAt(got, r.id) == i, what + " " + r.label + " option " + std::to_string(i));
        }
        CHECKW(Fp(lobby_detail::WithChoice(c, r.id, r.choices.size())) == Fp(c), what);          // past the list changes nothing
        const size_t n = r.choices.size(), at = r.choice;
        MatchConfig fwd = c;
        for (size_t step = 1; step < n; ++step) {                  // stepping forward visits every option, each the same config as its pick
            fwd = StepSetting(fwd, r.id, 1, l.damage);
            const size_t idx = lobby_detail::ChoiceAt(fwd, r.id);
            CHECKW(idx == (at + step) % n, what + " " + r.label + " forward step " + std::to_string(step));
            CHECKW(Fp(fwd) == Fp(lobby_detail::WithChoice(c, r.id, idx)), what + " " + r.label + " forward to " + std::to_string(idx) + ": " + Fp(fwd) + " vs " + Fp(lobby_detail::WithChoice(c, r.id, idx)));
        }
        const MatchConfig back = StepSetting(c, r.id, -1, l.damage);
        const size_t before = (at + n - 1) % n;
        CHECKW(lobby_detail::ChoiceAt(back, r.id) == before && Fp(back) == Fp(lobby_detail::WithChoice(c, r.id, before)), what + " " + r.label + " back");
    }
}

static void TestWalk() {
    static const Gear kGears[] = {Gear::Own, Gear::RandomTier, Gear::Mirror, Gear::HostKit};
    int scenes = 0;
    for (bool host : {true, false})
        for (bool physics : {false, true})
            for (Gear g : kGears)
                for (const ModeSlot& slot : kModeOrder) {
                    const Format f = slot.format;
                    MatchConfig c = SelectMode(MatchConfig{}, f, false, false, slot.blossfechten);     // the Blossfechten card is a slot of its own
                    if (c.gear == Gear::Own) c.gear = g;           // the bare modes keep their fists
                    c.hostPhysics = physics;
                    if (f == Format::Tournament) c.keepKit = true;
                    for (int players : {2, 4}) {
                        LobbyLocal l = InLobby(host, c, players);
                        if (HasTeams(f)) for (size_t i = 0; i < l.players.size(); ++i) l.players[i].team = int(i % 2);
                        WalkScene(l, std::string(host ? "host " : "member ") + (slot.blossfechten ? "Blossfechten" : FormatName(f)) + " " + std::to_string(int(g)) + (physics ? " host physics " : " per player ") + std::to_string(players));
                        ++scenes;
                    }
                }
    {   // Brawl, and the Crypts card with each battle, apart and together
        LobbyLocal l = InLobby(true, SelectMode(MatchConfig{}, Format::FreeForAll, true), 3);
        WalkScene(l, "Brawl");
        g_cryptsCoopOffered = true;                    // the Crypts run row (Together) and the Grave Robbers card show only with [mod] crypts_coop=1
        for (bool together : {false, true})
            for (Format battle : kCryptsBattles) {
                MatchConfig c = SelectMode(MatchConfig{}, Format::FreeForAll, false, true);
                c = SelectFormat(c, battle);
                c.cryptsTogether = together;
                c.cryptsTier = together ? 1 : 5;
                WalkScene(InLobby(true, c, 4), std::string("Crypts ") + FormatName(battle) + (together ? " together" : " apart"));
                WalkScene(InLobby(false, c, 4), std::string("Crypts member ") + FormatName(battle));
                WalkScene(InLobby(true, c, 2), std::string("Crypts two players ") + FormatName(battle));      // Last man standing reads Duel
                scenes += 3;
            }
        {   // Grave Robbers: Run time and Dungeon tier
            MatchConfig g = SelectMode(MatchConfig{}, Format::PlayersVsBots, false, true, false, true);
            WalkScene(InLobby(true, g, 3), "Grave Robbers");
            WalkScene(InLobby(false, g, 3), "Grave Robbers member");
            scenes += 2;
        }
        g_cryptsCoopOffered = false;
    }
    {   // Tournament with random loadouts: Keep kit; a shuffled team format: Shuffle teams
        MatchConfig t = SelectFormat(MatchConfig{}, Format::Tournament); t.gear = Gear::RandomTier;
        WalkScene(InLobby(true, t, 4), "Tournament random");
        MatchConfig tm = SelectFormat(MatchConfig{}, Format::TeamDuel); tm.shuffle = TeamShuffle::EachRound;
        WalkScene(InLobby(true, tm, 4), "Team duel shuffled");
    }
    CHECK(scenes > 200);
    // Every Choice row shows up in some mode: a row nobody lists would never be walked.
    for (LobbyRow r : {LobbyRow::Battle, LobbyRow::CryptsRun, LobbyRow::CryptsTier, LobbyRow::FightMap, LobbyRow::WinBy, LobbyRow::Shuffle, LobbyRow::Gear,
                       LobbyRow::Bodies, LobbyRow::Crossbows, LobbyRow::KeepKit, LobbyRow::TimeOfDay, LobbyRow::TeamKit, LobbyRow::Sync, LobbyRow::MyTeam})
        CHECKW(gSeen.count(r) == 1, std::string("row ") + lobby_detail::Row(InLobby(true, MatchConfig{}, 2), r).label + " was never in a walked scene");
}

// ------------------------------------------------------------------ the rows, one by one
static std::vector<std::string> Labels(const std::vector<DropOption>& v) { std::vector<std::string> o; for (const DropOption& d : v) o.push_back(d.label); return o; }

static void TestRowLists() {
    const LobbyLocal base = InLobby(true, MatchConfig{}, 4);
    const auto choices = [&](MatchConfig c, LobbyRow id) { LobbyLocal l = base; l.config = c; return Labels(lobby_detail::ChoicesFor(l, id)); };
    using V = std::vector<std::string>;
    CHECK(choices(MatchConfig{}, LobbyRow::Gear) == (V{"Own kits", "Random", "Identical", "Host's kit"}));
    CHECK(choices(MatchConfig{}, LobbyRow::Sync) == (V{"Per player", "Host physics"}));
    CHECK(choices(MatchConfig{}, LobbyRow::TimeOfDay) == (V{"Morning", "Midday", "Evening", "Night"}));
    CHECK(choices(MatchConfig{}, LobbyRow::TeamKit) == (V{"Tabard", "Full kit", "Off"}));
    CHECK(choices(SelectFormat(MatchConfig{}, Format::Duel), LobbyRow::WinBy) == (V{"Death", "First blood", "Knockdown"}));   // a round format, per player sync
    CHECK(choices(MatchConfig{}, LobbyRow::KeepKit) == (V{"New each fight", "Keep for the bracket"}));
    CHECK(choices(MatchConfig{}, LobbyRow::CryptsRun) == (V{"Apart", "Together"}));
    CHECK(choices(MatchConfig{}, LobbyRow::Crossbows).size() == kCrossbowModeCount);
    CHECK(choices(MatchConfig{}, LobbyRow::CryptsTier).size() == 9 && choices(MatchConfig{}, LobbyRow::CryptsTier).back() == "Game's own");
    CHECK(choices(MatchConfig{}, LobbyRow::CryptsTier).size() > kDropdownRows);          // the one RULES list that scrolls

    // Bodies: Own, or the same build for all, which reads By tier while a tier is in use.
    MatchConfig plain = MatchConfig{}; plain.gear = Gear::Own;
    CHECK(choices(plain, LobbyRow::Bodies) == (V{"Same for all", "Own"}));
    MatchConfig rolled = plain; rolled.gear = Gear::RandomTier;
    CHECK(choices(rolled, LobbyRow::Bodies)[0] == "By tier");                            // a rolled gear uses the tier, so the shared build reads By tier
    MatchConfig bots = SelectFormat(MatchConfig{}, Format::PlayersVsBots); bots.gear = Gear::Own;
    CHECK(choices(bots, LobbyRow::Bodies)[0] == "By tier");

    // Shuffle: Each round only where there are rounds to deal for.
    CHECK(choices(SelectFormat(MatchConfig{}, Format::TwoTeams), LobbyRow::Shuffle) == (V{"Off", "Each match"}));
    CHECK(choices(SelectFormat(MatchConfig{}, Format::TeamDuel), LobbyRow::Shuffle) == (V{"Off", "Each match", "Each round"}));

    // My team: Auto, then the teams; the pick shown is the player's own.
    MatchConfig teams = SelectFormat(MatchConfig{}, Format::TwoTeams); teams.teams = 3;
    LobbyLocal l = InLobby(true, teams, 4);
    CHECK(Labels(lobby_detail::ChoicesFor(l, LobbyRow::MyTeam)) == (V{"Auto", "Red", "Blue", "Green"}));
    CHECK(lobby_detail::ChoiceIndex(l, LobbyRow::MyTeam) == 0);
    l.players[0].team = 1;
    CHECK(lobby_detail::ChoiceIndex(l, LobbyRow::MyTeam) == 2);
    l.players[0].team = 3;                                                               // a team this format does not have: Auto
    CHECK(lobby_detail::ChoiceIndex(l, LobbyRow::MyTeam) == 0);
    CHECK(lobby_detail::MyTeamPick(0) == kNoTeam && lobby_detail::MyTeamPick(1) == 0 && lobby_detail::MyTeamPick(3) == 2);
    CHECK(lobby_detail::WithChoice(teams, LobbyRow::MyTeam, 1).teams == 3);              // the team pick is the player's, not the config's
}

static void TestBattleOptions() {
    // The Battle row lists the formats a gather can end in; the ones the player count cannot run are disabled with the mode card's reason.
    // Duel and Last man standing are one entry: it reads Duel with exactly two players, as the row and the mode card do.
    MatchConfig c = SelectMode(MatchConfig{}, Format::FreeForAll, false, true);
    const auto list = [&](int players) { return lobby_detail::ChoicesFor(InLobby(true, c, players), LobbyRow::Battle); };
    const std::vector<DropOption> three = list(3);
    CHECK(three.size() == kCryptsBattleCount);
    for (int players : {1, 2, 3, 4}) {
        const std::vector<DropOption> v = list(players);
        for (size_t i = 0; i < v.size(); ++i) {
            const ModeAvail av = ModeAvailable(kCryptsBattles[i], players);
            CHECK(v[i].enabled == av.ok && v[i].why == std::string(av.hint));
            CHECK(v[i].label == ModeCardName(kCryptsBattles[i], players));
        }
    }
    const auto find = [&](const std::vector<DropOption>& v, Format f) -> const DropOption& { for (size_t i = 0; i < kCryptsBattleCount; ++i) if (kCryptsBattles[i] == f) return v[i]; return v[0]; };
    CHECK(find(list(2), Format::LastManStanding).label == "Duel" && find(list(3), Format::LastManStanding).label == "Last man standing");
    CHECK(find(list(2), Format::LastManStanding).enabled && find(list(2), Format::LastManStanding).why.empty());
    CHECK(!find(list(2), Format::Tournament).enabled && find(list(2), Format::Tournament).why == "Needs 3 or more players");
    CHECK(find(list(3), Format::Tournament).enabled);
    CHECK(!find(list(1), Format::LastManStanding).enabled && find(list(1), Format::LastManStanding).why == "Needs 2 or more players");
    // The player count is the lobby's own count when it has one, else the players listed.
    LobbyLocal counted = InLobby(true, c, 2); counted.playerCount = 4;
    CHECK(lobby_detail::ChoicesFor(counted, LobbyRow::Battle)[0].label == "Last man standing" && lobby_detail::ChoicesFor(counted, LobbyRow::Battle)[4].enabled);
    counted.playerCount = -1;
    CHECK(lobby_detail::ChoicesFor(counted, LobbyRow::Battle)[0].label == "Duel" && !lobby_detail::ChoicesFor(counted, LobbyRow::Battle)[4].enabled);
    counted = InLobby(true, c, 3); counted.playerCount = 0;
    CHECK(!lobby_detail::ChoicesFor(counted, LobbyRow::Battle)[0].enabled);
    // The row's own current pick is the format fought; the gear stays Crypts when another is picked.
    const MatchConfig tournament = lobby_detail::WithChoice(c, LobbyRow::Battle, 4);
    CHECK(tournament.format == Format::Tournament && tournament.gear == Gear::Crypts);
    CHECK(lobby_detail::ChoiceIndex(InLobby(true, tournament, 4), LobbyRow::Battle) == 4);
    const MatchConfig lms = lobby_detail::WithChoice(c, LobbyRow::Battle, 0);
    CHECK(lms.format == Format::LastManStanding && lms.gear == Gear::Crypts);
    CHECK(lobby_detail::ChoiceIndex(InLobby(true, lms, 2), LobbyRow::Battle) == 0);
    // Every other Choice row lists everything enabled.
    for (LobbyRow r : {LobbyRow::Gear, LobbyRow::Sync, LobbyRow::TimeOfDay, LobbyRow::Crossbows})
        for (const DropOption& o : lobby_detail::ChoicesFor(InLobby(true, MatchConfig{}, 2), r)) CHECK(o.enabled && o.why.empty());
}

// ------------------------------------------------------------------ a stored value out of range
// An old INI or a lobby of another build can hold a value the row has no stop for. The list shows the stop the config reads as
// (the first, as StepSetting steps from it), and picking any option still lands.
static void TestStaleValues() {
    using lobby_detail::ChoiceAt;
    using lobby_detail::WithChoice;
    MatchConfig c;
    c.cryptsTier = 200;
    CHECK(ChoiceAt(c, LobbyRow::CryptsTier) == size_t(kCryptsTierTop));                   // above the top is the game's own
    c.cryptsTier = 3; CHECK(ChoiceAt(c, LobbyRow::CryptsTier) == 3);
    c.timeOfDay = 200; CHECK(ChoiceAt(c, LobbyRow::TimeOfDay) == 1);                        // reads Midday, the second pick
    c.timeOfDay = kTimeOfDayCount; CHECK(ChoiceAt(c, LobbyRow::TimeOfDay) == 1);
    c.timeOfDay = 0; CHECK(ChoiceAt(c, LobbyRow::TimeOfDay) == 1);                           // an older host's Map's own reads Midday too
    c.timeOfDay = kTimeOfDayCount - 1; CHECK(ChoiceAt(c, LobbyRow::TimeOfDay) == size_t(kTimeOfDayCount - 2));   // Night, the last pick
    for (size_t i = 0; i < kTimeOfDayPicks; ++i) CHECK(TimeOfDayOfPick(i) == uint8_t(1 + i) && TimeOfDayPickAt(TimeOfDayOfPick(i)) == i);
    CHECK(TimeOfDayOfPick(kTimeOfDayPicks) == uint8_t(TimeOfDay::Midday) && TimeOfDayOfPick(99) == uint8_t(TimeOfDay::Midday));   // a pick past the list: Midday
    c.crossbows = 200; CHECK(ChoiceAt(c, LobbyRow::Crossbows) == 0);
    c.crossbows = kCrossbowModeCount; CHECK(ChoiceAt(c, LobbyRow::Crossbows) == 0);
    c.crossbows = kCrossbowModeCount - 1; CHECK(ChoiceAt(c, LobbyRow::Crossbows) == size_t(kCrossbowModeCount - 1));
    c.teamKit = TeamKit(99); CHECK(ChoiceAt(c, LobbyRow::TeamKit) == 1);                  // reads Full kit
    c.teamKit = TeamKit::Tabard; CHECK(ChoiceAt(c, LobbyRow::TeamKit) == 0);
    c.teamKit = TeamKit::Off; CHECK(ChoiceAt(c, LobbyRow::TeamKit) == 2);

    // Win by is the rule the match runs: the picked one only where it applies, else Death.
    MatchConfig rounds = SelectFormat(MatchConfig{}, Format::LastManStanding);
    rounds.winBy = uint8_t(WinBy::Knockdown);
    CHECK(ChoiceAt(rounds, LobbyRow::WinBy) == size_t(WinBy::Knockdown));
    rounds.hostPhysics = true; CHECK(ChoiceAt(rounds, LobbyRow::WinBy) == 0);             // off under host physics
    rounds.hostPhysics = false; rounds.winBy = 200; CHECK(ChoiceAt(rounds, LobbyRow::WinBy) == 0);
    MatchConfig carnage = SelectFormat(MatchConfig{}, Format::FreeForAll);
    carnage.winBy = uint8_t(WinBy::FirstBlood);
    CHECK(ChoiceAt(carnage, LobbyRow::WinBy) == 0);                                       // not a round format

    // Shuffle: Each round exists only where the format has rounds to deal for.
    MatchConfig teams = SelectFormat(MatchConfig{}, Format::TwoTeams);
    teams.shuffle = TeamShuffle::EachRound; CHECK(ChoiceAt(teams, LobbyRow::Shuffle) == 0);
    teams.shuffle = TeamShuffle::EachMatch; CHECK(ChoiceAt(teams, LobbyRow::Shuffle) == 1);
    MatchConfig duels = SelectFormat(MatchConfig{}, Format::TeamDuel);
    duels.shuffle = TeamShuffle::EachRound; CHECK(ChoiceAt(duels, LobbyRow::Shuffle) == 2);

    // Picking another gear drops a tier that no longer shows; a rolled gear, Crypts and the bot formats keep theirs.
    size_t own = 0, rolled = 0;
    for (size_t i = 0; i < kGearRowCount; ++i) { if (kGearRow[i] == Gear::Own) own = i; if (kGearRow[i] == Gear::RandomTier) rolled = i; }
    MatchConfig gear = SelectFormat(MatchConfig{}, Format::FreeForAll);
    gear.gear = Gear::RandomTier; gear.tier = 7;
    CHECK(WithChoice(gear, LobbyRow::Gear, own).tier == MatchConfig{}.tier && WithChoice(gear, LobbyRow::Gear, own).gear == Gear::Own);
    gear.gear = Gear::Own;
    CHECK(WithChoice(gear, LobbyRow::Gear, rolled).tier == 7 && WithChoice(gear, LobbyRow::Gear, rolled).gear == Gear::RandomTier);
    MatchConfig bots = SelectFormat(MatchConfig{}, Format::PlayersVsBots); bots.tier = 7;
    CHECK(WithChoice(bots, LobbyRow::Gear, own).tier == 7);                               // the bots' tier is in use whatever the gear
}

// ------------------------------------------------------------------ the browser's filters
static void TestBrowserFilters() {
    BrowserFilter f;
    f.mine = Region::NaEast;
    const std::vector<FilterChip> chips = FilterChips(f);
    CHECK(chips[kRegionChip].choice && chips[kModeChip].choice);                          // the region and the mode open lists
    for (size_t i : {kRoomChip, kCompatibleChip, kMostPlayersChip, kPasswordChip}) {      // the other four are toggles, and no mods filter exists
        CHECK(!chips[i].choice && !FilterChipIsChoice(i) && FilterChoices(f, i).empty());
    }
    CHECK(chips.size() == kFilterChips && kFilterChips == 6);
    CHECK(FilterChipIsChoice(kRegionChip) && FilterChipIsChoice(kModeChip));

    // The region leads and starts on Any region; the list is every state the chip stepped through.
    const std::vector<DropOption> regions = FilterChoices(f, kRegionChip);
    CHECK(regions.size() == kRegionFilterStates && regions[0].label == "Any region" && regions[1].label == "My region (NA East)");
    CHECK(FilterChoiceAt(f, kRegionChip) == 0 && chips[kRegionChip].label == "Any region");
    for (size_t i = 0; i < regions.size(); ++i) {
        const BrowserFilter g = WithFilterChoice(f, kRegionChip, i);
        CHECK(FilterChips(g)[kRegionChip].label == regions[i].label && FilterChoiceAt(g, kRegionChip) == i);
        CHECK(g.mode == f.mode && g.hasRoom == f.hasRoom);                                // nothing else moves
    }
    BrowserFilter step = f;                                                              // the same stops the chip stepped through
    for (size_t i = 1; i < regions.size(); ++i) { step = ToggleFilter(step, kRegionChip); CHECK(FilterChips(step)[kRegionChip].label == regions[i].label); }

    const std::vector<DropOption> modes = FilterChoices(f, kModeChip);
    CHECK(modes.size() == BrowserModes().size() && modes[0].label == "Any mode" && modes.size() <= kDropdownPool && modes.size() > kDropdownRows);
    for (size_t i = 0; i < modes.size(); ++i) {
        const BrowserFilter g = WithFilterChoice(f, kModeChip, i);
        CHECK(FilterChips(g)[kModeChip].label == modes[i].label && FilterChoiceAt(g, kModeChip) == i && g.region == f.region);
        CHECK(FilterChips(g)[kModeChip].on == (i != 0));
    }
    CHECK(WithFilterChoice(f, kModeChip, modes.size()).mode == f.mode);                  // past the list changes nothing
    CHECK(WithFilterChoice(f, kRegionChip, regions.size()).region == f.region);
    const BrowserFilter room = WithFilterChoice(f, kRoomChip, 1);                        // a toggle chip has no list: nothing changes
    CHECK(room.region == f.region && room.mode == f.mode && !room.hasRoom);
    // A stale value reads the first option, as the chip's text does; the last option is still the last.
    BrowserFilter stale = f;
    stale.region = 200; stale.mode = 200;
    CHECK(FilterChoiceAt(stale, kRegionChip) == 0 && FilterChoiceAt(stale, kModeChip) == 0);
    CHECK(FilterChips(stale)[kRegionChip].label == "Any region" && FilterChips(stale)[kModeChip].label == "Any mode");
    stale.region = kRegionFilterStates; stale.mode = uint8_t(modes.size());               // the first value past the end
    CHECK(FilterChoiceAt(stale, kRegionChip) == 0 && FilterChoiceAt(stale, kModeChip) == 0);
    stale.region = uint8_t(kRegionFilterStates - 1); stale.mode = uint8_t(modes.size() - 1);
    CHECK(FilterChoiceAt(stale, kRegionChip) == size_t(kRegionFilterStates - 1) && FilterChoiceAt(stale, kModeChip) == modes.size() - 1);
    CHECK(FilterChoiceAt(f, kRoomChip) == 0 && FilterChoiceAt(stale, kPasswordChip) == 0);   // a toggle chip has no list

    // The merged Duel / Last man standing stop is in the mode list, and filtering by it keeps both and nothing else.
    std::vector<std::string> names = Labels(modes);
    const auto stop = std::find(names.begin(), names.end(), std::string(kDuelCardChip));
    CHECK(stop != names.end());
    if (stop != names.end()) {
        const BrowserFilter merged = WithFilterChoice(f, kModeChip, size_t(stop - names.begin()));
        CHECK(FilterChips(merged)[kModeChip].label == kDuelCardChip && names[FilterChoiceAt(merged, kModeChip)] == kDuelCardChip);
        LobbyFound duel, lms, carnage;
        duel.mode = "Duel"; lms.mode = "Last man standing"; carnage.mode = "Carnage";
        CHECK(PassesFilter(duel, merged) && PassesFilter(lms, merged) && !PassesFilter(carnage, merged));
    }
}

// ------------------------------------------------------------------ MODS
static void TestModsRows() {
    const std::vector<DropOption> p = ModPresetChoices();
    CHECK(p.size() == 5 && p.size() == kModPresetChoices);
    CHECK(Labels(p) == (std::vector<std::string>{"Vanilla", "Maps only", "Bundle", "Custom", "Anything"}));
    for (size_t i = 0; i < p.size(); ++i) {
        CHECK(p[i].enabled && p[i].label == ModPresetName(ModPreset(i)));
        ModChoice c; c.blockHse = false; c.officialOnly = true; c.classes = ClassesMode::Unrestricted;
        const ModChoice got = WithModPreset(c, i);
        CHECK(got.preset == ModPreset(i) && !got.blockHse && got.officialOnly && ModPresetChoiceAt(got.preset) == i);   // only the preset moves
        CHECK(got.classes == ClassesMode::Unrestricted);                                     // the Classes row is its own: a preset never moves it
    }
    CHECK(WithModPreset(ModChoice{}, p.size()).preset == ModPreset::Vanilla);
    CHECK(ModPresetChoiceAt(ModPreset(99)) < p.size());                                      // a stale preset still lands in the list
    // The same stops the arrows stepped through.
    ModChoice step;
    for (size_t i = 1; i < p.size(); ++i) { step = StepModRule(step, ModRuleRow::Preset, 1); CHECK(step.preset == WithModPreset(ModChoice{}, i).preset); }
    // The scene: the preset row carries the list, the two switches stay switches.
    LobbyModsLocal l; l.host = true; l.haveList = true; l.rule.preset = ModPreset::Bundle; l.chosen.preset = ModPreset::Bundle;
    const LobbyModsScene s = BuildLobbyMods(l);
    CHECK(s.rules[0].choices.size() == 5 && s.rules[0].choice == size_t(ModPreset::Bundle) && s.rules[0].choices[s.rules[0].choice].label == s.rules[0].value);
    CHECK(s.rules[2].choices.empty() && s.rules[3].choices.empty());                       // Block HS Enhancer and Official builds only stay switches
    CHECK(ModRuleIsChoice(ModRuleRow::Preset) && ModRuleIsChoice(ModRuleRow::Classes) && !ModRuleIsChoice(ModRuleRow::Hse) && !ModRuleIsChoice(ModRuleRow::Official));
}

// The Classes row is its own dropdown: Off, On, Unrestricted, with any Mods allowed preset. Only the Classes setting moves.
static void TestClassesRow() {
    const std::vector<DropOption> c = ClassesModeChoices();
    CHECK(Labels(c) == (std::vector<std::string>{"Off", "On", "Unrestricted"}) && c.size() == kClassesChoices);
    for (size_t i = 0; i < c.size(); ++i) {
        CHECK(c[i].enabled && c[i].label == ClassesModeName(ClassesMode(i)));
        ModChoice now; now.preset = ModPreset::Custom; now.blockHse = false; now.officialOnly = false;
        const ModChoice got = WithClassesMode(now, i);
        CHECK(got.classes == ClassesMode(i) && got.preset == ModPreset::Custom && !got.blockHse && !got.officialOnly && ClassesModeChoiceAt(got.classes) == i);
    }
    CHECK(WithClassesMode(ModChoice{}, c.size()).classes == ClassesMode::Off);              // a pick past the list changes nothing
    CHECK(ClassesModeChoiceAt(ClassesMode(99)) < c.size());                                  // a stale value still lands in the list
    // The arrows stepped through the same stops.
    ModChoice step;
    for (size_t i = 1; i < c.size(); ++i) { step = StepModRule(step, ModRuleRow::Classes, 1); CHECK(step.classes == WithClassesMode(ModChoice{}, i).classes); }
    // Only Unrestricted asks first, and not when it is already on.
    ModChoice now;
    CHECK(!ClassesPickAsksFirst(now, size_t(ClassesMode::Off)) && !ClassesPickAsksFirst(now, size_t(ClassesMode::On)) && ClassesPickAsksFirst(now, size_t(ClassesMode::Unrestricted)));
    now.classes = ClassesMode::On;
    CHECK(ClassesPickAsksFirst(now, size_t(ClassesMode::Unrestricted)));
    now.classes = ClassesMode::Unrestricted;
    CHECK(!ClassesPickAsksFirst(now, size_t(ClassesMode::Unrestricted)) && !ClassesPickAsksFirst(now, size_t(ClassesMode::Off)));
    // The scene: the Classes row (second) carries the list and the picked option, for every preset, on a host and a member.
    for (int host = 0; host < 2; ++host)
        for (uint8_t p = 0; p <= kMaxModPreset; ++p)
            for (uint8_t m = 0; m <= kMaxClassesMode; ++m) {
                LobbyModsLocal l; l.host = host != 0; l.haveList = true; l.hostHasRule = true; l.rulesOn = true;
                l.rule.preset = ModPreset(p); l.rule.classes = ClassesMode(m); l.chosen = ChoiceOf(l.rule);
                const LobbyModsScene s = BuildLobbyMods(l);
                const ModRuleView& r = s.rules[1];
                CHECK(r.id == ModRuleRow::Classes && r.choices.size() == 3 && r.choice == m && r.choices[r.choice].label == r.value);
                CHECK(s.rules[0].choice == p && s.rules[0].choices.size() == 5);              // the preset row is unaffected by the Classes setting
            }
}

// ------------------------------------------------------------------ SETTINGS
static void TestSettings() {
    // MY REGION: Auto with what the guess found, then the six regions, the order the button stepped.
    const std::vector<DropOption> r = MyRegionChoices(Region::Europe);
    CHECK(r.size() == kRegionCount && r[0].label == "Auto (Europe)" && r[1].label == "NA East");
    CHECK(MyRegionChoices(Region::Auto)[0].label == "Auto (unknown)");
    Region step = Region::Auto;
    for (size_t i = 0; i < r.size(); ++i) {
        CHECK(MyRegionPicked(i) == step && MyRegionChoiceAt(step) == i && r[i].label == RegionPickLabel(step, Region::Europe));
        step = NextRegion(step);
    }
    CHECK(MyRegionPicked(r.size()) == Region::Auto);
    CHECK(MyRegionChoiceAt(Region(99)) == 0);                                                  // a stale pick reads Auto
    // MENU LOOK: the order the button stepped, Classic first.
    const std::vector<DropOption> look = style::MenuLookChoices();
    CHECK(Labels(look) == (std::vector<std::string>{"Classic", "Game art", "Dark"}));
    style::MenuLook cur = style::MenuLook::Classic;
    for (size_t i = 0; i < look.size(); ++i) {
        CHECK(style::MenuLookPicked(i) == cur && style::MenuLookChoiceAt(cur) == i && look[i].label == style::MenuLookName(cur));
        cur = style::NextMenuLook(cur);
    }
    CHECK(cur == style::MenuLook::Classic && style::MenuLookPicked(look.size()) == style::MenuLook::Classic);
}

// ------------------------------------------------------------------ voice
static const VoiceRowView* VoiceRowOf(const VoiceScene& s, VoiceRow id) { for (const VoiceRowView& r : s.rows) if (r.id == id) return &r; return nullptr; }

static void TestVoiceRows() {
    MpVoice v;
    v.session = true; v.devices = {"Headset", "Desk microphone"}; v.reach = voice::Reach::Proximity; v.mic = voice::Mic::PushToTalk; v.device = "Desk microphone";
    const VoiceScene s = BuildVoiceScene(v, false);
    for (const VoiceRowView& r : s.rows) {
        CHECK(VoiceRowIsChoice(r.id) == !r.choices.empty());                                                // the toggles, the numbers and the key have no list
    }
    const VoiceRowView* reach = VoiceRowOf(s, VoiceRow::Reach);
    const VoiceRowView* mic = VoiceRowOf(s, VoiceRow::Mic);
    const VoiceRowView* dev = VoiceRowOf(s, VoiceRow::Device);
    CHECK(reach && mic && dev);
    CHECK(Labels(reach->choices) == (std::vector<std::string>{"Off", "Proximity", "Everyone"}) && reach->choices[reach->choice].label == reach->value);
    CHECK(Labels(mic->choices) == (std::vector<std::string>{"Open mic", "Push to talk", "Muted"}) && mic->choices[mic->choice].label == mic->value);
    CHECK(Labels(dev->choices) == (std::vector<std::string>{"Windows default", "Headset", "Desk microphone"}) && dev->choice == 2 && dev->choices[dev->choice].label == dev->value);
    // Picking option i reaches what the arrows stepped to.
    for (VoiceRow row : {VoiceRow::Reach, VoiceRow::Mic, VoiceRow::Device}) {
        const VoiceRowView* r = VoiceRowOf(s, row);
        MpVoice step = v;
        for (size_t k = 1; k < r->choices.size(); ++k) {
            step = StepVoice(step, row, 1);
            const MpVoice pick = WithVoiceChoice(v, row, (r->choice + k) % r->choices.size());
            CHECK(step.reach == pick.reach && step.mic == pick.mic && step.device == pick.device);
        }
    }
    CHECK(WithVoiceChoice(v, VoiceRow::Reach, 3).reach == v.reach && WithVoiceChoice(v, VoiceRow::Device, 9).device == v.device);   // past the list
    CHECK(WithVoiceChoice(v, VoiceRow::Mic, 3).mic == v.mic && WithVoiceChoice(v, VoiceRow::Mic, 200).mic == v.mic);
    for (int row = 0; row < int(VoiceRow::Count); ++row)                                   // exactly three rows open a list
        CHECK(VoiceRowIsChoice(VoiceRow(row)) == (VoiceRow(row) == VoiceRow::Reach || VoiceRow(row) == VoiceRow::Mic || VoiceRow(row) == VoiceRow::Device));
    CHECK(WithVoiceChoice(v, VoiceRow::Level, 0).levelDb == v.levelDb);                   // a row without a list changes nothing
    CHECK(WithVoiceChoice(v, VoiceRow::Device, 0).device.empty());
    // A device that is gone reads Windows default in the list; a server host cannot change its voice chat.
    v.device = "Unplugged";
    CHECK(VoiceRowOf(BuildVoiceScene(v, false), VoiceRow::Device)->choice == 0);
    v.server = true; v.host = true;
    CHECK(!VoiceRowOf(BuildVoiceScene(v, false), VoiceRow::Reach)->enabled);
    // A machine with a great many devices lists the first kDropdownPool.
    MpVoice many; for (int i = 0; i < 40; ++i) many.devices.push_back("Device " + std::to_string(i));
    CHECK(DeviceChoices(many.devices).size() == kDropdownPool);
    CHECK(WithVoiceChoice(many, VoiceRow::Device, kDropdownPool - 1).device == "Device " + std::to_string(kDropdownPool - 2));   // the last one listed
    CHECK(WithVoiceChoice(many, VoiceRow::Device, kDropdownPool).device == many.device);                                        // the first one not listed
}

// ------------------------------------------------------------------ the page a list belongs to
static void TestContextKey() {
    const uint32_t base = PageContextKey(true, PageView::Lobby, true, kRulesTab, LobbySection::Gear, false);
    CHECK(base == PageContextKey(true, PageView::Lobby, true, kRulesTab, LobbySection::Gear, false));
    CHECK(base != PageContextKey(false, PageView::Lobby, true, kRulesTab, LobbySection::Gear, false));      // the page closed
    CHECK(base != PageContextKey(true, PageView::Settings, true, kRulesTab, LobbySection::Gear, false));    // another view
    CHECK(base != PageContextKey(true, PageView::Lobby, false, kRulesTab, LobbySection::Gear, false));      // left the lobby
    CHECK(base != PageContextKey(true, PageView::Lobby, true, kModsTab, LobbySection::Gear, false));        // another tab
    CHECK(base != PageContextKey(true, PageView::Lobby, true, kRulesTab, LobbySection::World, false));      // another RULES sub-tab
    CHECK(base != PageContextKey(true, PageView::Lobby, true, kRulesTab, LobbySection::Gear, true));        // a window over the page
    std::set<uint32_t> keys;
    for (bool open : {false, true}) for (PageView v : {PageView::List, PageView::Lobby, PageView::Settings, PageView::Class}) for (bool in : {false, true})
        for (size_t tab = 0; tab < kTabCount; ++tab) for (size_t sec = 0; sec < kSectionCount; ++sec) for (bool modal : {false, true})
            keys.insert(PageContextKey(open, v, in, tab, LobbySection(sec), modal));
    CHECK(keys.size() == 2 * 4 * 2 * kTabCount * kSectionCount * 2);                      // every combination is its own page
}

int main() {
    TestKinds();
    TestWalk();
    TestRowLists();
    TestBattleOptions();
    TestStaleValues();
    TestBrowserFilters();
    TestModsRows();
    TestClassesRow();
    TestSettings();
    TestVoiceRows();
    TestContextKey();
    if (g_fail) { std::printf("%d failure(s)\n", g_fail); return 1; }
    std::printf("test_hsm_dropdown_rows: ok\n");
    return 0;
}
