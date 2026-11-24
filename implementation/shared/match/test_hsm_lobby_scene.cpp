// Lobby page (MATCH_ROUNDS_UI_CHAT_PLAN 3.4): the state pill and badge, the browser rows, the players, the mode cards,
// which setting rows each format shows with their values and hints, host against member, the steppers wrapping both
// ways, the members' Ready toggle, and the map cards with the selected and the refused ones.
#include "match/hsm_lobby_scene.h"
#include <cmath>
#include <cstdio>

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++g_fail; } } while (0)
using namespace hsm;
using namespace hsm::style;

static bool Near(double a, double b, double eps = 1e-4) { return std::fabs(a - b) <= eps; }
static bool Same(Rgba a, Rgba b) { return Near(a.r, b.r) && Near(a.g, b.g) && Near(a.b, b.b) && Near(a.a, b.a); }

static const std::string kDot = "\xC2\xB7";

static LobbyPlayer Player(const char* name, bool host = false, bool me = false, int team = kNoTeam, int over = kNoTeam) {
    LobbyPlayer p; p.name = name; p.host = host; p.me = me; p.team = team; p.hostOverride = over;
    return p;
}
static size_t PickableCount() { size_t n = 0; for (size_t i = 0; i < MapCount(); ++i) if (kMaps[i].pick) ++n; return n; }

static LobbyLocal InLobby(bool host, Format f = Format::FreeForAll, Gear g = Gear::Own) {
    LobbyLocal l;
    l.phase = LobbyPhase::InLobby; l.host = host; l.version = "0.2.0"; l.myName = host ? "Ann" : "Bob";
    l.config.format = f; l.config.gear = g; l.config.scoreLimit = DefaultScoreLimit(f);
    l.players = {Player("Ann", true, host), Player("Bob", false, !host)};
    return l;
}
// A listed lobby that advertises `mode`, `scenario` is the lobby data hsm_scen (0.9.0: the browser reads both).
static LobbyFound Found(const char* mode, int players = 3, int maxPlayers = 8, uint8_t scenario = 0) {
    LobbyFound f; f.mode = mode; f.players = players; f.maxPlayers = maxPlayers; f.scenario = scenario; return f;
}
static LobbyLocal Teamed(bool host, int teams, Gear g = Gear::Own) { LobbyLocal l = InLobby(host, Format::TwoTeams, g); l.config.teams = uint8_t(teams); return l; }
static std::vector<LobbyRow> Ids(const LobbyScene& s) { std::vector<LobbyRow> v; for (const SettingRow& r : s.settings) v.push_back(r.id); return v; }
static const SettingRow* RowOf(const LobbyScene& s, LobbyRow id) { for (const SettingRow& r : s.settings) if (r.id == id) return &r; return nullptr; }
static const SettingRow& Need(const LobbyScene& s, LobbyRow id) {
    static const SettingRow kNone{};
    const SettingRow* r = RowOf(s, id);
    CHECK(r != nullptr);
    return r ? *r : kNone;
}

static void TestPrevNext() {
    const std::vector<int> o = {3, 5, 10};
    CHECK(NextOf(o, 3) == 5 && NextOf(o, 10) == 3 && NextOf(o, 7) == 3);
    CHECK(PrevOf(o, 5) == 3 && PrevOf(o, 10) == 5);
    CHECK(PrevOf(o, 3) == 10);                           // wraps back to the last
    CHECK(PrevOf(o, 7) == 10);                           // unknown: the last (NextOf: the first)
    CHECK(PrevOf({}, 7) == 7 && NextOf({}, 7) == 7);
    CHECK(StepOf(o, 5, 1) == 10 && StepOf(o, 5, -1) == 3 && StepOf(o, 5, 0) == 10);
}

static void TestNewRows() {
    MatchConfig b; b.format = Format::PlayersVsBots;
    CHECK(WaveBreakShown(b) == 60 && StepSetting(b, LobbyRow::WaveBreak, 1).waveBreakS == 120 && StepSetting(b, LobbyRow::WaveBreak, -1).waveBreakS == 30);
    MatchConfig z; z.format = Format::Zombies;
    CHECK(WaveBreakShown(z) == 10 && StepSetting(z, LobbyRow::WaveBreak, 1).waveBreakS == 30 && StepSetting(z, LobbyRow::WaveBreak, -1).waveBreakS == 120);
    MatchConfig f;   // a stock config has no Clear ground yet (the lobby starts at 2 min): the first step is the first stop, the last a step back
    CHECK(StepSetting(f, LobbyRow::ClearGround, 1).clearGroundS == 30 && StepSetting(f, LobbyRow::ClearGround, -1).clearGroundS == 600);
    // The row: 30 s, 1, 2, 3, 5, 10 min, round again, and no Off.
    f.clearGroundS = kClearGroundDefaultS;
    CHECK(StepSetting(f, LobbyRow::ClearGround, 1).clearGroundS == 180 && StepSetting(f, LobbyRow::ClearGround, -1).clearGroundS == 60);
    {
        MatchConfig w = f; w.clearGroundS = 30;
        const uint16_t order[7] = {30, 60, 120, 180, 300, 600, 30};
        for (int i = 0; i < 7; ++i) { CHECK(w.clearGroundS == order[i]); w = StepSetting(w, LobbyRow::ClearGround, 1); }
        for (int i = 0; i < 7; ++i) { w = StepSetting(w, LobbyRow::ClearGround, -1); CHECK(w.clearGroundS != 0 && ValidClearGroundS(w.clearGroundS)); }
    }
    {   // A format that hides the row leaves Clear ground at 0; coming back to one that shows it starts at Every 2 min, not Never.
        MatchConfig d; d.format = Format::Duel; d.clearGroundS = 0;
        CHECK(SelectFormat(d, Format::FreeForAll).clearGroundS == kClearGroundDefaultS);
        CHECK(SelectFormat(d, Format::TwoTeams).clearGroundS == kClearGroundDefaultS);
        CHECK(SelectFormat(d, Format::PlayersVsBots).clearGroundS == 0 && SelectFormat(d, Format::Zombies).clearGroundS == 0);   // no row there
        CHECK(SelectMode(d, Format::FreeForAll, true).clearGroundS == kClearGroundDefaultS);                                      // the Brawl card too
        d.format = Format::TwoTeams; d.clearGroundS = 300;
        CHECK(SelectFormat(d, Format::FreeForAll).clearGroundS == 300);                                                           // a stop the host picked stays
    }
    f.clearGroundS = 600; CHECK(StepSetting(f, LobbyRow::ClearGround, 1).clearGroundS == 30);
    f.clearGroundS = 30; CHECK(StepSetting(f, LobbyRow::ClearGround, -1).clearGroundS == 600);
    CHECK(ClearGroundLabel(30) == "Every 30 s" && ClearGroundLabel(60) == "Every 1 min" && ClearGroundLabel(120) == "Every 2 min");
    CHECK(ClearGroundLabel(180) == "Every 3 min" && ClearGroundLabel(300) == "Every 5 min" && ClearGroundLabel(600) == "Every 10 min");
    CHECK(ClearGroundLabel(0) == "Never");                                          // an older host's Off: shown, not a stop
    for (uint16_t s : kClearGroundOptionsS) CHECK(ClearGroundLabel(s).find("Off") == std::string::npos && ClearGroundLabel(s) != "Never");
    auto has = [](const MatchConfig& c, LobbyRow r) { for (LobbyRow x : SettingsFor(c)) if (x == r) return true; return false; };
    CHECK(has(b, LobbyRow::WaveBreak) && has(z, LobbyRow::WaveBreak) && !has(b, LobbyRow::ClearGround));
    MatchConfig d; d.format = Format::Duel;
    CHECK(!has(d, LobbyRow::ClearGround) && !has(d, LobbyRow::WaveBreak));
    CHECK(has(MatchConfig{}, LobbyRow::ClearGround));
    MatchConfig br; br.gear = Gear::Fists; CHECK(has(br, LobbyRow::ClearGround) && !has(br, LobbyRow::Tier) && !has(br, LobbyRow::Gear));   // Brawl: fixed strength, no gear
    MatchConfig t; t.format = Format::TwoTeams; CHECK(has(t, LobbyRow::ClearGround));
}

static void TestStepSetting() {
    MatchConfig c;                                       // free-for-all, limit 5
    CHECK(StepSetting(c, LobbyRow::Limit, 1).scoreLimit == 10);
    CHECK(StepSetting(c, LobbyRow::Limit, -1).scoreLimit == 3);
    c.scoreLimit = 50;
    CHECK(StepSetting(c, LobbyRow::Limit, 1).scoreLimit == 3);
    c.scoreLimit = 3;
    CHECK(StepSetting(c, LobbyRow::Limit, -1).scoreLimit == 50);
    CHECK((LimitOptions(Format::TwoTeams) == std::vector<int>{3, 5, 10, 15, 20, 30, 50}));
    MatchConfig d; d.format = Format::Duel; d.scoreLimit = 3;
    CHECK((LimitOptions(Format::Duel) == std::vector<int>{1, 2, 3, 5, 7, 10}));
    CHECK(StepSetting(d, LobbyRow::Limit, 1).scoreLimit == 5 && StepSetting(d, LobbyRow::Limit, -1).scoreLimit == 2);
    d.scoreLimit = 1;
    CHECK(StepSetting(d, LobbyRow::Limit, -1).scoreLimit == 10);
    d.scoreLimit = 10;
    CHECK(StepSetting(d, LobbyRow::Limit, 1).scoreLimit == 1);

    MatchConfig t;
    CHECK(StepSetting(t, LobbyRow::Time, 1).timeLimitS == 300 && StepSetting(t, LobbyRow::Time, -1).timeLimitS == 1200);
    t.timeLimitS = 1200;
    CHECK(StepSetting(t, LobbyRow::Time, 1).timeLimitS == 0 && StepSetting(t, LobbyRow::Time, -1).timeLimitS == 900);

    MatchConfig r;                                       // respawn 5
    CHECK((kRespawnOptions == std::vector<int>{3, 5, 10, 15, 20}));
    CHECK(StepSetting(r, LobbyRow::Respawn, 1).respawnDelayS == 10 && StepSetting(r, LobbyRow::Respawn, -1).respawnDelayS == 3);
    r.respawnDelayS = 3;
    CHECK(StepSetting(r, LobbyRow::Respawn, -1).respawnDelayS == 20);
    r.respawnDelayS = 20;
    CHECK(StepSetting(r, LobbyRow::Respawn, 1).respawnDelayS == 3);

    MatchConfig g;                                       // own gear
    CHECK(StepSetting(g, LobbyRow::Gear, 1).gear == Gear::RandomTier && StepSetting(g, LobbyRow::Gear, -1).gear == Gear::HostKit);
    g.gear = Gear::Mirror;
    CHECK(StepSetting(g, LobbyRow::Gear, 1).gear == Gear::HostKit && StepSetting(g, LobbyRow::Gear, -1).gear == Gear::RandomTier);
    g.gear = Gear::HostKit;                              // the last stop: on to Own, back to Everyone identical
    CHECK(StepSetting(g, LobbyRow::Gear, 1).gear == Gear::Own && StepSetting(g, LobbyRow::Gear, -1).gear == Gear::Mirror);
    g.gear = Gear(7);                                    // out of range from the lobby: steps as if own
    CHECK(StepSetting(g, LobbyRow::Gear, 1).gear == Gear::RandomTier);

    MatchConfig k;                                       // tier 2
    CHECK(StepSetting(k, LobbyRow::Tier, 1).tier == 3 && StepSetting(k, LobbyRow::Tier, -1).tier == 1);
    k.tier = 7;
    CHECK(StepSetting(k, LobbyRow::Tier, 1).tier == 0 && StepSetting(k, LobbyRow::Tier, -1).tier == 6);
    k.tier = 0;
    CHECK(StepSetting(k, LobbyRow::Tier, -1).tier == 7);
    k.tier = 9;
    CHECK(StepSetting(k, LobbyRow::Tier, 1).tier == 0);

    MatchConfig f;
    CHECK(StepSetting(f, LobbyRow::FriendlyFire, 1).friendlyFire && StepSetting(f, LobbyRow::FriendlyFire, -1).friendlyFire);
    f.friendlyFire = true;
    CHECK(!StepSetting(f, LobbyRow::FriendlyFire, 1).friendlyFire);

    MatchConfig m; m.scoreLimit = 7; m.tier = 4; m.gear = Gear::Mirror; m.timeLimitS = 600; m.respawnDelayS = 15;
    MatchConfig same = StepSetting(m, LobbyRow::MyTeam, 1);
    CHECK(same.scoreLimit == 7 && same.tier == 4 && same.gear == Gear::Mirror && same.timeLimitS == 600 && same.respawnDelayS == 15 && !same.friendlyFire);
    MatchConfig only = StepSetting(m, LobbyRow::Tier, 1);  // a step changes its own setting alone
    CHECK(only.tier == 5 && only.scoreLimit == 7 && only.gear == Gear::Mirror && only.timeLimitS == 600 && only.respawnDelayS == 15);
}

static void TestTeamPick() {
    CHECK(StepTeamPick(kNoTeam, 2, 1) == 0 && StepTeamPick(0, 2, 1) == 1 && StepTeamPick(1, 2, 1) == kNoTeam);
    CHECK(StepTeamPick(kNoTeam, 2, -1) == 1 && StepTeamPick(1, 2, -1) == 0 && StepTeamPick(0, 2, -1) == kNoTeam);
    CHECK(StepTeamPick(kNoTeam, 3, -1) == 2 && StepTeamPick(2, 3, -1) == 1 && StepTeamPick(2, 3, 1) == kNoTeam);
    CHECK(StepTeamPick(2, 2, -1) == 1 && StepTeamPick(2, 2, 1) == kNoTeam);   // a pick the format lacks is Auto
    CHECK(StepTeamPick(5, 2, -1) == 1 && StepTeamPick(5, 3, -1) == 2);
    CHECK(StepTeamPick(kNoTeam, 0, -1) == kNoTeam && StepTeamPick(kNoTeam, 0, 1) == kNoTeam);
}

static void TestSelectFormat() {
    MatchConfig c; c.scoreLimit = 20; c.timeLimitS = 600; c.gear = Gear::Mirror; c.tier = 5; c.respawnDelayS = 10; c.friendlyFire = true;
    MatchConfig d = SelectFormat(c, Format::Duel);
    CHECK(d.format == Format::Duel && d.scoreLimit == 3);
    CHECK(d.timeLimitS == 600 && d.gear == Gear::Mirror && d.tier == 5 && d.respawnDelayS == 10 && d.friendlyFire);
    CHECK(SelectFormat(c, Format::TwoTeams).scoreLimit == 10);
    MatchConfig same = SelectFormat(c, Format::FreeForAll);
    CHECK(same.format == Format::FreeForAll && same.scoreLimit == 20);   // the card already chosen: nothing changes
}

static void TestPill() {
    LobbyLocal l; l.version = "0.2.0";
    LobbyScene s = BuildLobbyScene(l);
    CHECK(s.pill.text == "Steam not ready" && Same(s.pill.color, kWarn));
    CHECK(!s.idle && !s.in);
    l.phase = LobbyPhase::Idle;
    s = BuildLobbyScene(l);
    CHECK(s.pill.text == "Not in a lobby" && Same(s.pill.color, kIron) && s.idle && !s.in);
    l.phase = LobbyPhase::Creating;
    s = BuildLobbyScene(l);
    CHECK(s.pill.text == "Creating a lobby" && Same(s.pill.color, kWarn) && !s.idle && !s.in);
    l.phase = LobbyPhase::Joining;
    s = BuildLobbyScene(l);
    CHECK(s.pill.text == "Joining a lobby" && Same(s.pill.color, kWarn) && !s.idle && !s.in);

    LobbyLocal h = InLobby(true);
    s = BuildLobbyScene(h);
    CHECK(s.pill.text == "Hosting " + kDot + " friends" && Same(s.pill.color, kOk) && s.in && !s.idle);
    h.publicLobby = true;
    CHECK(BuildLobbyScene(h).pill.text == "Hosting " + kDot + " public");
    LobbyLocal m = InLobby(false);
    s = BuildLobbyScene(m);
    CHECK(s.pill.text == "In Ann's lobby" && Same(s.pill.color, kOk));
    m.players = {Player("Bob", false, true)};             // the owner not in the list yet
    CHECK(BuildLobbyScene(m).pill.text == "In a lobby");
    m.players = {Player("", true), Player("Bob", false, true)};
    CHECK(BuildLobbyScene(m).pill.text == "In a lobby");
}

static void TestHeader() {
    LobbyLocal l; l.phase = LobbyPhase::Idle; l.version = "0.2.0"; l.myName = "Ann"; l.status = "3 public lobbies found";
    LobbyScene s = BuildLobbyScene(l);
    CHECK(s.me == "Ann");
    CHECK(s.badge == "v0.2.0 " + kDot + " protocol " + std::to_string(int(kProtocolRevision)));
    CHECK(s.status == "3 public lobbies found");
    l.version = "1.4.2";
    CHECK(BuildLobbyScene(l).badge == "v1.4.2 " + kDot + " protocol " + std::to_string(int(kProtocolRevision)));
    l.phase = LobbyPhase::Joining; l.status = "Joining lobby...";
    CHECK(BuildLobbyScene(l).status == "Joining lobby...");
    LobbyLocal in = InLobby(true);
    in.status = "Hosting - invite friends, then start any fight";
    CHECK(BuildLobbyScene(in).status.empty());            // in a lobby the pill says it
}

static void TestBrowser() {
    LobbyLocal l; l.phase = LobbyPhase::Idle; l.version = "0.2.0";
    LobbyFound ok; ok.name = "Ann's fight"; ok.map = "Yard"; ok.mode = "Duel"; ok.players = 2; ok.compatible = true;
    LobbyFound old; old.name = "Old"; old.players = 1; old.compatible = false; old.reason = "different mod version (host 0.1.0, you 0.2.0)";
    LobbyFound bare; bare.players = 3; bare.compatible = false;
    LobbyFound full; full.name = "Full"; full.map = "Pit"; full.players = 8; full.compatible = true;
    LobbyFound modeOnly; modeOnly.name = "M"; modeOnly.mode = "Zombies"; modeOnly.players = 5; modeOnly.compatible = true;
    l.found = {ok, old, bare, full, modeOnly};
    LobbyScene s = BuildLobbyScene(l);
    CHECK(s.browser.size() == 5);
    if (s.browser.size() == 5) {
        CHECK(s.browser[0].name == "Ann's fight" && s.browser[0].players == "2/8" && s.browser[0].detail == "Yard " + kDot + " Duel");
        CHECK(s.browser[0].joinable && s.browser[0].note.text == "Join" && Same(s.browser[0].note.color, kOk));
        CHECK(!s.browser[1].joinable && s.browser[1].note.text == old.reason && Same(s.browser[1].note.color, kWarn));
        CHECK(s.browser[1].detail.empty() && s.browser[1].players == "1/8");
        CHECK(s.browser[2].name == "Unnamed lobby" && s.browser[2].note.text == "Incompatible" && !s.browser[2].joinable);
        CHECK(!s.browser[3].joinable && s.browser[3].note.text == "Full" && Same(s.browser[3].note.color, kWarn) && s.browser[3].detail == "Pit");
        CHECK(s.browser[4].joinable && s.browser[4].detail == "Zombies" && s.browser[4].players == "5/8");
    }
    l.found.assign(8, ok);
    CHECK(BuildLobbyScene(l).browser.size() == kBrowserRows && kBrowserRows == 6);
    for (LobbyFound& f : l.found) f.maxPlayers = 2;          // the limit is each lobby's own (hsm_max), not one global
    s = BuildLobbyScene(l);
    CHECK(!s.browser.empty() && !s.browser[0].joinable && s.browser[0].players == "2/2" && s.browser[0].note.text == "Full");
    l.phase = LobbyPhase::Joining;
    CHECK(BuildLobbyScene(l).browser.empty());
    LobbyLocal in = InLobby(true);
    in.found = {ok};
    CHECK(BuildLobbyScene(in).browser.empty());           // the browser is for finding a lobby
}

// The host's player limit: each listing carries its own, the row reads "3/4", Full and Has room go by it, the title and the
// Players row show the lobby's.
static void TestPlayerLimit() {
    LobbyLocal l; l.phase = LobbyPhase::Idle; l.version = "0.2.0";
    LobbyFound four; four.name = "Four"; four.players = 3; four.maxPlayers = 4; four.compatible = true;
    LobbyFound fourFull = four; fourFull.name = "FourFull"; fourFull.players = 4;
    LobbyFound two; two.name = "Two"; two.players = 2; two.maxPlayers = 2; two.compatible = true;
    LobbyFound legacy; legacy.name = "Legacy"; legacy.players = 5; legacy.compatible = true;      // no hsm_max: 8
    l.found = {four, fourFull, two, legacy};
    LobbyScene s = BuildLobbyScene(l);
    CHECK(s.browser.size() == 4);
    if (s.browser.size() == 4) {
        CHECK(s.browser[0].players == "3/4" && s.browser[0].joinable && s.browser[0].note.text == "Join");
        CHECK(s.browser[1].players == "4/4" && !s.browser[1].joinable && s.browser[1].note.text == "Full");
        CHECK(s.browser[2].players == "2/2" && !s.browser[2].joinable && s.browser[2].note.text == "Full");
        CHECK(s.browser[3].players == "5/8" && s.browser[3].joinable);
    }
    l.maxPlayers = 3;                                                   // the in-lobby limit never leaks into the browser
    CHECK(BuildLobbyScene(l).browser[0].players == "3/4");
    l.maxPlayers = 8;
    // Has room goes by the lobby's own limit.
    CHECK(PassesFilter(four, BrowserFilter{}) && PassesFilter(fourFull, BrowserFilter{}));
    BrowserFilter room; room.hasRoom = true;
    CHECK(PassesFilter(four, room) && !PassesFilter(fourFull, room) && !PassesFilter(two, room) && PassesFilter(legacy, room));
    LobbyFound seven = legacy; seven.players = 7;
    CHECK(PassesFilter(seven, room));
    seven.players = 8;
    CHECK(!PassesFilter(seven, room));
    l.filter = room;
    s = BuildLobbyScene(l);
    CHECK(s.browser.size() == 2 && s.browser[0].players == "3/4" && s.browser[1].players == "5/8" && s.browserCount == "2 of 4 lobbies shown");
    // In the lobby: the title is n/limit and the Players row reads the limit (the host steps it, members read it).
    LobbyLocal in = InLobby(true);
    in.maxPlayers = 4;
    LobbyScene h = BuildLobbyScene(in);
    const SettingRow& limit = Need(h, LobbyRow::Players);   // a row of the RULES tab like the rest, first in the list
    CHECK(h.playersTitle == "PLAYERS 2/4" && h.settings.front().id == LobbyRow::Players);
    CHECK(limit.id == LobbyRow::Players && limit.label == "Player limit" && limit.value == "4" && limit.enabled && limit.hint.empty());
    LobbyLocal mem = InLobby(false);
    mem.maxPlayers = 6;
    LobbyScene m = BuildLobbyScene(mem);
    CHECK(Need(m, LobbyRow::Players).value == "6" && !Need(m, LobbyRow::Players).enabled && m.playersTitle == "PLAYERS 2/6");
    LobbyLocal prog = InLobby(true); prog.progression = true;
    CHECK(RowOf(BuildLobbyScene(prog), LobbyRow::Players) != nullptr);
    LobbyLocal idle; idle.phase = LobbyPhase::Idle;
    CHECK(BuildLobbyScene(idle).settings.empty());
    CHECK(SectionOf(LobbyRow::Players) == LobbySection::Match);
    // StepSetting leaves the config alone for it: the limit is the lobby's (StepPlayerLimit).
    MatchConfig c;
    const MatchConfig stepped = StepSetting(c, LobbyRow::Players, 1);
    CHECK(stepped.scoreLimit == c.scoreLimit && stepped.format == c.format);
}

// A lobby with a modpack: the row names it, and a lobby this machine cannot join says why in its note.
static void TestBrowserPack() {
    LobbyLocal l; l.phase = LobbyPhase::Idle; l.version = "0.2.0";
    LobbyFound have; have.name = "A"; have.map = "Yard"; have.players = 1; have.compatible = true; have.pack = "pack ArenaPack";
    LobbyFound lack; lack.name = "B"; lack.players = 1; lack.compatible = false; lack.reason = "Install the pack"; lack.pack = "pack ArenaPack (not installed)";
    LobbyFound plain; plain.name = "C"; plain.players = 1; plain.compatible = true;
    l.found = {have, lack, plain};
    const LobbyScene s = BuildLobbyScene(l);
    CHECK(s.browser.size() == 3);
    if (s.browser.size() == 3) {
        CHECK(s.browser[0].detail == "Yard " + kDot + " pack ArenaPack" && s.browser[0].joinable);
        CHECK(s.browser[1].detail == "pack ArenaPack (not installed)" && !s.browser[1].joinable && s.browser[1].note.text == "Install the pack");
        CHECK(s.browser[2].detail.empty());
    }
    // The lobby's mod rule follows the pack; a lobby whose rule refuses this machine's mods says why in its note.
    LobbyFound rule; rule.name = "D"; rule.map = "Yard"; rule.players = 1; rule.compatible = true; rule.pack = "pack ArenaPack"; rule.mods = "mods: Vanilla";
    LobbyFound blocked; blocked.name = "E"; blocked.players = 1; blocked.compatible = false; blocked.reason = "Blocks HSEnhancer";
    l.found = {rule, blocked};
    const LobbyScene m = BuildLobbyScene(l);
    CHECK(m.browser.size() == 2);
    if (m.browser.size() == 2) {
        CHECK(m.browser[0].detail == "Yard " + kDot + " pack ArenaPack " + kDot + " mods: Vanilla" && m.browser[0].joinable);
        CHECK(!m.browser[1].joinable && m.browser[1].note.text == "Blocks HSEnhancer");
    }
    // A lobby whose rule refuses mods this machine can turn off: joinable, and the note says a restart comes first.
    LobbyFound sw; sw.name = "F"; sw.players = 1; sw.compatible = false; sw.reason = "Vanilla: remove HalfWolverine"; sw.switchable = true; sw.switchTag = "Restart: 1 mod off";
    LobbyFound swFull = sw; swFull.name = "G"; swFull.players = 8;
    LobbyFound swBare = sw; swBare.name = "H"; swBare.switchTag.clear();
    l.found = {sw, swFull, swBare, blocked};
    const LobbyScene w = BuildLobbyScene(l);
    CHECK(w.browser.size() == 4);
    if (w.browser.size() == 4) {
        CHECK(w.browser[0].joinable && w.browser[0].restart && w.browser[0].note.text == "Restart: 1 mod off");
        CHECK(!w.browser[1].joinable && !w.browser[1].restart && w.browser[1].note.text == "Vanilla: remove HalfWolverine");   // full: the restart would be for nothing
        CHECK(w.browser[2].joinable && w.browser[2].restart && w.browser[2].note.text == "Restart");
        CHECK(!w.browser[3].joinable && !w.browser[3].restart);
    }
    l.filter.compatibleOnly = true;   // the chip keeps a lobby that is a restart away
    const LobbyScene c = BuildLobbyScene(l);
    CHECK(c.browser.size() == 3);
    if (c.browser.size() == 3) CHECK(c.browser[0].name == "F" && c.browser[1].name == "G" && c.browser[2].name == "H");
    l.filter = {};
}

// Locked lobbies: the row says so in plain text and can be hidden with the No password chip.
static void TestPasswordFilter() {
    LobbyLocal l; l.phase = LobbyPhase::Idle; l.version = "0.2.0";
    LobbyFound open; open.name = "Open"; open.players = 1; open.compatible = true;
    LobbyFound lockedL; lockedL.name = "Private"; lockedL.players = 2; lockedL.compatible = true; lockedL.locked = true;
    LobbyFound noName; noName.players = 1; noName.compatible = true; noName.locked = true;
    l.found = {open, lockedL, noName};
    LobbyScene s = BuildLobbyScene(l);
    CHECK(s.browser.size() == 3 && s.filters.size() == 6 && kFilterChips == 6);
    if (s.browser.size() == 3) {
        CHECK(s.browser[0].name == "Open" && !s.browser[0].locked);
        CHECK(s.browser[1].name == std::string(kLockedMarker) + "Private" && s.browser[1].locked && s.browser[1].joinable && s.browser[1].note.text == "Join");
        CHECK(s.browser[2].name == std::string(kLockedMarker) + "Unnamed lobby" && s.browser[2].locked);
        CHECK(s.browser[0].name.find(kLockedMarker) == std::string::npos);
    }
    for (unsigned char ch : std::string(kLockedMarker)) CHECK(ch >= 0x20 && ch < 0x80);   // plain ASCII: no glyph the font may lack
    CHECK(s.filters[kPasswordChip].label == "No password" && !s.filters[kPasswordChip].on);
    BrowserFilter f = ToggleFilter(BrowserFilter{}, kPasswordChip);
    CHECK(f.noPassword && !f.hasRoom && !f.compatibleOnly && !f.mostPlayers && f.mode == 0);
    CHECK(!ToggleFilter(f, kPasswordChip).noPassword);
    CHECK(PassesFilter(open, f) && !PassesFilter(lockedL, f) && PassesFilter(lockedL, BrowserFilter{}));
    l.filter = f;
    s = BuildLobbyScene(l);
    CHECK(s.filters[kPasswordChip].on && s.browser.size() == 1 && s.browser[0].name == "Open" && s.browserCount == "1 of 3 lobbies shown");
    BrowserFilter both = ToggleFilter(f, kRoomChip);                                           // with Has room: both apply
    LobbyFound fullOpen = open; fullOpen.players = 8;
    CHECK(PassesFilter(open, both) && !PassesFilter(fullOpen, both) && !PassesFilter(lockedL, both));
}

// The host's password box: host only, in a lobby. No note sits under it (docs/UI_STYLE.md): the box's own text says what to type.
static void TestHostPassword() {
    LobbyLocal h = InLobby(true);
    LobbyScene s = BuildLobbyScene(h);
    CHECK(s.hostPassword);
    h.locked = true;
    s = BuildLobbyScene(h);
    CHECK(s.hostPassword);
    LobbyLocal m = InLobby(false); m.locked = true;
    s = BuildLobbyScene(m);
    CHECK(!s.hostPassword);                                                // members have no box
    LobbyLocal idle; idle.phase = LobbyPhase::Idle; idle.host = true; idle.locked = true;
    s = BuildLobbyScene(idle);
    CHECK(!s.hostPassword);
}

static void TestOutOfLobby() {
    LobbyLocal l; l.phase = LobbyPhase::Idle; l.version = "0.2.0"; l.players = {Player("Ann", true, true)};
    LobbyScene s = BuildLobbyScene(l);
    CHECK(s.roster.empty() && s.modes.empty() && s.settings.empty() && s.maps.empty());
    CHECK(!s.start && s.waiting.empty() && s.hostOnly.empty() && !s.balance && s.playersTitle.empty());
}

static void TestHostAgainstMember() {
    LobbyScene h = BuildLobbyScene(InLobby(true, Format::TwoTeams, Gear::RandomTier));
    CHECK(h.start && h.waiting.empty() && h.hostOnly.empty() && h.balance);
    for (const ModeCard& c : h.modes) CHECK(c.enabled == ModeAvailable(c.format, 2).ok);   // two players: Riot and Juggernaut need 3
    for (const SettingRow& r : h.settings) CHECK(r.enabled);
    for (const MapCard& m : h.maps) CHECK(m.enabled == m.allowed);
    for (const RosterRow& r : h.roster) CHECK(r.movable);

    LobbyScene m = BuildLobbyScene(InLobby(false, Format::TwoTeams, Gear::RandomTier));
    CHECK(!m.start && m.waiting == "Waiting for the host to start." && m.startWhy == "Only the host starts the match." && m.hostOnly == "Only the host can change these." && !m.balance);
    CHECK(m.modes.size() == kModeCardCount && m.settings.size() == h.settings.size() && m.maps.size() == PickableCount());
    for (const ModeCard& c : m.modes) CHECK(!c.enabled);
    for (const SettingRow& r : m.settings) CHECK(r.enabled == (r.id == LobbyRow::MyTeam));   // their own team only
    for (const MapCard& c : m.maps) CHECK(!c.enabled);
    for (const RosterRow& r : m.roster) CHECK(!r.movable);

    CHECK(!BuildLobbyScene(InLobby(true, Format::FreeForAll)).balance);
    CHECK(!BuildLobbyScene(InLobby(true, Format::PlayersVsBots)).balance);
    CHECK(BuildLobbyScene(InLobby(true, Format::LastManStanding)).balance == false && BuildLobbyScene(Teamed(true, 4)).balance);
}

static void TestRowsPerFormat() {
    using V = std::vector<LobbyRow>;
    using enum LobbyRow;
    CHECK(Ids(BuildLobbyScene(InLobby(true, Format::FreeForAll))) == (V{Players, Limit, Time, Respawn, Damage, DeadWeight, WeaponWeight, Bite, Gear, Bodies, Crossbows, Strip, TimeOfDay, ClearGround, Props, Sync}));   // 0.8.0-pre20: the tier is strength, own gear too; PR4: the tier chips sit under Gear, so no Tier row outside Crypts; every row is here, in the order the sub-tabs show them
    CHECK(Ids(BuildLobbyScene(InLobby(true, Format::FreeForAll, Gear::RandomTier))) == (V{Players, Limit, Time, Respawn, Damage, DeadWeight, WeaponWeight, Bite, Gear, Weapons, Crossbows, Strip, TimeOfDay, ClearGround, Props, Sync}));
    CHECK(Ids(BuildLobbyScene(InLobby(true, Format::FreeForAll, Gear::Mirror))) == (V{Players, Limit, Time, Respawn, Damage, DeadWeight, WeaponWeight, Bite, Gear, Weapons, Crossbows, Strip, TimeOfDay, ClearGround, Props, Sync}));
    CHECK(Ids(BuildLobbyScene(InLobby(true, Format::TwoTeams))) == (V{Players, Limit, Time, Respawn, Damage, DeadWeight, WeaponWeight, Bite, Teams, FriendlyFire, MyTeam, TeamKit, Shuffle, Gear, Bodies, Crossbows, Strip, TimeOfDay, ClearGround, Props, Sync}));
    CHECK(Ids(BuildLobbyScene(Teamed(true, 3, Gear::Mirror))) == (V{Players, Limit, Time, Respawn, Damage, DeadWeight, WeaponWeight, Bite, Teams, FriendlyFire, MyTeam, TeamKit, Shuffle, Gear, Weapons, Crossbows, Strip, TimeOfDay, ClearGround, Props, Sync}));
    CHECK(Ids(BuildLobbyScene(Teamed(false, 4))) == (V{Players, Limit, Time, Respawn, Damage, DeadWeight, WeaponWeight, Bite, Teams, FriendlyFire, MyTeam, TeamKit, Shuffle, Gear, Bodies, Crossbows, Strip, TimeOfDay, ClearGround, Props, Sync}));
    CHECK(Ids(BuildLobbyScene(InLobby(true, Format::LastManStanding))) == (V{Players, Limit, Time, WinBy, Respawn, Damage, DeadWeight, WeaponWeight, Bite, Gear, Bodies, Crossbows, Strip, TimeOfDay, Props, Sync}));   // Duel's rows
    CHECK(Ids(BuildLobbyScene(InLobby(true, Format::LastManStanding, Gear::RandomTier))) == Ids(BuildLobbyScene(InLobby(true, Format::Duel, Gear::RandomTier))));
    CHECK(Ids(BuildLobbyScene(InLobby(true, Format::Duel))) == (V{Players, Limit, Time, WinBy, Respawn, Damage, DeadWeight, WeaponWeight, Bite, Gear, Bodies, Crossbows, Strip, TimeOfDay, Props, Sync}));
    CHECK(Ids(BuildLobbyScene(InLobby(true, Format::Duel, Gear::RandomTier))) == (V{Players, Limit, Time, WinBy, Respawn, Damage, DeadWeight, WeaponWeight, Bite, Gear, Weapons, Crossbows, Strip, TimeOfDay, Props, Sync}));
    CHECK(Ids(BuildLobbyScene(InLobby(true, Format::PlayersVsBots))) == (V{Players, Time, Damage, DeadWeight, WeaponWeight, Bite, FriendlyFire, Gear, Bodies, Crossbows, Strip, TimeOfDay, WaveBreak, Props, Sync}));
    CHECK(Ids(BuildLobbyScene(InLobby(true, Format::Zombies, Gear::RandomTier))) == (V{Players, Time, Damage, DeadWeight, WeaponWeight, Bite, FriendlyFire, Gear, Weapons, Crossbows, Strip, WaveBreak, Props, Sync}));   // 0.8.0-pre20: the bot formats take a gear setting; Zombies' arena has no day cycle and no scenarios
    CHECK(Ids(BuildLobbyScene(InLobby(true, Format::PlayersVsBots, Gear::Mirror))) == (V{Players, Time, Damage, DeadWeight, WeaponWeight, Bite, FriendlyFire, Gear, Weapons, Crossbows, Strip, TimeOfDay, WaveBreak, Props, Sync}));
    // Wave 2 integration: the team rounds and the role formats take every row that fits them. Team Duel is Duel's rows with the team rows; Riot and
    // Juggernaut deal gear, tier, build and team by role, so only the match rules, Free camera, Team colours and Crossbows remain.
    CHECK(Ids(BuildLobbyScene(InLobby(true, Format::TeamDuel))) == (V{Players, Limit, Time, WinBy, Respawn, Damage, DeadWeight, WeaponWeight, Bite, Teams, FriendlyFire, MyTeam, TeamKit, Shuffle, Gear, Bodies, Crossbows, Strip, TimeOfDay, Props, Sync}));
    CHECK(Ids(BuildLobbyScene(InLobby(true, Format::Riot))) == (V{Players, Limit, Time, WinBy, Respawn, Damage, DeadWeight, WeaponWeight, Bite, FriendlyFire, TeamKit, Crossbows, Strip, TimeOfDay, Props, Sync}));
    CHECK(Ids(BuildLobbyScene(InLobby(true, Format::Juggernaut))) == (V{Players, Limit, Time, WinBy, Respawn, Damage, DeadWeight, WeaponWeight, Bite, FriendlyFire, TeamKit, Crossbows, Strip, TimeOfDay, Props, Sync}));
    for (Format f : {Format::TwoTeams, Format::TeamDuel, Format::Riot, Format::Juggernaut}) { MatchConfig c; c.format = f; CHECK(HasTeams(f)); bool k = false; for (LobbyRow r : SettingsFor(c)) k = k || r == TeamKit; CHECK(k); }
    for (Format f : {Format::FreeForAll, Format::Duel, Format::LastManStanding, Format::PlayersVsBots, Format::Zombies}) { MatchConfig c; c.format = f; for (LobbyRow r : SettingsFor(c)) CHECK(r != TeamKit); }   // no teams (the bot formats always wear the full kit)
    { MatchConfig br; br.gear = Gear::Fists; for (LobbyRow r : SettingsFor(br)) CHECK(r != Crossbows && r != TeamKit); CHECK(SectionOf(TeamKit) == LobbySection::Teams); }
}

// The Weapons row (0.8.0-pre23): only where gear is generated; steps through the five filters; the host's alone.
static void TestWeaponsRow() {
    auto has = [](const MatchConfig& c, LobbyRow r) { for (LobbyRow x : SettingsFor(c)) if (x == r) return true; return false; };
    for (Format f : {Format::FreeForAll, Format::Duel, Format::TwoTeams, Format::ThreeTeams, Format::PlayersVsBots, Format::Zombies}) {
        MatchConfig c; c.format = f;
        c.gear = Gear::Own; CHECK(!has(c, LobbyRow::Weapons));
        c.gear = Gear::RandomTier; CHECK(has(c, LobbyRow::Weapons));
        c.gear = Gear::Mirror; CHECK(has(c, LobbyRow::Weapons));
    }
    MatchConfig br; br.gear = Gear::Fists; CHECK(!has(br, LobbyRow::Weapons));
    CHECK(SectionOf(LobbyRow::Weapons) == LobbySection::Gear);
    // The row is chips the host ticks: the Any chip first, then the seven game types; a step by the arrows changes nothing.
    MatchConfig c; c.gear = Gear::RandomTier;
    CHECK(StepSetting(c, LobbyRow::Weapons, 1).weapons == 0 && StepSetting(c, LobbyRow::Weapons, -1).weapons == 0);
    c.weapons = WeaponBit(2);
    CHECK(StepSetting(c, LobbyRow::Weapons, 1).weapons == WeaponBit(2) && StepSetting(c, LobbyRow::Weapons, -1).weapons == WeaponBit(2));
    LobbyLocal l = InLobby(true, Format::FreeForAll, Gear::RandomTier);
    {
        const LobbyScene anyScene = BuildLobbyScene(l);
        const SettingRow& any = Need(anyScene, LobbyRow::Weapons);
        CHECK(any.value == "Any" && any.chips.size() == 8 && any.chips[0].label == "Any" && any.chips[0].on);
        for (size_t i = 1; i < any.chips.size(); ++i) CHECK(!any.chips[i].on && any.chips[i].label == WeaponTypeName(uint8_t(i)));
    }
    l.config.weapons = WeaponBit(7);
    const LobbyScene sc = BuildLobbyScene(l);
    const SettingRow& r = Need(sc, LobbyRow::Weapons);
    CHECK(r.label == "Weapons" && r.value == "Polearms" && r.enabled && r.hint.empty());
    CHECK(r.chips.size() == 8 && !r.chips[0].on && r.chips[7].on && r.chips[7].label == "Polearms" && !r.chips[1].on);
    CHECK(BuildLobbyScene(l).summary.find(", Polearms") != std::string::npos);
    l.config.weapons = WeaponMask(WeaponBit(1) | WeaponBit(2) | WeaponBit(5));
    {
        const LobbyScene threeScene = BuildLobbyScene(l);
        const SettingRow& three = Need(threeScene, LobbyRow::Weapons);
        CHECK(three.value == "1H swords / 2H swords / 1H axes" && !three.chips[0].on && three.chips[1].on && three.chips[2].on && !three.chips[3].on && three.chips[5].on);
        CHECK(BuildLobbyScene(l).summary.find(", 1H swords / 2H swords / 1H axes") != std::string::npos);
        // A click on chip i flips game type i; the Any chip clears all.
        MatchConfig k = l.config;
        k.weapons = ToggleWeaponType(k.weapons, 2); CHECK(k.weapons == WeaponMask(WeaponBit(1) | WeaponBit(5)));
        k.weapons = ToggleWeaponType(k.weapons, 7); CHECK(k.weapons == WeaponMask(WeaponBit(1) | WeaponBit(5) | WeaponBit(7)));
        k.weapons = ToggleWeaponType(k.weapons, 0); CHECK(k.weapons == 0);
    }
    l.config.weapons = 0;
    CHECK(Need(BuildLobbyScene(l), LobbyRow::Weapons).value == "Any" && BuildLobbyScene(l).summary.find("Any") == std::string::npos);
    l.host = false; l.players = {Player("Ann", true, false), Player("Bob", false, true)};
    CHECK(!Need(BuildLobbyScene(l), LobbyRow::Weapons).enabled);
    CHECK(Need(BuildLobbyScene(l), LobbyRow::Weapons).chips.size() == 8);               // members read the chips too
}

// The Crossbows row (rev 36): in every format but Brawl, steps Off / 1 / 2 / Half / All, the host's alone, marked experimental.
static void TestCrossbowsRow() {
    auto has = [](const MatchConfig& c, LobbyRow r) { for (LobbyRow x : SettingsFor(c)) if (x == r) return true; return false; };
    for (Format f : {Format::FreeForAll, Format::Duel, Format::LastManStanding, Format::TwoTeams, Format::TeamDuel, Format::Riot, Format::Juggernaut, Format::PlayersVsBots, Format::Zombies}) {
        MatchConfig c; c.format = f; CHECK(has(c, LobbyRow::Crossbows));
    }
    MatchConfig brawl; brawl.gear = Gear::Fists; CHECK(!has(brawl, LobbyRow::Crossbows));
    CHECK(SectionOf(LobbyRow::Crossbows) == LobbySection::Gear);
    MatchConfig c;
    CHECK(c.crossbows == 0);                                                          // Off by default
    const uint8_t fwd[] = {1, 2, 3, 4, 0};
    for (uint8_t v : fwd) { c = StepSetting(c, LobbyRow::Crossbows, 1); CHECK(c.crossbows == v); }
    CHECK(StepSetting(c, LobbyRow::Crossbows, -1).crossbows == 4);                    // Off steps back to All
    MatchConfig bad; bad.crossbows = 77; CHECK(StepSetting(bad, LobbyRow::Crossbows, 1).crossbows == 1);   // out of range steps from Off
    MatchConfig o = StepSetting(c, LobbyRow::Crossbows, 1);
    CHECK(o.gear == c.gear && o.tier == c.tier && o.format == c.format && o.weapons == c.weapons);   // a step changes its own setting alone
    CHECK(Sanitize(bad).crossbows == 0 && Sanitize(o).crossbows == 1);
    MatchConfig fists; fists.gear = Gear::Fists; fists.crossbows = 2; CHECK(Sanitize(fists).crossbows == 0);   // bare fists: no crossbow
    LobbyLocal l = InLobby(true, Format::FreeForAll);
    l.config.crossbows = 3;
    const LobbyScene sc = BuildLobbyScene(l);
    const SettingRow& r = Need(sc, LobbyRow::Crossbows);
    CHECK(r.label == "Crossbows" && r.value == "Half" && r.enabled && r.hint.empty());
    l.config.crossbows = 0; CHECK(Need(BuildLobbyScene(l), LobbyRow::Crossbows).value == "Off");
    l.host = false; l.players = {Player("Ann", true, false), Player("Bob", false, true)};
    CHECK(!Need(BuildLobbyScene(l), LobbyRow::Crossbows).enabled);
}

static void TestLabelsAndValues() {
    LobbyLocal l = InLobby(true, Format::FreeForAll, Gear::RandomTier);
    l.config.scoreLimit = 15; l.config.tier = 3; l.config.respawnDelayS = 10;
    LobbyScene s = BuildLobbyScene(l);
    CHECK(Need(s, LobbyRow::Limit).label == "Score limit" && Need(s, LobbyRow::Limit).value == "15 kills");
    CHECK(Need(s, LobbyRow::Time).label == "Time limit" && Need(s, LobbyRow::Time).value == "No limit");
    CHECK(Need(s, LobbyRow::Respawn).label == "Respawn delay" && Need(s, LobbyRow::Respawn).value == "10 s");
    CHECK(Need(s, LobbyRow::Gear).label == "Gear" && Need(s, LobbyRow::Gear).value == "Random");
    CHECK(RowOf(s, LobbyRow::Tier) == nullptr && Need(s, LobbyRow::Gear).tierChips.size() == 8 && Need(s, LobbyRow::Gear).tierChips[3].on && Need(s, LobbyRow::Gear).tierChips[3].label == "Militia");   // the tier is the chips under Gear
    l.config.timeLimitS = 600; l.config.gear = Gear::Mirror; l.config.tier = 7;
    s = BuildLobbyScene(l);
    CHECK(Need(s, LobbyRow::Time).value == "10 min" && Need(s, LobbyRow::Gear).value == "Identical");
    CHECK(Need(s, LobbyRow::Gear).tierChips.size() == 8 && Need(s, LobbyRow::Gear).tierChips[7].on && Need(s, LobbyRow::Gear).tierChips[7].label == "Knight");
    l.config.gear = Gear::Own;
    CHECK(Need(BuildLobbyScene(l), LobbyRow::Gear).value == "Own kits");
    CHECK(Need(BuildLobbyScene(l), LobbyRow::Gear).tierChips.empty());                      // own gear: no tier in use, no chips
    // 0.9.0: the sync row, per player (owner sync) by default; a step flips it either way.
    CHECK(Need(BuildLobbyScene(l), LobbyRow::Sync).label == "Sync" && Need(BuildLobbyScene(l), LobbyRow::Sync).value == "Per player");
    CHECK(StepSetting(l.config, LobbyRow::Sync, 1).hostPhysics && StepSetting(l.config, LobbyRow::Sync, -1).hostPhysics);
    l.config.hostPhysics = true;
    CHECK(Need(BuildLobbyScene(l), LobbyRow::Sync).value == "Host physics" && !StepSetting(l.config, LobbyRow::Sync, 1).hostPhysics);
    CHECK(Need(BuildLobbyScene(l), LobbyRow::Sync).hint == "Every screen shows the same fight; your moves lag by your ping.");
    l.config.hostPhysics = false;
    CHECK(Need(BuildLobbyScene(l), LobbyRow::Sync).hint == "No delay for you; hits can differ between screens.");
    l.config.hostPhysics = true;
    l.config.hostPhysics = false;
    // The free camera has no row since 0.9.0 (always on): the config keeps the field, on by default, and no row of any format carries it.
    CHECK(MatchConfig{}.freeCam && Sanitize(MatchConfig{}).freeCam);
    {
        MatchConfig off; off.freeCam = false;
        CHECK(Sanitize(off).freeCam);                                                           // an old config that had it off is on
        for (int fmt = 0; fmt < kFormatCount; ++fmt) { LobbyLocal fl = InLobby(true, Format(fmt)); for (const SettingRow& r : BuildLobbyScene(fl).settings) CHECK(r.label != "Free camera" && r.label != "Scenario"); }
        for (const SettingRow& r : BuildLobbyScene(l).settings) CHECK(!StepSetting(l.config, r.id, 1).freeCam == false);   // no row turns it off
    }

    LobbyLocal d = InLobby(true, Format::Duel);
    d.config.respawnDelayS = 3;
    s = BuildLobbyScene(d);
    CHECK(Need(s, LobbyRow::Limit).label == "Score limit" && Need(s, LobbyRow::Limit).value == "3 rounds");   // one label; the unit is in the value
    CHECK(Need(s, LobbyRow::Respawn).label == "Respawn delay" && Need(s, LobbyRow::Respawn).value == "3 s");

    LobbyLocal b = InLobby(true, Format::PlayersVsBots);
    b.config.tier = 4;
    s = BuildLobbyScene(b);
    CHECK(RowOf(s, LobbyRow::Tier) == nullptr && Need(s, LobbyRow::Gear).tierChips.size() == 8 && Need(s, LobbyRow::Gear).tierChips[4].on && Need(s, LobbyRow::Gear).tierChips[4].label == "Soldier");   // the bots start at it
    CHECK(Need(s, LobbyRow::FriendlyFire).label == "Friendly fire" && Need(s, LobbyRow::FriendlyFire).value == "Off");
    b.config.friendlyFire = true;
    CHECK(Need(BuildLobbyScene(b), LobbyRow::FriendlyFire).value == "On");

    CHECK(TierBudget(0) == 0 && TierBudget(1) == 15 && TierBudget(2) == 50 && TierBudget(3) == 100);
    CHECK(TierBudget(4) == 250 && TierBudget(5) == 444 && TierBudget(6) == 666 && TierBudget(7) == 888);
    CHECK(TierBudget(-1) == 0 && TierBudget(9) == 888);
}

static const std::string kBotsHint = "Bots start at the tier picked here, and rise.";
static std::string GearHint(Format f, Gear g) { return Need(BuildLobbyScene(InLobby(true, f, g)), LobbyRow::Gear).hint; }

// Host's loadout (#777): its own name and hint; no budget beside the tier and no Weapons row (nothing is rolled); the
// summary names it without a tier.
static void TestHostKitRows() {
    for (Format f : {Format::FreeForAll, Format::Duel, Format::TwoTeams, Format::PlayersVsBots}) {
        CHECK(GearHint(f, Gear::HostKit) == (IsBotFormat(f) ? kBotsHint : std::string()));   // the value says it; the bots' tier hint rides on Gear
        const LobbyScene s = BuildLobbyScene(InLobby(true, f, Gear::HostKit));
        CHECK(Need(s, LobbyRow::Gear).value == "Host's kit");
        CHECK(RowOf(s, LobbyRow::Tier) == nullptr && Need(s, LobbyRow::Gear).value.find("budget") == std::string::npos && Need(s, LobbyRow::Gear).hint.find("random gear") == std::string::npos);
        CHECK(Need(s, LobbyRow::Gear).tierChips.empty() == !IsBotFormat(f));              // the host's kit has no tier; the bots still start at one
        bool weapons = false;
        for (const SettingRow& r : s.settings) weapons = weapons || r.id == LobbyRow::Weapons;
        CHECK(!weapons);
        CHECK(s.summary.find("Host's kit") != std::string::npos && s.summary.find("Host's kit,") == std::string::npos);
        LobbyLocal ticked = InLobby(true, f, Gear::HostKit); ticked.config.weapons = WeaponBit(1);
        CHECK(BuildLobbyScene(ticked).summary == s.summary);                           // a ticked weapon type is for rolled gear only
        const LobbyScene rolled = BuildLobbyScene(InLobby(true, f, Gear::Mirror));
        CHECK(Need(rolled, LobbyRow::Gear).value == "Identical" && Need(rolled, LobbyRow::Gear).tierChips.size() == 8 && rolled.summary.find("Identical, ") != std::string::npos);
        const LobbyScene own = BuildLobbyScene(InLobby(true, f, Gear::Own));
        CHECK(Need(own, LobbyRow::Gear).tierChips.empty() == !IsBotFormat(f) && own.summary.find("Own kits,") == std::string::npos);
        CHECK(Need(own, LobbyRow::Gear).hint == Need(s, LobbyRow::Gear).hint);        // the tier is the strength only, as with own gear
    }
    CHECK(std::string(GearName(Gear::HostKit)) == "Host's kit" && std::string(GearName(Gear::Fists)) == "Own kits");
}

// The Bodies row (#1410): with own loadouts, after the Gear tier: every player built for the tier (as before) or as their
// own character is. Not with rolled, dealt or the host's gear, which come with the tier's build.
static void TestBodiesRow() {
    auto has = [](const MatchConfig& c) { const auto v = SettingsFor(c); return std::count(v.begin(), v.end(), LobbyRow::Bodies) == 1; };
    for (Format f : {Format::FreeForAll, Format::Duel, Format::LastManStanding, Format::TwoTeams, Format::TeamDuel, Format::Tournament,
                     Format::PitBridgeDuel, Format::Infection, Format::CrossbowTDM, Format::PlayersVsBots, Format::Zombies}) {
        MatchConfig c; c.format = f; c = Sanitize(c);
        CHECK(c.gear == Gear::Own && has(c));
        const auto v = SettingsFor(c);
        const auto gear = std::find(v.begin(), v.end(), LobbyRow::Gear);
        CHECK(gear != v.end() && gear + 1 != v.end() && gear[1] == LobbyRow::Bodies);          // right after Gear (the tier chips sit under it)
        CHECK(std::count(v.begin(), v.end(), LobbyRow::Tier) == 0);                            // the Starting tier row is the Crypts card's
        for (Gear g : {Gear::RandomTier, Gear::Mirror, Gear::HostKit}) { MatchConfig o = c; o.gear = g; CHECK(!has(o)); }
        MatchConfig own = c; own.ownBodies = true;
        CHECK(has(own) && SettingsFor(own) == v);                                              // the row's value does not move the rows
    }
    for (Format f : {Format::Riot, Format::Juggernaut, Format::Chaos}) { MatchConfig c; c.format = f; CHECK(!has(Sanitize(c))); }   // the roles build the bodies
    { MatchConfig c; c.gear = Gear::Fists; CHECK(!has(Sanitize(c))); }                         // Brawl
    MatchConfig c;
    CHECK(!c.ownBodies && StepSetting(c, LobbyRow::Bodies, 1).ownBodies && StepSetting(c, LobbyRow::Bodies, -1).ownBodies);
    CHECK(!StepSetting(StepSetting(c, LobbyRow::Bodies, 1), LobbyRow::Bodies, 1).ownBodies);
    CHECK(SectionOf(LobbyRow::Bodies) == LobbySection::Gear && size_t(LobbyRow::Tier) + 1 == size_t(LobbyRow::Bodies) && size_t(LobbyRow::Bodies) + 1 == size_t(LobbyRow::Weapons) && size_t(LobbyRow::Bodies) < size_t(LobbyRow::Sync));
    LobbyLocal l = InLobby(true, Format::FreeForAll);
    const LobbyScene same = BuildLobbyScene(l);
    CHECK(Need(same, LobbyRow::Bodies).label == "Bodies" && Need(same, LobbyRow::Bodies).value == "Same for all" && Need(same, LobbyRow::Bodies).enabled);
    CHECK(Need(same, LobbyRow::Bodies).hint == "Everyone gets the same build." && Need(same, LobbyRow::Gear).tierChips.empty());   // no tier chips: nothing for it to be By
    CHECK(RowOf(same, LobbyRow::Tier) == nullptr);
    l.config.ownBodies = true;
    const LobbyScene own = BuildLobbyScene(l);
    CHECK(Need(own, LobbyRow::Bodies).value == "Own" && Need(own, LobbyRow::Bodies).hint == "Everyone keeps their own build.");
    l.config.gear = Gear::RandomTier;                    // rolled gear: the flag is kept but does nothing; no Bodies row
    CHECK(RowOf(BuildLobbyScene(l), LobbyRow::Bodies) == nullptr && Need(BuildLobbyScene(l), LobbyRow::Gear).tierChips.size() == 8);
    // By tier shows only while the tier chips do: with bots (the chips are in use) the same build reads By tier, Own the other way,
    // and the row steps between those two whatever it is called.
    LobbyLocal bots = InLobby(true, Format::PlayersVsBots);
    CHECK(Need(BuildLobbyScene(bots), LobbyRow::Gear).tierChips.size() == 8);
    CHECK(Need(BuildLobbyScene(bots), LobbyRow::Bodies).value == "By tier" && Need(BuildLobbyScene(bots), LobbyRow::Bodies).hint == "Strength from the tier.");
    bots.config.ownBodies = true;
    CHECK(Need(BuildLobbyScene(bots), LobbyRow::Bodies).value == "Own");
    CHECK(std::string(BodiesName(MatchConfig{})) == "Same for all" && std::string(BodiesName(bots.config)) == "Own");
    MatchConfig tiered; tiered.gear = Gear::Crypts;
    CHECK(TierInUse(tiered) && std::string(BodiesName(tiered)) == "By tier" && !TierInUse(MatchConfig{}));
    tiered.ownBodies = true; CHECK(std::string(BodiesName(tiered)) == "Own");
    tiered.gear = Gear::Own; tiered.ownBodies = false; tiered.format = Format::Zombies;
    CHECK(TierInUse(tiered) && std::string(BodiesName(tiered)) == "By tier");                 // bots
    tiered.format = Format::FreeForAll; CHECK(!TierInUse(tiered) && std::string(BodiesName(tiered)) == "Same for all");
    tiered.gear = Gear::Mirror; CHECK(TierInUse(tiered)); tiered.gear = Gear::RandomTier; CHECK(TierInUse(tiered));
    tiered.gear = Gear::HostKit; CHECK(!TierInUse(tiered));
    LobbyLocal guest = InLobby(false, Format::FreeForAll);
    CHECK(!Need(BuildLobbyScene(guest), LobbyRow::Bodies).enabled);
}

// The Gear row says everything in its value (Own kits, Host's kit, Random, Identical): no hint, except the bot formats, where it
// carries the tier chips and says the bots start at the tier, and Blossfechten, where the value would say Own kits but everyone
// gets a random sword.
static void TestGearHint() {
    for (Format f : {Format::FreeForAll, Format::TwoTeams, Format::Duel, Format::LastManStanding})
        for (Gear g : {Gear::Own, Gear::RandomTier, Gear::Mirror, Gear::HostKit}) CHECK(GearHint(f, g).empty());
    for (Format f : {Format::PlayersVsBots, Format::Zombies})
        for (Gear g : {Gear::Own, Gear::RandomTier, Gear::Mirror, Gear::HostKit}) CHECK(GearHint(f, g) == kBotsHint);
    for (Format f : {Format::PlayersVsBots, Format::Zombies})
        CHECK(Need(BuildLobbyScene(InLobby(true, f, Gear::Mirror)), LobbyRow::Gear).value == "Identical");
    CHECK(std::string(GearName(Gear::Own)) == "Own kits" && std::string(GearName(Gear::HostKit)) == "Host's kit");
    CHECK(std::string(GearName(Gear::RandomTier)) == "Random" && std::string(GearName(Gear::Mirror)) == "Identical");
    CHECK(std::string(GearName(Gear::Crypts)) == "Crypts loot");
}

// Most rows have no hint: the label and the value say it. The few that keep one say what neither can.
static void TestOtherHints() {
    auto hint = [](Format f, LobbyRow id, auto tweak) { LobbyLocal l = InLobby(true, f, Gear::RandomTier); tweak(l.config); return Need(BuildLobbyScene(l), id).hint; };
    auto none = [](MatchConfig&) {};
    for (Format f : {Format::FreeForAll, Format::TwoTeams, Format::LastManStanding, Format::Duel, Format::Zombies, Format::PlayersVsBots, Format::Roulette, Format::Tournament}) {
        for (int variant = 0; variant < 2; ++variant) {                                // default, then a clock and friendly fire on
            LobbyLocal l = InLobby(true, f, Gear::RandomTier);
            if (variant) { l.config.timeLimitS = 300; l.config.friendlyFire = true; }
            const LobbyScene sc = BuildLobbyScene(l);
            for (LobbyRow id : {LobbyRow::Limit, LobbyRow::Time, LobbyRow::Respawn, LobbyRow::FriendlyFire})
                if (const SettingRow* r = RowOf(sc, id)) CHECK(r->hint.empty());
        }
    }
    // Bots start at the tier picked here, and rise: the one tier hint, now on the Gear row that carries the chips.
    CHECK(hint(Format::Zombies, LobbyRow::Gear, none) == kBotsHint);
    CHECK(hint(Format::PlayersVsBots, LobbyRow::Gear, none) == hint(Format::Zombies, LobbyRow::Gear, none));
    CHECK(hint(Format::Duel, LobbyRow::Gear, none).empty() && hint(Format::FreeForAll, LobbyRow::Gear, none).empty());
    auto own = [](MatchConfig& c) { c.gear = Gear::Own; };
    CHECK(hint(Format::Duel, LobbyRow::Gear, own).empty());
    CHECK(hint(Format::PlayersVsBots, LobbyRow::Gear, own) == kBotsHint);
}

static void TestMyTeam() {
    LobbyLocal l = InLobby(false, Format::TwoTeams);
    LobbyScene s = BuildLobbyScene(l);
    CHECK(Need(s, LobbyRow::MyTeam).label == "My team" && Need(s, LobbyRow::MyTeam).value == "Auto");
    CHECK(Need(s, LobbyRow::MyTeam).hint.empty());                  // Auto and the value say it
    l.players[1].team = 1;
    CHECK(Need(BuildLobbyScene(l), LobbyRow::MyTeam).value == "Blue");
    l.players[1].team = 2;                                   // Green, picked in three teams: Auto here
    CHECK(Need(BuildLobbyScene(l), LobbyRow::MyTeam).value == "Auto");
    l.config.teams = 3;
    CHECK(Need(BuildLobbyScene(l), LobbyRow::MyTeam).value == "Green");
    l.players[1].hostOverride = 0;
    s = BuildLobbyScene(l);
    CHECK(Need(s, LobbyRow::MyTeam).value == "Green" && Need(s, LobbyRow::MyTeam).hint == "The host moved you to Red.");
    l.players[1].hostOverride = 3;                           // no fourth team: no move
    CHECK(Need(BuildLobbyScene(l), LobbyRow::MyTeam).hint.empty());
    l.config.teams = 4;                                      // the fourth team: Gold
    CHECK(Need(BuildLobbyScene(l), LobbyRow::MyTeam).hint == "The host moved you to Gold.");
    l.players[1].team = 3; l.players[1].hostOverride = -1;
    CHECK(Need(BuildLobbyScene(l), LobbyRow::MyTeam).value == "Gold");
    l.config.teams = 2;                                      // a pick the count no longer has is Auto
    CHECK(Need(BuildLobbyScene(l), LobbyRow::MyTeam).value == "Auto");
    l.config.teams = 3; l.config.format = Format::ThreeTeams;   // the old value is three teams
    l.players[1].team = 2;
    CHECK(Need(BuildLobbyScene(l), LobbyRow::MyTeam).value == "Green");
    l.players[1].me = false;                                 // not in the list yet
    CHECK(Need(BuildLobbyScene(l), LobbyRow::MyTeam).value == "Auto");
}

static void TestRoster() {
    LobbyLocal l = InLobby(true, Format::TwoTeams);
    LobbyPlayer cy = Player("Cy", false, false, 0, 1); cy.ready = true;
    l.players = {Player("Ann", true, true, 1), Player("", false, false, 2), cy};
    LobbyScene s = BuildLobbyScene(l);
    CHECK(s.playersTitle == "PLAYERS 3/8" && s.roster.size() == 3);
    if (s.roster.size() == 3) {
        const RosterRow& a = s.roster[0];
        CHECK(a.name == "Ann" && a.host && a.me && a.team == "Blue" && Same(a.swatch, TeamColor(1)) && !a.moved && !a.ready);
        const RosterRow& b = s.roster[1];
        CHECK(b.name == "?" && !b.host && !b.me && b.team == "Auto" && Near(b.swatch.a, 0) && !b.moved);
        const RosterRow& c = s.roster[2];
        CHECK(c.team == "Blue" && c.moved && c.ready && Same(c.swatch, TeamColor(1)));   // the host's move wins
    }
    l.players[2].hostOverride = 2;                           // a move to a team this format lacks: their pick stands
    s = BuildLobbyScene(l);
    if (s.roster.size() == 3) CHECK(s.roster[2].team == "Red" && !s.roster[2].moved && Same(s.roster[2].swatch, TeamColor(0)));

    LobbyScene member = BuildLobbyScene(InLobby(false, Format::TwoTeams));   // the host is someone else
    if (member.roster.size() == 2) CHECK(member.roster[0].host && !member.roster[0].me && !member.roster[1].host && member.roster[1].me);

    LobbyLocal f = InLobby(true, Format::FreeForAll);
    f.players = {Player("Ann", true, true, 1, 0)};
    s = BuildLobbyScene(f);
    if (s.roster.size() == 1) CHECK(s.roster[0].team.empty() && Near(s.roster[0].swatch.a, 0) && !s.roster[0].moved && !s.roster[0].movable);
    f.maxPlayers = 4;
    CHECK(BuildLobbyScene(f).playersTitle == "PLAYERS 1/4");
    LobbyLocal z = InLobby(true, Format::Zombies);
    z.players = {Player("Ann", true, true, 1, 1)};
    s = BuildLobbyScene(z);
    if (s.roster.size() == 1) CHECK(s.roster[0].team.empty() && !s.roster[0].moved && !s.roster[0].movable);
}

// A member marks themselves ready (their row's dot); the toggle reads and frames their own ready only. The host starts
// the match instead.
static void TestReady() {
    LobbyLocal l = InLobby(false);                           // Bob, a member
    LobbyScene s = BuildLobbyScene(l);
    CHECK(s.ready && !s.readyOn && s.readyLabel == "Mark me ready");
    l.players[1].ready = true;
    s = BuildLobbyScene(l);
    CHECK(s.ready && s.readyOn && s.readyLabel == "Ready " + kDot + " click to undo");
    if (s.roster.size() == 2) CHECK(s.roster[1].me && s.roster[1].ready && !s.roster[0].ready);
    l.players[0].ready = true; l.players[1].ready = false;   // the host's ready is not ours
    s = BuildLobbyScene(l);
    CHECK(s.ready && !s.readyOn && s.readyLabel == "Mark me ready");
    l.players = {Player("Ann", true)};                       // our own row not listed yet
    s = BuildLobbyScene(l);
    CHECK(s.ready && !s.readyOn && s.readyLabel == "Mark me ready");

    LobbyLocal h = InLobby(true);
    h.players[0].ready = true;
    s = BuildLobbyScene(h);
    CHECK(!s.ready && !s.readyOn && s.readyLabel.empty() && s.start);

    for (LobbyPhase p : {LobbyPhase::Offline, LobbyPhase::Idle, LobbyPhase::Creating, LobbyPhase::Joining}) {
        LobbyLocal o = InLobby(false);
        o.phase = p; o.players[1].ready = true;
        s = BuildLobbyScene(o);
        CHECK(!s.ready && !s.readyOn && s.readyLabel.empty());
    }
}

static void TestModes() {
    LobbyScene s = BuildLobbyScene(InLobby(true, Format::Duel));   // two players: the Duel card reads Duel
    CHECK(s.modes.size() == kModeCardCount);
    // Duel and Last man standing are one card (the second); Pit bridge duels have none; Blossfechten is a card of Last man standing's rounds.
    const Format order[] = {Format::FreeForAll, Format::LastManStanding, Format::TwoTeams, Format::TeamDuel, Format::Riot,
                            Format::Juggernaut, Format::Tag, Format::Infection, Format::CrossbowTDM, Format::Tournament, Format::LastManStanding,
                            Format::Chaos, Format::OneCrossbow, Format::Roulette, Format::HideSeek, Format::PlayersVsBots, Format::Zombies};
    const char* names[] = {"CARNAGE", "DUEL", "TEAMS", "TEAM DUEL", "RIOT", "JUGGERNAUT", "TAG", "INFECTION",
                           "CROSSBOW TDM", "TOURNAMENT", "BLOSSFECHTEN", "CHAOS", "ONE CROSSBOW", "ROULETTE", "HIDE AND SEEK", "SURVIVAL", "ZOMBIES"};
    CHECK(kModeOrderCount == 17 && kModeCardCount == 22);
    for (size_t i = 0; i < s.modes.size() && i < kModeOrderCount; ++i) {
        CHECK(s.modes[i].format == order[i] && s.modes[i].name == names[i] && !s.modes[i].description.empty());
        CHECK(s.modes[i].selected == (i == 1));
        CHECK(s.modes[i].blossfechten == (i == 10));
    }
    if (s.modes.size() > 10) CHECK(s.modes[10].description == "Swords only, no armour.");
    const LobbyScene three = [] { LobbyLocal l = InLobby(true, Format::LastManStanding); l.players.push_back(Player("Cy", false, false)); return BuildLobbyScene(l); }();
    CHECK(three.modes[1].name == "LAST MAN STANDING" && three.modes[1].selected);   // three players: the same card reads Last man standing
    if (s.modes.size() > 1)
        CHECK(s.modes[1].description == "Rounds, last one standing.");
    CHECK(std::string(FormatBlurb(Format::Duel)) == FormatBlurb(Format::LastManStanding) && std::string(FormatBlurb(Format::LastManStanding)) == "Rounds, last one standing.");   // one card, one line
    CHECK(std::string(FormatBlurb(Format::FreeForAll)) == "Everyone for themselves.");
    CHECK(std::string(FormatBlurb(Format::TwoTeams)) == "Two to four teams.");
    CHECK(std::string(FormatBlurb(Format::ThreeTeams)) == FormatBlurb(Format::TwoTeams));
    CHECK(std::string(FormatBlurb(Format::PlayersVsBots)) == "Waves of bots.");
    CHECK(std::string(FormatBlurb(Format::Zombies)) == "Zombie waves in the Abyss.");
}

// The Brawl card: free-for-all with bare fists. A mode card (format free-for-all, gear Fists), not a format; no Gear or
// Tier row; picking it and leaving it restore the settings.
static void TestBrawl() {
    LobbyScene s = BuildLobbyScene(InLobby(true, Format::FreeForAll, Gear::Fists));
    CHECK(s.modes.size() == kModeCardCount && kModeCardCount == size_t(kModeOrderCount) + 5);
    const ModeCard& card = s.modes[kModeOrderCount];                          // Brawl, then CRYPTS, Progression, Crypt Royale and Grave Robbers
    CHECK(card.brawl && card.format == Format::FreeForAll && card.name == "BRAWL" && card.selected && card.enabled && !card.progression);
    CHECK(card.description == "Bare fists, everyone for themselves.");
    int selected = 0, brawls = 0;
    for (const ModeCard& m : s.modes) { selected += m.selected; brawls += m.brawl; }
    CHECK(selected == 1 && brawls == 1 && !s.modes[0].selected);              // the free-for-all card is not also lit
    CHECK(Ids(s) == (std::vector<LobbyRow>{LobbyRow::Players, LobbyRow::Limit, LobbyRow::Time, LobbyRow::Respawn, LobbyRow::Damage, LobbyRow::DeadWeight, LobbyRow::WeaponWeight, LobbyRow::Bite, LobbyRow::Strip, LobbyRow::TimeOfDay, LobbyRow::ClearGround, LobbyRow::Props, LobbyRow::Sync}));   // no Gear, no Tier
    CHECK(RowOf(s, LobbyRow::Gear) == nullptr && RowOf(s, LobbyRow::Tier) == nullptr);
    LobbyScene own = BuildLobbyScene(InLobby(true));
    CHECK(own.modes[0].selected && !own.modes[kModeOrderCount].selected && RowOf(own, LobbyRow::Gear) != nullptr);
    LobbyScene member = BuildLobbyScene(InLobby(false, Format::FreeForAll, Gear::Fists));
    CHECK(member.modes[kModeOrderCount].selected && !member.modes[kModeOrderCount].enabled);
    LobbyLocal prog = InLobby(true, Format::FreeForAll, Gear::Fists); prog.progression = true;
    CHECK(!BuildLobbyScene(prog).modes[kModeOrderCount].selected);   // the Progression card wins

    MatchConfig c; c.format = Format::TwoTeams; c.scoreLimit = 30; c.gear = Gear::Mirror; c.tier = 6; c.timeLimitS = 600;
    MatchConfig b = SelectMode(c, Format::FreeForAll, true);
    CHECK(IsBrawl(b) && b.gear == Gear::Fists && b.tier == kBrawlTier && b.scoreLimit == DefaultScoreLimit(Format::FreeForAll) && b.timeLimitS == 600);
    CHECK(Sanitize(b).gear == Gear::Fists && Sanitize(b).tier == kBrawlTier);
    b.tier = 6; CHECK(Sanitize(b).tier == kBrawlTier); b.tier = kBrawlTier;   // Brawl's tier is fixed
    b.scoreLimit = 20;
    CHECK(SelectMode(b, Format::FreeForAll, true).scoreLimit == 20);          // the card again changes nothing
    MatchConfig back = SelectMode(b, Format::FreeForAll, false);              // the free-for-all card: same format, other rules
    CHECK(!IsBrawl(back) && back.format == Format::FreeForAll && back.gear == Gear::Own && back.tier == MatchConfig{}.tier && back.scoreLimit == 5);
    MatchConfig duel = SelectMode(b, Format::Duel, false);                    // the Duel card is the Last man standing one
    CHECK(duel.format == Format::LastManStanding && duel.gear == Gear::Own && duel.tier == MatchConfig{}.tier && duel.scoreLimit == DefaultScoreLimit(Format::Duel));
    CHECK(SelectMode(c, Format::TwoTeams, false).scoreLimit == 30 && SelectMode(c, Format::TwoTeams, false).gear == Gear::Mirror);   // unchanged
    CHECK(SelectMode(c, Format::Duel, false).gear == Gear::Mirror && SelectMode(c, Format::LastManStanding, false).format == Format::LastManStanding);                                                                    // as SelectFormat
    // The Gear row never lands on Fists, whichever way it is stepped.
    MatchConfig g; g.gear = Gear::Mirror;
    CHECK(StepSetting(g, LobbyRow::Gear, 1).gear == Gear::HostKit && StepSetting(StepSetting(g, LobbyRow::Gear, -1), LobbyRow::Gear, -1).gear == Gear::Own);
    g.gear = Gear::Fists;
    CHECK(StepSetting(g, LobbyRow::Gear, 1).gear == Gear::RandomTier && StepSetting(g, LobbyRow::Gear, -1).gear == Gear::HostKit);
    g.gear = Gear::Own;                                  // all the way round, both ways: four stops, never Fists
    for (int dir : {1, -1}) {
        MatchConfig w = g; std::set<uint8_t> seen;
        for (int i = 0; i < 4; ++i) { w = StepSetting(w, LobbyRow::Gear, dir); seen.insert(uint8_t(w.gear)); }
        CHECK(w.gear == Gear::Own && seen.size() == 4 && !seen.count(uint8_t(Gear::Fists)));
    }
}

static size_t IndexOf(const char* display) { for (size_t i = 0; i < MapCount(); ++i) if (std::string(kMaps[i].display) == display) return i; return MapCount(); }

static const MapCard* Card(const LobbyScene& s, size_t index) { for (const MapCard& m : s.maps) if (m.index == index) return &m; return nullptr; }

static void TestMaps() {
    LobbyLocal l = InLobby(true, Format::FreeForAll);
    l.mapPick = "/Game/Maps/Arenas/Map_Arena_Yard";
    LobbyScene s = BuildLobbyScene(l);
    CHECK(s.maps.size() == PickableCount());                    // Cutting, Tavern and Smithery are not offered
    size_t yard = IndexOf("Yard"), tavern = IndexOf("Tavern"), abyss = IndexOf("Abyss"), alley = IndexOf("Alley");
    CHECK(yard < MapCount() && tavern < MapCount() && abyss < MapCount() && alley < MapCount());
    if (s.maps.size() != PickableCount() || yard >= MapCount() || tavern >= MapCount() || abyss >= MapCount() || alley >= MapCount()) return;
    CHECK(!Card(s, tavern) && !Card(s, IndexOf("Cutting")) && !Card(s, IndexOf("Smithery")) && Card(s, abyss));
    for (const MapCard& m : s.maps) {
        CHECK(m.name == kMaps[m.index].display);
        CHECK(m.selected == (m.index == yard));
        CHECK(m.allowed == (m.index != abyss));   // the Abyss is for Zombies and Infection
    }
    CHECK(Card(s, yard)->tag == "Arena");
    CHECK(Card(s, abyss)->tag == "Large level " + kDot + " no spawn points: ring spawn");

    l.config = SelectFormat(l.config, Format::Duel);
    l.mapPick = "/Game/Maps/Map_Hub_Tavern_Frank";              // not pickable: the first arena instead
    s = BuildLobbyScene(l);
    CHECK(Card(s, alley)->selected);
    CHECK(!Card(s, abyss)->allowed && Card(s, abyss)->reason == "Not for this mode" && !Card(s, abyss)->enabled);   // Duel follows Last man standing's maps (0.9.0)
    CHECK(Card(s, yard)->allowed && Card(s, yard)->enabled && Card(s, yard)->reason.empty());

    l.config = SelectFormat(l.config, Format::Zombies);
    s = BuildLobbyScene(l);
    for (const MapCard& m : s.maps) {
        if (kMaps[m.index].crypts) { CHECK(m.allowed && m.enabled && !m.selected); continue; }   // the Crypts map is always pickable: it enters Crypt Royale
        CHECK(m.allowed == (m.index == abyss) && m.selected == (m.index == abyss));
        if (m.index != abyss) CHECK(m.reason == "Zombies needs the Abyss");
    }
    CHECK(std::string(MapRefusal(Format::FreeForAll)) == "Not for this mode");
}

// Item 8: another player on a different mod build is named, and the host cannot start until it matches or the
// override is on. Builds not known yet say nothing. The words are the START gate's (match/hsm_start_gate.h).
static void TestBuildMismatch() {
    LobbyLocal h = InLobby(true);
    h.myBuild = "0.6.4";
    h.players[1].build = "0.6.4";
    LobbyScene s = BuildLobbyScene(h);
    CHECK(s.start && !s.buildMismatch && s.waiting.empty() && s.startWhy.empty() && !s.waitingWarn);
    h.players[1].build = "";                          // Bob's member data has not come yet
    s = BuildLobbyScene(h);
    CHECK(s.start && !s.buildMismatch);
    h.players[1].joined = true;                       // ... but the host has his Hello, and it had no build: an older HSMMod
    s = BuildLobbyScene(h);
    CHECK(!s.start && s.buildMismatch && s.waiting == "Bob is on an older build. Everyone has to update before you can start.");
    CHECK(s.startWhy == s.waiting && s.waitingWarn);   // a blocked host reads the line in the warning colour
    h.players[1].joined = false;
    h.players[1].build = "0.6.3";
    s = BuildLobbyScene(h);
    CHECK(!s.start && s.buildMismatch && s.waiting == "Bob is on a different build. Everyone needs the same build to start.");
    CHECK(s.startWhy == s.waiting);
    h.players[0].build = "9.9.9";                     // my own row never counts against me
    CHECK(BuildLobbyScene(h).waiting == "Bob is on a different build. Everyone needs the same build to start.");
    h.allowBuildMismatch = true;
    s = BuildLobbyScene(h);
    CHECK(s.start && s.buildMismatch && s.waiting == "Bob is on a different build. You allowed it, so you can still start." && s.startWhy.empty() && s.waitingWarn);
    CHECK(s.waiting.find("allow_build_mismatch") == std::string::npos);
    LobbyLocal m = InLobby(false);                    // a member sees the build named instead of "Waiting"
    m.myBuild = "0.6.3";
    m.players[0].build = "0.6.4";
    s = BuildLobbyScene(m);
    CHECK(!s.start && s.buildMismatch && s.waiting == "Ann is on a different build. Everyone needs the same build to start.");
    CHECK(s.startWhy == "Only the host starts the match.");
    m.allowBuildMismatch = true;                      // a member's own override opens nothing
    CHECK(!BuildLobbyScene(m).start);
    m.allowBuildMismatch = false;
    m.players[0].build = "0.6.3";
    CHECK(BuildLobbyScene(m).waiting == "Waiting for the host to start." && !BuildLobbyScene(m).waitingWarn);   // plain waiting is not a warning
}

static void TestDamage() {
    DamageRange r;                                         // until the slider is read: 0..200%, stock 100
    MatchConfig c;
    CHECK(c.damagePct == kDamageStock && Sanitize(c).damagePct == kDamageStock);
    c.damagePct = 5000;
    CHECK(Sanitize(c).damagePct == kDamageMaxPct);
    c.damagePct = kDamageStock;
    CHECK(StepSetting(c, LobbyRow::Damage, 1, r).damagePct == 110 && StepSetting(c, LobbyRow::Damage, -1, r).damagePct == 90);
    c.damagePct = 110;
    CHECK(StepSetting(c, LobbyRow::Damage, -1, r).damagePct == kDamageStock);   // back on the stock value: stored as stock
    c.damagePct = 200;
    CHECK(StepSetting(c, LobbyRow::Damage, 1, r).damagePct == 200);              // the slider's top: no wrap
    c.damagePct = 0;
    CHECK(StepSetting(c, LobbyRow::Damage, -1, r).damagePct == 0);
    MatchConfig only = StepSetting(MatchConfig{}, LobbyRow::Damage, 1, r);
    CHECK(only.tier == MatchConfig{}.tier && only.scoreLimit == MatchConfig{}.scoreLimit);   // a step changes damage alone

    LobbyLocal l = InLobby(true, Format::FreeForAll);
    CHECK(Need(BuildLobbyScene(l), LobbyRow::Damage).label == "Player damage");
    CHECK(Need(BuildLobbyScene(l), LobbyRow::Damage).value == "100%" && Need(BuildLobbyScene(l), LobbyRow::Damage).enabled);
    l.config.damagePct = 150;
    CHECK(Need(BuildLobbyScene(l), LobbyRow::Damage).value == "150%");
    LobbyLocal m = InLobby(false, Format::FreeForAll);
    CHECK(!Need(BuildLobbyScene(m), LobbyRow::Damage).enabled);                 // the host's alone
    LobbyLocal prog = InLobby(true, Format::FreeForAll); prog.progression = true;
    CHECK(Ids(BuildLobbyScene(prog)) == (std::vector<LobbyRow>{LobbyRow::Players, LobbyRow::Damage, LobbyRow::DeadWeight, LobbyRow::WeaponWeight}));   // co-op takes them too
}

// The Dead weight and Weapon weight rows: 50% of the game's 0..1 sliders (its own default) unless the host moves them,
// 5% a step, 0..100%.
static void TestWeights() {
    MatchConfig c;
    CHECK(c.deadWeightPct == 50 && c.weaponWeightPct == 50 && Sanitize(c).deadWeightPct == 50 && Sanitize(c).weaponWeightPct == 50);
    CHECK(kWeightDefaultPct == kWeightStockPct && WeightScale(kWeightDefaultPct) == 0.5f);   // the game's own 0.5
    CHECK(WeightScale(20) == 0.2f && WeightScale(0) == 0.f && WeightScale(100) == 1.f && WeightScale(kWeightStockPct) == 0.5f);
    c.deadWeightPct = 250; c.weaponWeightPct = 101;
    CHECK(Sanitize(c).deadWeightPct == 100 && Sanitize(c).weaponWeightPct == 100);
    c.deadWeightPct = 0; c.weaponWeightPct = 35;
    CHECK(Sanitize(c).deadWeightPct == 0 && Sanitize(c).weaponWeightPct == 35);
    for (LobbyRow r : {LobbyRow::DeadWeight, LobbyRow::WeaponWeight}) {
        MatchConfig m;
        CHECK(WeightField(m, r) == (r == LobbyRow::DeadWeight ? &m.deadWeightPct : &m.weaponWeightPct) && *WeightField(m, r) == 50);
        MatchConfig up = StepSetting(m, r, 1), down = StepSetting(m, r, -1);
        CHECK(*WeightField(up, r) == 55 && *WeightField(down, r) == 45);
        const LobbyRow other = r == LobbyRow::DeadWeight ? LobbyRow::WeaponWeight : LobbyRow::DeadWeight;
        CHECK(*WeightField(up, other) == 50 && up.damagePct == m.damagePct && up.tier == m.tier);   // a step changes that row alone
        *WeightField(m, r) = 22;                                   // off the 5% grid: the next step that way
        CHECK(*WeightField(StepSetting(m, r, 1), r) == 25 && *WeightField(StepSetting(m, r, -1), r) == 20);
        *WeightField(m, r) = 100;
        CHECK(*WeightField(StepSetting(m, r, 1), r) == 100 && *WeightField(StepSetting(m, r, -1), r) == 95);   // no wrap
        *WeightField(m, r) = 0;
        CHECK(*WeightField(StepSetting(m, r, -1), r) == 0 && *WeightField(StepSetting(m, r, 1), r) == 5);
        CHECK(SectionOf(r) == SectionOf(LobbyRow::Damage));
    }
    MatchConfig m;
    CHECK(WeightField(m, LobbyRow::Damage) == nullptr && WeightField(m, LobbyRow::Tier) == nullptr);
    CHECK(WeightFromSlider(0.f) == 0 && WeightFromSlider(19.9f) == 20 && WeightFromSlider(22.4f) == 20 && WeightFromSlider(22.6f) == 25);
    CHECK(WeightFromSlider(100.f) == 100 && WeightFromSlider(150.f) == 100 && WeightFromSlider(-3.f) == 0 && WeightFromSlider(std::nanf("")) == 0);
    CHECK(WeightLabel(20) == "20%" && WeightLabel(50) == "50%" && WeightLabel(0) == "0%");

    LobbyLocal l = InLobby(true, Format::FreeForAll);
    CHECK(Need(BuildLobbyScene(l), LobbyRow::DeadWeight).label == "Dead weight" && Need(BuildLobbyScene(l), LobbyRow::DeadWeight).value == "50%");
    CHECK(Need(BuildLobbyScene(l), LobbyRow::WeaponWeight).label == "Weapon weight" && Need(BuildLobbyScene(l), LobbyRow::WeaponWeight).value == "50%");
    CHECK(Need(BuildLobbyScene(l), LobbyRow::DeadWeight).enabled && Need(BuildLobbyScene(l), LobbyRow::WeaponWeight).enabled);
    l.config.deadWeightPct = 20; l.config.weaponWeightPct = 75;
    CHECK(Need(BuildLobbyScene(l), LobbyRow::DeadWeight).value == "20%" && Need(BuildLobbyScene(l), LobbyRow::WeaponWeight).value == "75%");
    LobbyLocal guest = InLobby(false, Format::FreeForAll);
    CHECK(!Need(BuildLobbyScene(guest), LobbyRow::DeadWeight).enabled && !Need(BuildLobbyScene(guest), LobbyRow::WeaponWeight).enabled);   // the host's alone
}

static void TestModdedMaps() {
    LobbyLocal l = InLobby(true, Format::FreeForAll);
    l.modMaps = {{"/Game/Maps/Custom/Arena_Bog", "Arena Bog", "BogMap_P.pak"}, {"/Game/Maps/Custom/Plain", "Plain", ""}};
    l.mapPick = "/Game/Maps/Arenas/Map_Arena_Yard";
    LobbyScene s = BuildLobbyScene(l);
    CHECK(s.maps.size() == PickableCount() + 2);
    if (s.maps.size() != PickableCount() + 2) return;
    const MapCard& bog = s.maps[PickableCount()];
    const MapCard& plain = s.maps[PickableCount() + 1];
    CHECK(bog.modded && bog.index == MapCount() && bog.path == "/Game/Maps/Custom/Arena_Bog" && bog.name == "Arena Bog");
    CHECK(bog.tag == "Modded map \xC2\xB7 BogMap_P.pak" && plain.tag == "Modded map" && plain.index == MapCount() + 1);
    CHECK(bog.allowed && bog.enabled && !bog.selected && !plain.selected);
    size_t selectedStock = 0;
    for (const MapCard& m : s.maps) if (!m.modded && m.selected) ++selectedStock;
    CHECK(selectedStock == 1);                                     // the stock pick stays marked while no custom map is picked
    // Picking a custom map: it alone is selected (the object-name form of the pick counts), and START waits for the hash.
    l.mapPick = "/game/maps/custom/arena_bog.Arena_Bog";
    s = BuildLobbyScene(l);
    CHECK(s.maps[PickableCount()].selected && !s.maps[PickableCount() + 1].selected);
    selectedStock = 0;
    for (const MapCard& m : s.maps) if (!m.modded && m.selected) ++selectedStock;
    CHECK(selectedStock == 0);
    CHECK(s.start);
    l.modPickPending = true; s = BuildLobbyScene(l);
    CHECK(!s.start && s.waiting == "Checking the map pack." && s.startWhy == "Checking the map pack. Try again in a moment.");
    l.mapPick = "/Game/Maps/Arenas/Map_Arena_Yard"; s = BuildLobbyScene(l);   // a stock pick ignores the flag
    CHECK(s.start);
    // A member sees them, disabled; Zombies refuses them (the Abyss is its map); Progression shows no maps.
    LobbyLocal m = InLobby(false); m.modMaps = l.modMaps; m.mapPick = "/Game/Maps/Custom/Plain";
    s = BuildLobbyScene(m);
    CHECK(s.maps.size() == PickableCount() + 2 && !s.maps[PickableCount()].enabled && s.maps[PickableCount() + 1].selected);
    LobbyLocal z = InLobby(true, Format::Zombies); z.modMaps = l.modMaps; z.mapPick = "/Game/Maps/Custom/Plain";
    s = BuildLobbyScene(z);
    CHECK(s.maps.size() == PickableCount() + 2 && !s.maps[PickableCount()].allowed && !s.maps[PickableCount()].enabled &&
          !s.maps[PickableCount()].reason.empty() && !s.maps[PickableCount() + 1].selected);
    LobbyLocal pr = InLobby(true); pr.modMaps = l.modMaps; pr.progression = true;
    CHECK(BuildLobbyScene(pr).maps.empty());
}

static void TestGroupsAndSections() {
    LobbyScene s = BuildLobbyScene(InLobby(true));
    CHECK(s.modes.size() == kModeCardCount);
    for (size_t i = 0; i < s.modes.size(); ++i) CHECK(s.modes[i].group == ModeGroupAt(i));
    for (size_t i = 0; i < s.modes.size(); ++i) {
        const ModeCard& c = s.modes[i];
        const bool coop = c.progression || (!c.brawl && !c.crypts && IsBotFormat(c.format)) || i >= kModeOrderCount + 2;   // Progression, Crypt Royale, Grave Robbers
        CHECK((c.group == ModeGroup::Coop) == coop);
    }
    CHECK(ModeGroupAt(0) == ModeGroup::Versus && ModeGroupAt(kModeOrderCount) == ModeGroup::Versus &&
          ModeGroupAt(kModeOrderCount + 1) == ModeGroup::Versus && ModeGroupAt(kModeOrderCount + 2) == ModeGroup::Coop &&
          ModeGroupAt(kModeOrderCount + 3) == ModeGroup::Coop && ModeGroupAt(kModeOrderCount + 4) == ModeGroup::Coop);   // Brawl, CRYPTS; Progression, Crypt Royale, Grave Robbers
    for (Format f : {Format::Duel, Format::LastManStanding, Format::TwoTeams}) {         // Duel, Last Man Standing and Teams are versus modes
        for (size_t i = 0; i < kModeOrderCount; ++i) if (kModeOrder[i].format == f) CHECK(ModeGroupAt(i) == ModeGroup::Versus);
    }
    CHECK(SectionOf(LobbyRow::Limit) == LobbySection::Match && SectionOf(LobbyRow::Damage) == LobbySection::Combat);   // PR4: the damage rows have a sub-tab of their own
    CHECK(SectionOf(LobbyRow::MyTeam) == LobbySection::Teams && SectionOf(LobbyRow::FriendlyFire) == LobbySection::Teams && SectionOf(LobbyRow::Teams) == LobbySection::Teams);
    CHECK(SectionOf(LobbyRow::Gear) == LobbySection::Gear && SectionOf(LobbyRow::Tier) == LobbySection::Gear);
    CHECK(SectionOf(LobbyRow::ClearGround) == LobbySection::World && SectionOf(LobbyRow::Sync) == LobbySection::World);
    CHECK(std::string(SectionName(LobbySection::Match)) == "MATCH" && std::string(SectionName(LobbySection::Combat)) == "COMBAT" && std::string(SectionName(LobbySection::World)) == "WORLD");
    CHECK(std::string(SectionName(LobbySection::Teams)) == "TEAMS" && std::string(SectionName(LobbySection::Gear)) == "GEAR" && size_t(LobbySection::Count) == kSectionCount && kSectionCount == 5);
    for (size_t i = 0; i <= size_t(LobbyRow::Sync); ++i) CHECK(SectionOf(LobbyRow(i)) < LobbySection::Count);
}

// The class row is for lobbies where the class counts, and for players in one: the page never has to open on the classes.
static void TestClassLine() {
    LobbyLocal l = InLobby(false, Format::FreeForAll, Gear::Own);
    CHECK(BuildLobbyScene(l).classLine.empty());
    l.myClass = "Crusader";
    CHECK(BuildLobbyScene(l).classLine == "YOUR CLASS: Crusader");
    l.phase = LobbyPhase::Idle;
    CHECK(BuildLobbyScene(l).classLine.empty());
}

static void TestSummary() {
    LobbyScene s = BuildLobbyScene(InLobby(true, Format::TwoTeams, Gear::RandomTier));
    CHECK(s.summary.rfind("TEAMS (2) " + kDot + " ", 0) == 0);
    CHECK(BuildLobbyScene(Teamed(true, 4)).summary.rfind("TEAMS (4) " + kDot + " ", 0) == 0);
    CHECK(s.summary.find("Random, Commoner") != std::string::npos);
    LobbyScene own = BuildLobbyScene(InLobby(true));
    CHECK(own.summary.find("Own kits") != std::string::npos && own.summary.find("tier") == std::string::npos);
    LobbyScene brawl = BuildLobbyScene(InLobby(true, Format::FreeForAll, Gear::Fists));
    CHECK(brawl.summary.rfind("BRAWL", 0) == 0 && brawl.summary.find("kits") == std::string::npos);
    LobbyLocal pr = InLobby(false); pr.progression = true;
    CHECK(BuildLobbyScene(pr).summary.rfind("PROGRESSION", 0) == 0);
    LobbyLocal idle; idle.phase = LobbyPhase::Idle;
    CHECK(BuildLobbyScene(idle).summary.empty());
}

// Free-for-all reads Carnage in every player text; a host on an older build that still advertises the old name is read as Carnage.
static void TestCarnageName() {
    CHECK(BrowserModes()[1] == "Carnage" && std::count(BrowserModes().begin(), BrowserModes().end(), "Free-for-all") == 0);
    CHECK(BrowserModeShown(Found("Free-for-all")) == "Carnage" && BrowserModeShown(Found("Carnage")) == "Carnage" && BrowserModeShown(Found("Duel", 2)) == "Duel");
    CHECK(ModeMatches(Found("Free-for-all"), "Carnage") && ModeMatches(Found("Carnage"), "Carnage") && !ModeMatches(Found("Duel", 2), "Carnage") && !ModeMatches(Found("Carnage"), "Duel"));
    CHECK(ModeMatches(Found("Teams (3)"), "Teams") && !ModeMatches(Found("Free-for-all"), "Teams"));
    LobbyLocal l; l.phase = LobbyPhase::Idle; l.version = "0.2.0";
    LobbyFound a; a.name = "Old"; a.mode = "Free-for-all"; a.players = 2; a.compatible = true;
    LobbyFound b; b.name = "New"; b.mode = "Carnage"; b.players = 2; b.compatible = true;
    LobbyFound d; d.name = "Duel"; d.mode = "Duel"; d.players = 2; d.compatible = true;
    l.found = {a, b, d};
    l.filter.mode = 1;                                                                         // the Carnage chip
    LobbyScene s = BuildLobbyScene(l);
    CHECK(s.browser.size() == 2 && s.browser[0].name == "Old" && s.browser[1].name == "New");
    for (const BrowserRow& r : s.browser) CHECK(r.detail == "Carnage");
    CHECK(s.filters[kModeChip].label == "Carnage");
    l.filter.mode = 0;
    s = BuildLobbyScene(l);
    CHECK(s.browser.size() == 3 && s.browser[0].detail == "Carnage" && s.browser[2].detail == "Duel");
    CHECK(std::string(FormatName(Format::FreeForAll)) == "Carnage" && Upper(FormatName(Format::FreeForAll)) == "CARNAGE");
}

static void TestFilters() {
    LobbyLocal l; l.phase = LobbyPhase::Idle; l.version = "0.2.0";
    LobbyFound duel; duel.name = "D"; duel.mode = "Duel"; duel.players = 2; duel.compatible = true;
    LobbyFound full; full.name = "F"; full.mode = "Free-for-all"; full.players = 8; full.compatible = true;
    LobbyFound old; old.name = "O"; old.mode = "Duel"; old.players = 5; old.compatible = false;
    l.found = {duel, full, old};
    LobbyScene s = BuildLobbyScene(l);
    CHECK(s.filters.size() == kFilterChips && s.browser.size() == 3 && s.browserCount == "3 lobbies");
    for (const FilterChip& c : s.filters) CHECK(!c.on);
    CHECK(s.filters[kModeChip].label == "Any mode");
    // The mode chip steps through every mode and wraps to any.
    BrowserFilter f;
    for (size_t i = 0; i < BrowserModes().size(); ++i) f = ToggleFilter(f, kModeChip);
    CHECK(f.mode == 0);
    f = ToggleFilter(f, kModeChip); f = ToggleFilter(f, kModeChip);
    CHECK(BrowserModes()[f.mode] == kDuelCardChip);                       // the merged stop: Duel and Last man standing are one
    l.filter = f;
    s = BuildLobbyScene(l);
    CHECK(s.filters[kModeChip].on && s.filters[kModeChip].label == kDuelCardChip && s.browser.size() == 2 && s.browserCount == "2 of 3 lobbies shown");
    l.filter = ToggleFilter(l.filter, kCompatibleChip);                 // compatible only
    s = BuildLobbyScene(l);
    CHECK(s.filters[kCompatibleChip].on && s.browser.size() == 1 && s.browser[0].name == "D");
    l.filter = {};
    l.filter = ToggleFilter(l.filter, kRoomChip);                 // has room
    s = BuildLobbyScene(l);
    CHECK(s.browser.size() == 2 && s.browser[0].name == "D" && s.browser[1].name == "O");
    l.filter = {};
    l.filter = ToggleFilter(l.filter, kMostPlayersChip);                 // most players first
    s = BuildLobbyScene(l);
    CHECK(s.browser.size() == 3 && s.browser[0].name == "F" && s.browser[1].name == "O" && s.browser[2].name == "D");
    CHECK(s.browser[0].source == 1 && s.browser[1].source == 2 && s.browser[2].source == 0);   // what JOIN joins
    CHECK(s.browserEmpty.empty());
    CHECK(s.browserCount == "3 lobbies");
    l.filter = ToggleFilter(l.filter, kMostPlayersChip);
    CHECK(!l.filter.mostPlayers && ToggleFilter(l.filter, 99).mode == l.filter.mode);
    // Every browser mode is a name a host can publish.
    for (const ModeSlot& m : kModeOrder) {
        const std::string want = m.blossfechten ? std::string(kBlossfechtenName) : IsDuelCard(m.format) ? std::string(kDuelCardChip) : ModeName(m.format, Gear::Own);
        bool listed = false;
        for (const std::string& n : BrowserModes()) listed = listed || n == want;
        CHECK(listed);
    }
    l.filter = {}; l.filter.mode = uint8_t(std::find(BrowserModes().begin(), BrowserModes().end(), "Zombies") - BrowserModes().begin());   // Zombies: none listed
    s = BuildLobbyScene(l);
    CHECK(s.browser.empty() && s.browserEmpty == "3 lobbies hidden by your filters (Zombies)." && s.browserCount == "0 of 3 lobbies shown");
    l.found.clear(); l.filter = {};
    s = BuildLobbyScene(l);
    CHECK(s.browserCount.empty() && s.browserEmpty == "No public lobbies yet. REFRESH looks again.");
    LobbyLocal in = InLobby(true);
    CHECK(BuildLobbyScene(in).filters.empty());
}

// Teams 2..4: one Teams card with a Teams row (2, 3, 4 teams), the stepper, the value, and the browser filter.
static void TestTeamsRow() {
    MatchConfig c; c.format = Format::TwoTeams;
    CHECK(c.teams == 2);
    CHECK(StepSetting(c, LobbyRow::Teams, 1).teams == 3 && StepSetting(StepSetting(c, LobbyRow::Teams, 1), LobbyRow::Teams, 1).teams == 4);
    c.teams = 4; CHECK(StepSetting(c, LobbyRow::Teams, 1).teams == 2 && StepSetting(c, LobbyRow::Teams, -1).teams == 3);   // wraps
    c.teams = 2; CHECK(StepSetting(c, LobbyRow::Teams, -1).teams == 4);
    CHECK(StepSetting(c, LobbyRow::Teams, 1).format == Format::TwoTeams && StepSetting(c, LobbyRow::Teams, 1).scoreLimit == c.scoreLimit);
    LobbyLocal l = Teamed(true, 3);
    const LobbyScene ts = BuildLobbyScene(l);
    const SettingRow& row = Need(ts, LobbyRow::Teams);
    CHECK(row.label == "Teams" && row.value == "3 teams" && row.enabled && row.hint.empty());
    CHECK(Need(BuildLobbyScene(Teamed(true, 4)), LobbyRow::Teams).value == "4 teams" && Need(BuildLobbyScene(Teamed(true, 2)), LobbyRow::Teams).value == "2 teams");
    CHECK(!Need(BuildLobbyScene(Teamed(false, 3)), LobbyRow::Teams).enabled);                  // members see it, the host sets it
    for (Format f : {Format::FreeForAll, Format::Duel, Format::LastManStanding, Format::PlayersVsBots, Format::Zombies})
        CHECK(RowOf(BuildLobbyScene(InLobby(true, f)), LobbyRow::Teams) == nullptr);            // only the Teams format has the row
    // The roster shows four teams.
    LobbyLocal r = Teamed(true, 4);
    r.players = {Player("A", true, true, 3), Player("B", false, false, kNoTeam, 3), Player("C", false, false, 1)};
    LobbyScene s = BuildLobbyScene(r);
    CHECK(s.roster.size() == 3 && s.roster[0].team == "Gold" && s.roster[1].team == "Gold" && s.roster[1].moved && s.roster[2].team == "Blue");
    r.config.teams = 3;
    s = BuildLobbyScene(r);
    CHECK(s.roster[0].team == "Auto" && !s.roster[1].moved);
    // One Teams card, selected for the Teams format at any count; no ThreeTeams card.
    for (int t = 2; t <= 4; ++t) {
        int cards = 0, lit = 0;
        for (const ModeCard& m : BuildLobbyScene(Teamed(true, t)).modes) { cards += m.format == Format::TwoTeams || m.format == Format::ThreeTeams; lit += m.selected; }
        CHECK(cards == 1 && lit == 1);
    }
    CHECK(FormatName(Format::TwoTeams) == "Teams" && ModeName(Format::TwoTeams, Gear::Own, 3) == "Teams (3)" && ModeName(Format::TwoTeams, Gear::Own) == "Teams");
    CHECK(ModeName(uint8_t(Format::TwoTeams), uint8_t(Gear::Own), 4) == "Teams (4)" && ModeName(Format::Duel, Gear::Own, 4) == "Duel");
    CHECK(ModeName(Format::LastManStanding, Gear::Own) == "Last man standing");
    // The browser: one "Teams" entry matches "Teams (2)", "Teams (3)" and "Teams (4)", and nothing else.
    const auto& modes = BrowserModes();
    size_t at = 0;
    for (size_t i = 0; i < modes.size(); ++i) if (modes[i] == "Teams") at = i;
    CHECK(at != 0 && std::find(modes.begin(), modes.end(), "2 teams") == modes.end() && std::find(modes.begin(), modes.end(), "3 teams") == modes.end());
    BrowserFilter f; f.mode = uint8_t(at);
    LobbyFound x; x.compatible = true;
    for (const char* m : {"Teams", "Teams (2)", "Teams (3)", "Teams (4)"}) { x.mode = m; CHECK(PassesFilter(x, f)); }
    for (const char* m : {"Duel", "Teams of", "Teamsx (3)", "teams (3)", "", "Last man standing"}) { x.mode = m; CHECK(!PassesFilter(x, f)); }   // the Teams stop holds no Duel card
    CHECK(std::find(modes.begin(), modes.end(), "Last man standing") == modes.end() && std::find(modes.begin(), modes.end(), "Duel") == modes.end());   // one stop for both
    f.mode = uint8_t(std::find(modes.begin(), modes.end(), kDuelCardChip) - modes.begin());
    CHECK(f.mode != 0 && f.mode < modes.size());
    x.mode = "Last man standing"; CHECK(PassesFilter(x, f));
    x.mode = "Duel"; CHECK(PassesFilter(x, f));
    x.mode = "Carnage"; CHECK(!PassesFilter(x, f));
}

// The Team colours row (Wave 2): Tabard / Full kit / Off, default Full kit, in the TEAMS section, for the Teams format only.
static void TestTeamKitRow() {
    MatchConfig c; c.format = Format::TwoTeams;
    CHECK(c.teamKit == TeamKit::Full);
    CHECK(StepSetting(c, LobbyRow::TeamKit, 1).teamKit == TeamKit::Off && StepSetting(c, LobbyRow::TeamKit, -1).teamKit == TeamKit::Tabard);
    c.teamKit = TeamKit::Off;    CHECK(StepSetting(c, LobbyRow::TeamKit, 1).teamKit == TeamKit::Tabard && StepSetting(c, LobbyRow::TeamKit, -1).teamKit == TeamKit::Full);
    c.teamKit = TeamKit::Tabard; CHECK(StepSetting(c, LobbyRow::TeamKit, 1).teamKit == TeamKit::Full && StepSetting(c, LobbyRow::TeamKit, -1).teamKit == TeamKit::Off);
    c.teamKit = TeamKit(9);      CHECK(StepSetting(c, LobbyRow::TeamKit, 1).teamKit == TeamKit::Off);      // out of range steps from Full
    c = MatchConfig{}; c.format = Format::TwoTeams;
    const MatchConfig only = StepSetting(c, LobbyRow::TeamKit, 1);
    CHECK(only.teams == c.teams && only.friendlyFire == c.friendlyFire && only.tier == c.tier && only.scoreLimit == c.scoreLimit && only.damagePct == c.damagePct);
    LobbyLocal l = Teamed(true, 2);
    const LobbyScene scene = BuildLobbyScene(l);
    const SettingRow& r = Need(scene, LobbyRow::TeamKit);
    CHECK(r.label == "Team colours" && r.value == "Full kit" && r.enabled && r.hint.empty() && SectionOf(LobbyRow::TeamKit) == LobbySection::Teams);
    l.config.teamKit = TeamKit::Tabard; CHECK(Need(BuildLobbyScene(l), LobbyRow::TeamKit).value == "Tabard");
    l.config.teamKit = TeamKit::Off;    CHECK(Need(BuildLobbyScene(l), LobbyRow::TeamKit).value == "Off");
    CHECK(!Need(BuildLobbyScene(Teamed(false, 2)), LobbyRow::TeamKit).enabled);       // members see it, the host sets it
    for (Format f : {Format::FreeForAll, Format::Duel, Format::LastManStanding, Format::PlayersVsBots, Format::Zombies})
        CHECK(RowOf(BuildLobbyScene(InLobby(true, f)), LobbyRow::TeamKit) == nullptr);   // team formats only
}

// Tiers read by the game's own names (Enum_Ranks), never as "tier N": the Tier row and the summary line.
static void TestTierNames() {
    const char* const names[] = {"Beggar", "Peasant", "Commoner", "Militia", "Soldier", "Veteran", "Man-at-Arms", "Knight", "Lord"};
    for (int t = 0; t < 9; ++t) CHECK(std::string(TierName(t)) == names[t]);
    CHECK(std::string(TierName(-1)) == "Beggar" && std::string(TierName(9)) == "Lord" && std::string(TierName(255)) == "Lord");
    for (int t = 0; t <= 7; ++t) {
        LobbyLocal l = InLobby(true, Format::FreeForAll, Gear::RandomTier);
        l.config.tier = uint8_t(t);
        const LobbyScene s = BuildLobbyScene(l);
        const SettingRow& g = Need(s, LobbyRow::Gear);                                     // the tier is the chips under Gear (PR4); the budget is not in them
        CHECK(RowOf(s, LobbyRow::Tier) == nullptr && g.tierChips.size() == 8);
        for (int i = 0; i < 8 && i < int(g.tierChips.size()); ++i) CHECK(g.tierChips[i].label == names[i] && g.tierChips[i].on == (i == t));
        CHECK(s.summary.find(std::string(", ") + names[t]) != std::string::npos && s.summary.find("tier " + std::to_string(t)) == std::string::npos);
        l.config.gear = Gear::Own;                                                          // no tier in use: no chips, and no Tier row either
        CHECK(Need(BuildLobbyScene(l), LobbyRow::Gear).tierChips.empty() && RowOf(BuildLobbyScene(l), LobbyRow::Tier) == nullptr);
        l.config.format = Format::Zombies; l.config.gear = Gear::Mirror;                    // the bots start at the tier
        CHECK(Need(BuildLobbyScene(l), LobbyRow::Gear).tierChips.size() == 8 && Need(BuildLobbyScene(l), LobbyRow::Gear).tierChips[t].on);
    }
    LobbyLocal own = InLobby(true, Format::FreeForAll, Gear::Own);
    own.config.tier = 4;
    CHECK(BuildLobbyScene(own).summary.find("Soldier") == std::string::npos);                 // own gear: no tier in the summary
}

// The Shuffle teams row: Off / Each match / Each round (the team rounds only), in the TEAMS section, for the formats that pick teams.
static void TestShuffleRow() {
    using R = LobbyRow;
    MatchConfig c; c.format = Format::TwoTeams;
    CHECK(c.shuffle == TeamShuffle::Off && SectionOf(R::Shuffle) == LobbySection::Teams);
    CHECK(StepSetting(c, R::Shuffle, 1).shuffle == TeamShuffle::EachMatch && StepSetting(c, R::Shuffle, -1).shuffle == TeamShuffle::EachMatch);   // two stops
    c.shuffle = TeamShuffle::EachMatch;
    CHECK(StepSetting(c, R::Shuffle, 1).shuffle == TeamShuffle::Off && StepSetting(c, R::Shuffle, -1).shuffle == TeamShuffle::Off);
    MatchConfig d; d.format = Format::TeamDuel;
    CHECK(StepSetting(d, R::Shuffle, 1).shuffle == TeamShuffle::EachMatch && StepSetting(d, R::Shuffle, -1).shuffle == TeamShuffle::EachRound);
    d.shuffle = TeamShuffle::EachMatch;
    CHECK(StepSetting(d, R::Shuffle, 1).shuffle == TeamShuffle::EachRound && StepSetting(d, R::Shuffle, -1).shuffle == TeamShuffle::Off);
    d.shuffle = TeamShuffle::EachRound;
    CHECK(StepSetting(d, R::Shuffle, 1).shuffle == TeamShuffle::Off && StepSetting(d, R::Shuffle, -1).shuffle == TeamShuffle::EachMatch);
    const MatchConfig only = StepSetting(d, R::Shuffle, 1);
    CHECK(only.teams == d.teams && only.teamKit == d.teamKit && only.tier == d.tier && only.scoreLimit == d.scoreLimit && only.friendlyFire == d.friendlyFire);
    auto has = [](const MatchConfig& m, LobbyRow r) { for (LobbyRow x : SettingsFor(m)) if (x == r) return true; return false; };
    for (int i = 0; i < kFormatCount; ++i) {
        MatchConfig m; m.format = Format(i);
        CHECK(has(m, R::Shuffle) == PicksTeams(Format(i)));
    }
    CHECK(has(c, R::Shuffle) && has(d, R::Shuffle));
    MatchConfig cx; cx.format = Format::CrossbowTDM; CHECK(has(cx, R::Shuffle));
    LobbyLocal l = Teamed(true, 2);
    const SettingRow row = Need(BuildLobbyScene(l), R::Shuffle);
    CHECK(row.label == "Shuffle teams" && row.value == "Off" && row.enabled && row.hint.empty() && row.chips.empty());
    l.config.shuffle = TeamShuffle::EachMatch;
    const SettingRow each = Need(BuildLobbyScene(l), R::Shuffle);
    CHECK(each.value == "Each match" && each.hint == "Teams are dealt again at random. Picks are ignored.");
    LobbyLocal t = InLobby(true, Format::TeamDuel);
    t.config.shuffle = TeamShuffle::EachRound;
    const SettingRow rnd = Need(BuildLobbyScene(t), R::Shuffle);
    CHECK(rnd.value == "Each round" && rnd.hint == each.hint);
    CHECK(!Need(BuildLobbyScene(Teamed(false, 2)), R::Shuffle).enabled);                  // members see it, the host sets it
    // SHUFFLE NOW: the host, in the formats that pick teams.
    CHECK(BuildLobbyScene(Teamed(true, 2)).shuffle && !BuildLobbyScene(Teamed(false, 2)).shuffle);
    CHECK(BuildLobbyScene(InLobby(true, Format::TeamDuel)).shuffle && BuildLobbyScene(InLobby(true, Format::CrossbowTDM)).shuffle);
    for (Format f : {Format::FreeForAll, Format::Duel, Format::LastManStanding, Format::Riot, Format::Juggernaut, Format::Infection, Format::Tag, Format::PlayersVsBots})
        CHECK(!BuildLobbyScene(InLobby(true, f)).shuffle);
    // My team: the pick is ignored while shuffling (the host's move still reads as it was).
    LobbyLocal m = Teamed(false, 2);
    CHECK(Need(BuildLobbyScene(m), R::MyTeam).hint.empty());
    m.config.shuffle = TeamShuffle::EachMatch;
    CHECK(Need(BuildLobbyScene(m), R::MyTeam).hint == "Ignored while Shuffle teams is on.");
    for (LobbyPlayer& p : m.players) if (p.me) p.hostOverride = 1;
    CHECK(Need(BuildLobbyScene(m), R::MyTeam).hint == "The host moved you to Blue.");
}

// The Tagger speed row (Tag): 100 / 110 / 120 percent, default 110.
static void TestTaggerSpeedRow() {
    using R = LobbyRow;
    MatchConfig c; c.format = Format::Tag;
    CHECK(c.taggerSpeedPct == 110 && kTaggerSpeedDefault == 110 && SectionOf(R::TaggerSpeed) == LobbySection::Match);
    CHECK(StepSetting(c, R::TaggerSpeed, 1).taggerSpeedPct == 120 && StepSetting(c, R::TaggerSpeed, -1).taggerSpeedPct == 100);
    c.taggerSpeedPct = 120; CHECK(StepSetting(c, R::TaggerSpeed, 1).taggerSpeedPct == 100 && StepSetting(c, R::TaggerSpeed, -1).taggerSpeedPct == 110);
    c.taggerSpeedPct = 100; CHECK(StepSetting(c, R::TaggerSpeed, 1).taggerSpeedPct == 110 && StepSetting(c, R::TaggerSpeed, -1).taggerSpeedPct == 120);
    c.taggerSpeedPct = 105; CHECK(StepSetting(c, R::TaggerSpeed, 1).taggerSpeedPct == 100 && StepSetting(c, R::TaggerSpeed, -1).taggerSpeedPct == 120);   // unknown: like the other steppers
    c.taggerSpeedPct = 110;
    CHECK(SettingsFor(c) == (std::vector<R>{R::Limit, R::Respawn, R::Fuse, R::TaggerSpeed, R::Damage, R::DeadWeight, R::WeaponWeight, R::Sync}));
    for (int i = 0; i < kFormatCount; ++i) {
        if (Format(i) == Format::Tag) continue;
        MatchConfig m; m.format = Format(i);
        for (R r : SettingsFor(m)) CHECK(r != R::TaggerSpeed);                              // Tag only
    }
    LobbyLocal l = InLobby(true, Format::Tag);
    const SettingRow row = Need(BuildLobbyScene(l), R::TaggerSpeed);
    CHECK(row.label == "Tagger speed" && row.value == "110%" && row.enabled && row.hint.empty());
    l.config.taggerSpeedPct = 120;
    const SettingRow fast = Need(BuildLobbyScene(l), R::TaggerSpeed);
    CHECK(fast.value == "120%" && fast.hint.empty());
    l.config.taggerSpeedPct = 100;
    const SettingRow stock = Need(BuildLobbyScene(l), R::TaggerSpeed);
    CHECK(stock.value == "100%" && stock.hint.empty());
    CHECK(!Need(BuildLobbyScene(InLobby(false, Format::Tag)), R::TaggerSpeed).enabled);
    const MatchConfig only = StepSetting(c, R::TaggerSpeed, 1);
    CHECK(only.fuseS == c.fuseS && only.scoreLimit == c.scoreLimit && only.damagePct == c.damagePct);
}

// The Duel card is Last Man Standing for 2 or more (the host's count), and reads Duel with exactly two (hsm_mode_merge.h). Duel
// itself is no card of its own, and a Duel config of an older host is on the same card.
static void TestModeAvailability() {
    for (Format f : {Format::Duel, Format::LastManStanding}) {
        for (int n : {0, 1}) CHECK(!ModeAvailable(f, n).ok && std::string(ModeAvailable(f, n).hint) == "Needs 2 or more players");
        for (int n : {2, 3, 4, 8}) CHECK(ModeAvailable(f, n).ok && std::string(ModeAvailable(f, n).hint).empty());   // 3 players are no longer refused a Duel
    }
    for (Format f : {Format::FreeForAll, Format::TwoTeams, Format::PlayersVsBots, Format::Zombies})
        for (int n : {0, 1, 2, 5}) CHECK(ModeAvailable(f, n).ok);

    auto card = [](const LobbyScene& s, Format f) { for (const ModeCard& m : s.modes) if (m.format == f && !m.brawl && !m.progression && !m.blossfechten) return m; return ModeCard{}; };
    auto lobby = [](bool host, int n, Format f) {
        LobbyLocal l = InLobby(host, f);
        l.players.clear();
        for (int i = 0; i < n; ++i) l.players.push_back(Player(i ? "P" : "Ann", i == 0, i == (host ? 0 : n - 1)));
        return l;
    };
    LobbyScene one = BuildLobbyScene(lobby(true, 1, Format::FreeForAll));
    CHECK(!card(one, Format::LastManStanding).enabled && card(one, Format::LastManStanding).hint == "Needs 2 or more players");
    CHECK(card(one, Format::LastManStanding).name == "LAST MAN STANDING" && card(one, Format::Duel).name.empty());   // no card of Duel's own
    CHECK(card(one, Format::FreeForAll).enabled && card(one, Format::FreeForAll).hint.empty() && card(one, Format::TwoTeams).enabled);
    LobbyScene two = BuildLobbyScene(lobby(true, 2, Format::FreeForAll));
    CHECK(card(two, Format::LastManStanding).enabled && card(two, Format::LastManStanding).hint.empty() && card(two, Format::LastManStanding).name == "DUEL");   // two: it reads Duel
    LobbyScene three = BuildLobbyScene(lobby(true, 3, Format::FreeForAll));
    CHECK(card(three, Format::LastManStanding).enabled && card(three, Format::LastManStanding).hint.empty() && card(three, Format::LastManStanding).name == "LAST MAN STANDING");
    LobbyScene member = BuildLobbyScene(lobby(false, 2, Format::FreeForAll));
    CHECK(!card(member, Format::LastManStanding).enabled && card(member, Format::LastManStanding).hint.empty());     // a member's cards are off, with no hint to read into
    // The server seat is no player: playerCount overrides the roster's size.
    LobbyLocal srv = lobby(true, 3, Format::FreeForAll); srv.playerCount = 2;
    CHECK(card(BuildLobbyScene(srv), Format::LastManStanding).enabled && card(BuildLobbyScene(srv), Format::LastManStanding).name == "DUEL");
    srv.playerCount = 1;
    CHECK(!card(BuildLobbyScene(srv), Format::LastManStanding).enabled);
    // START: the Duel card (Duel or Last Man Standing) needs two or more; the waiting line says why.
    LobbyScene d1 = BuildLobbyScene(lobby(true, 1, Format::Duel));
    CHECK(!d1.start && d1.waiting == "Needs 2 or more players." && d1.startWhy == "Needs 2 or more players. Invite someone or pick another mode." && d1.modes[1].selected);
    CHECK(BuildLobbyScene(lobby(true, 2, Format::Duel)).start && BuildLobbyScene(lobby(true, 2, Format::Duel)).waiting.empty());
    CHECK(BuildLobbyScene(lobby(true, 3, Format::Duel)).start && BuildLobbyScene(lobby(true, 3, Format::Duel)).modes[1].selected);   // three: Last man standing, it starts
    LobbyScene l1 = BuildLobbyScene(lobby(true, 1, Format::LastManStanding));
    CHECK(!l1.start && l1.waiting == "Needs 2 or more players." && l1.startWhy == "Needs 2 or more players. Invite someone or pick another mode.");
    CHECK(BuildLobbyScene(lobby(true, 3, Format::LastManStanding)).start && BuildLobbyScene(lobby(true, 4, Format::LastManStanding)).start);
    CHECK(BuildLobbyScene(lobby(true, 1, Format::FreeForAll)).start);                          // alone in free-for-all: as before
    LobbyLocal prog = lobby(true, 1, Format::Duel); prog.progression = true;
    CHECK(BuildLobbyScene(prog).start);                                                         // Progression ignores the mode
    CHECK(BuildLobbyScene(lobby(false, 1, Format::Duel)).waiting == "Waiting for the host to start.");   // a member waits for the host, whatever the mode
}

// A third player joins a lobby set to the Duel card: the card keeps its one id and its settings, the match it starts as is
// Last Man Standing (FormatForPlayers), with the round count and the gear rows kept. (It was a switch to Last Man Standing.)
static void TestDuelOutgrown() {
    MatchConfig d; d.format = Format::Duel; d.scoreLimit = 5; d.gear = Gear::Mirror; d.tier = 4; d.timeLimitS = 300; d.respawnDelayS = 10; d.damagePct = 150;
    for (int n : {2}) CHECK(FormatForPlayers(d.format, n) == Format::Duel);
    for (int n : {0, 1, 3, 4, 8}) CHECK(FormatForPlayers(d.format, n) == Format::LastManStanding);
    for (Format f : {Format::FreeForAll, Format::TwoTeams, Format::PlayersVsBots, Format::Zombies}) CHECK(FormatForPlayers(f, 5) == f && FormatForPlayers(f, 2) == f);
    // The lobby itself holds Last Man Standing and keeps every setting while the roster changes.
    const MatchConfig held = LobbyConfigOf(d);
    CHECK(held.format == Format::LastManStanding && held.scoreLimit == 5 && held.gear == Gear::Mirror && held.tier == 4 && held.timeLimitS == 300 && held.respawnDelayS == 10 && held.damagePct == 150);
    for (int n : {1, 2, 3, 8}) { MatchConfig started = held; started.format = FormatForPlayers(started.format, n); CHECK(started.scoreLimit == 5 && started.gear == Gear::Mirror && started.damagePct == 150); }
    CHECK(FormatForPlayers(held.format, 2) == Format::Duel && FormatForPlayers(held.format, 3) == Format::LastManStanding);   // back to two: a Duel again, the host picks nothing
}


// Capture the flag was removed in 0.8.0: no card, no browser chip, no rows; a stale Format 10 (an old lobby) plays Teams.
// Forest, once its only map, is an arena like the others and is offered to every arena format.
static void TestCaptureTheFlagRemoved() {
    CHECK(uint8_t(Format::CaptureTheFlag) == 10 && kFormatCount == 20);              // the number stays taken
    LobbyScene s = BuildLobbyScene(InLobby(true, Format::TwoTeams));
    for (const ModeCard& m : s.modes) CHECK(m.format != Format::CaptureTheFlag && m.name != "CAPTURE THE FLAG");
    for (size_t i = 0; i < kModeOrderCount; ++i) CHECK(kModeOrder[i].format != Format::CaptureTheFlag);
    for (const std::string& n : BrowserModes()) CHECK(n != "Capture the flag");
    MatchConfig old; old.format = Format::CaptureTheFlag; old.teams = 4; old.scoreLimit = 7;
    const MatchConfig ok = Sanitize(old);
    CHECK(ok.format == Format::TwoTeams && ok.teams == 2 && ok.scoreLimit == 7);
    LobbyLocal stale = InLobby(true, Format::CaptureTheFlag);
    stale.config = Sanitize(stale.config);
    s = BuildLobbyScene(stale);
    const ModeCard* sel = nullptr;
    for (const ModeCard& m : s.modes) if (m.selected) sel = &m;
    CHECK(sel && sel->format == Format::TwoTeams);
    // Forest: every arena format offers it, tagged as an arena; Zombies and Pit bridge duels keep their own maps.
    const size_t forest = IndexOf("Forest");
    CHECK(forest < MapCount());
    if (forest >= MapCount()) return;
    for (Format f : {Format::FreeForAll, Format::Duel, Format::LastManStanding, Format::TwoTeams, Format::TeamDuel, Format::Riot, Format::Juggernaut,
                     Format::Tag, Format::Infection, Format::CrossbowTDM, Format::Tournament, Format::PlayersVsBots}) {
        s = BuildLobbyScene(InLobby(true, f));
        CHECK(Card(s, forest) && Card(s, forest)->allowed && Card(s, forest)->enabled && Card(s, forest)->reason.empty() && Card(s, forest)->tag == "Arena");
    }
    LobbyLocal m = InLobby(true, Format::Duel);
    m.mapPick = "/Game/Maps/Arenas/Map_Arena_Ambush_Test";
    s = BuildLobbyScene(m);
    CHECK(Card(s, forest)->selected && s.summary.find("Forest") != std::string::npos);
    s = BuildLobbyScene(InLobby(true, Format::Zombies));
    CHECK(!Card(s, forest)->allowed && Card(s, forest)->reason == "Zombies needs the Abyss");
}

// The region chip, the region on a browser row, the picker's button text, how far Steam is asked to look, and the host's view
// of a player's Steam name beside a nickname.
static void TestRegion() {
    LobbyLocal l; l.phase = LobbyPhase::Idle; l.version = "0.2.0";
    LobbyFound eu; eu.name = "EU"; eu.players = 2; eu.compatible = true; eu.region = Region::Europe; eu.map = "Yard"; eu.mode = "Duel";
    LobbyFound us; us.name = "US"; us.players = 1; us.compatible = true; us.region = Region::NaEast;
    LobbyFound none; none.name = "None"; none.players = 3; none.compatible = true;
    l.found = {eu, us, none};

    // The chip leads the row, starts on Any region, and steps through My region and the six regions back to any.
    LobbyScene s = BuildLobbyScene(l);
    CHECK(kRegionChip == 0 && s.filters.size() == kFilterChips && kFilterChips == 6);
    CHECK(s.filters[kRegionChip].label == "Any region" && !s.filters[kRegionChip].on);
    CHECK(BrowserFilter{}.region == kRegionAny && kRegionAny == 0 && kRegionMine == 1);
    BrowserFilter f;
    std::vector<std::string> seen;
    for (size_t i = 0; i < size_t(kRegionFilterStates); ++i) { seen.push_back(FilterChips(f)[kRegionChip].label); f = ToggleFilter(f, kRegionChip); }
    CHECK(f.region == 0 && seen.size() == 8);
    CHECK(seen[0] == "Any region" && seen[1] == "My region" && seen[2] == "NA East" && seen[3] == "NA West" && seen[4] == "South America");
    CHECK(seen[5] == "Europe" && seen[6] == "Asia" && seen[7] == "Oceania");
    CHECK(ToggleFilter(BrowserFilter{}, kRegionChip).region == kRegionMine && FilterChips(ToggleFilter(BrowserFilter{}, kRegionChip))[kRegionChip].on);
    CHECK(!FilterChips(BrowserFilter{})[kRegionChip].on);
    CHECK(std::string(RegionFilterLabel(kRegionFilterStates)) == "Any region");                // out of range: any
    CHECK(RegionFilterAt(0) == Region::Auto && RegionFilterAt(1) == Region::Auto && RegionFilterAt(2) == Region::NaEast);
    CHECK(RegionFilterAt(7) == Region::Oceania && RegionFilterAt(kRegionFilterStates) == Region::Auto);
    // My region names the region it means once that is known: "My region (NA East)".
    CHECK(RegionFilterLabel(kRegionMine, Region::NaEast) == "My region (NA East)" && RegionFilterLabel(kRegionMine, Region::Auto) == "My region");
    CHECK(RegionFilterLabel(kRegionAny, Region::NaEast) == "Any region" && RegionFilterLabel(5, Region::NaEast) == "Europe");
    BrowserFilter named; named.region = kRegionMine; named.mine = Region::Europe;
    CHECK(FilterChips(named)[kRegionChip].label == "My region (Europe)");

    // How far Steam looks: only my region keeps Steam's own idea of near.
    BrowserFilter d;
    CHECK(DistanceFor(d) == LobbyDistance::Worldwide);
    d.region = kRegionMine; CHECK(DistanceFor(d) == LobbyDistance::Default);
    d.region = 5; CHECK(DistanceFor(d) == LobbyDistance::Worldwide);

    // Any region (the start): all of them, whatever my region is. My region: a lobby of another region is hidden, a lobby that
    // did not say stays, and with no region of mine nothing is hidden.
    l.filter = {}; l.filter.mine = Region::Europe;
    CHECK(BuildLobbyScene(l).browser.size() == 3);
    l.filter.region = kRegionMine;
    s = BuildLobbyScene(l);
    CHECK(s.browser.size() == 2 && s.browser[0].name == "EU" && s.browser[1].name == "None");
    CHECK(s.browserCount == "2 of 3 lobbies shown");
    l.filter.mine = Region::Auto;
    CHECK(BuildLobbyScene(l).browser.size() == 3);
    // One region: only the lobbies that say they are in it, whatever my own region is.
    l.filter.region = 2;                                                                       // NA East
    s = BuildLobbyScene(l);
    CHECK(s.browser.size() == 1 && s.browser[0].name == "US" && s.filters[kRegionChip].label == "NA East" && s.filters[kRegionChip].on);
    l.filter.region = 5;                                                                       // Europe
    s = BuildLobbyScene(l);
    CHECK(s.browser.size() == 1 && s.browser[0].name == "EU");
    l.filter.region = 3;                                                                       // NA West: none
    s = BuildLobbyScene(l);
    CHECK(s.browser.empty() && s.browserEmpty == "3 lobbies hidden by your filters (NA West).");
    // The other chips still work with it.
    l.filter = {}; l.filter.region = 5; l.filter.hasRoom = true;
    CHECK(BuildLobbyScene(l).browser.size() == 1);
    l.filter.mode = uint8_t(std::find(BrowserModes().begin(), BrowserModes().end(), "Carnage") - BrowserModes().begin());
    CHECK(BuildLobbyScene(l).browser.empty());

    // The row names the region first, then the map, the mode: "Europe · Yard · Duel". No region said: as before.
    l.filter = {};
    s = BuildLobbyScene(l);
    CHECK(s.browser.size() == 3);
    if (s.browser.size() == 3) {
        CHECK(s.browser[0].detail == "Europe " + kDot + " Yard " + kDot + " Duel");
        CHECK(s.browser[1].detail == "NA East");
        CHECK(s.browser[2].detail.empty());
    }

    // The picker's button text.
    l.regionPick = Region::Auto; l.regionGuess = Region::Auto;
    CHECK(BuildLobbyScene(l).regionButton == "Auto (unknown)");
    l.regionGuess = Region::Asia;
    CHECK(BuildLobbyScene(l).regionButton == "Auto (Asia)");
    l.regionPick = Region::Oceania;
    CHECK(BuildLobbyScene(l).regionButton == "Oceania");

    // The host sees a player's Steam name beside their nickname; a member does not, and neither does the host beside their own row.
    LobbyLocal h = InLobby(true);
    LobbyPlayer ann = Player("Ann", true, true); ann.steamName = "AnnSteam";
    LobbyPlayer bob = Player("Bobby"); bob.steamName = "bob_steam";
    LobbyPlayer cy = Player("Cy"); cy.steamName = "Cy";                                         // the same name: nothing to add
    LobbyPlayer di = Player("Di");                                                              // no Steam name known
    h.players = {ann, bob, cy, di};
    LobbyScene hs = BuildLobbyScene(h);
    CHECK(hs.roster.size() == 4);
    if (hs.roster.size() == 4) {
        CHECK(hs.roster[0].steam.empty() && hs.roster[1].steam == "bob_steam" && hs.roster[2].steam.empty() && hs.roster[3].steam.empty());
    }
    LobbyLocal m = InLobby(false);
    m.players = {ann, bob};
    m.players[0].me = false; m.players[1].me = true;
    LobbyScene ms = BuildLobbyScene(m);
    CHECK(ms.roster.size() == 2);
    if (ms.roster.size() == 2) CHECK(ms.roster[0].steam.empty() && ms.roster[1].steam.empty());
}

// The empty list names the chips that hide the lobbies found. The region chip says "My region" plain, not with the region in brackets.
static void TestHiddenByFilters() {
    LobbyFound us; us.name = "US"; us.region = Region::NaEast; us.compatible = true;
    LobbyFound eu; eu.name = "EU"; eu.region = Region::Europe; eu.compatible = true; eu.locked = true;
    BrowserFilter f; f.mine = Region::Europe; f.region = kRegionMine;
    CHECK(HidingChips(us, f) == 1u << kRegionChip && HidingChips(eu, f) == 0);
    CHECK(HidingChips(us, BrowserFilter{}) == 0);                                   // no region of mine: nothing hidden
    CHECK(HiddenByFiltersText({us}, f) == "1 lobby hidden by your filters (My region).");
    f.noPassword = true;
    CHECK(HidingChips(eu, f) == 1u << kPasswordChip);
    CHECK(HiddenByFiltersText({us, eu}, f) == "2 lobbies hidden by your filters (My region, No password).");   // each lobby's chips
    CHECK(HiddenByFiltersText({eu, us}, f) == "2 lobbies hidden by your filters (My region, No password).");
    LobbyFound full = us; full.region = Region::Europe; full.players = 8; full.locked = true;
    f.hasRoom = true; f.mostPlayers = true;
    CHECK(HidingChips(full, f) == ((1u << kRoomChip) | (1u << kPasswordChip)));                         // both of its chips; sorting never hides
    BrowserFilter one; one.region = 3;                                               // NA West asked for
    CHECK(HidingChips(eu, one) == 1u << kRegionChip && HidingChips(us, one) == 1u << kRegionChip);
    CHECK(HiddenByFiltersText({eu}, one) == "1 lobby hidden by your filters (NA West).");
    CHECK(HiddenByFiltersText({eu}, BrowserFilter{}) == "1 lobby hidden by your filters.");   // never shown: no chip hides it
}

// PR4: the sub-tab each row sits in, and the order inside it, are the approved mock's (tools/ui_mock/lobby.html SECS), written out
// here row by row. FreeCam and Scenario are gone from the lobby (0.9.0). Tier is the Crypts card's Starting tier, a Gear row.
static void TestSectionTable() {
    using R = LobbyRow;
    struct Sec { LobbySection s; std::vector<R> rows; };
    const std::vector<Sec> mock = {
        {LobbySection::Match, {R::Players, R::Battle, R::Gather, R::CryptsRun, R::CryptsTier, R::FightMap, R::Limit, R::Time, R::WinBy, R::Respawn, R::Fuse, R::TaggerSpeed, R::ClassPower}},
        {LobbySection::Combat, {R::Damage, R::DeadWeight, R::WeaponWeight, R::Bite}},
        {LobbySection::Teams, {R::Teams, R::FriendlyFire, R::MyTeam, R::TeamKit, R::Shuffle}},
        {LobbySection::Gear, {R::Gear, R::Tier, R::Bodies, R::Weapons, R::Crossbows, R::Strip, R::KeepKit}},
        {LobbySection::World, {R::TimeOfDay, R::WaveBreak, R::ClearGround, R::Props, R::Sync}},
    };
    CHECK(mock.size() == kSectionCount);
    size_t listed = 0;
    for (const Sec& sec : mock) {
        for (size_t i = 0; i < sec.rows.size(); ++i) {
            ++listed;
            CHECK(SectionOf(sec.rows[i]) == sec.s);
            if (i) CHECK(size_t(sec.rows[i - 1]) < size_t(sec.rows[i]));           // the enum is the display order
        }
        if (sec.s != LobbySection::World) {                                          // one section after the other: the whole enum reads in section order
            CHECK(size_t(sec.rows.back()) + 1 == size_t(mock[size_t(sec.s) + 1].rows.front()));
        }
    }
    CHECK(listed == size_t(R::Sync) + 1);                                            // every row is in exactly one list
    CHECK(R::Players == R(0) && R::Sync == R(listed - 1));
    // The special rows are ordinary scene rows now, in the section they belong to.
    CHECK(SectionOf(R::Players) == LobbySection::Match && SectionOf(R::WinBy) == LobbySection::Match && SectionOf(R::ClassPower) == LobbySection::Match);
    CHECK(SectionOf(R::Strip) == LobbySection::Gear && SectionOf(R::KeepKit) == LobbySection::Gear);
    CHECK(SectionOf(R::Props) == LobbySection::World && SectionOf(R::TimeOfDay) == LobbySection::World);
    CHECK(SectionOf(R::Damage) == LobbySection::Combat && SectionOf(R::DeadWeight) == LobbySection::Combat && SectionOf(R::WeaponWeight) == LobbySection::Combat);
    // A scene lists its rows section by section.
    for (Format f : {Format::FreeForAll, Format::TwoTeams, Format::Duel, Format::PlayersVsBots, Format::Tag}) {
        const LobbyScene sc = BuildLobbyScene(InLobby(true, f));
        for (size_t i = 1; i < sc.settings.size(); ++i) {
            CHECK(SectionOf(sc.settings[i - 1].id) <= SectionOf(sc.settings[i].id));
            CHECK(size_t(sc.settings[i - 1].id) < size_t(sc.settings[i].id));
        }
    }
}

// PR4: the tier chips show under Gear only while the tier is used: random or identical gear, the bot formats, Crypts. Own kits and the
// host's kit have none. Each format and gear is checked against that rule, written out without TierInUse.
static void TestTierChipVisibility() {
    for (int i = 0; i < kFormatCount; ++i) {
        const Format f = Format(i);
        for (Gear g : {Gear::Own, Gear::RandomTier, Gear::Mirror, Gear::HostKit}) {
            const LobbyScene sc = BuildLobbyScene(InLobby(true, f, g));
            const SettingRow* gear = RowOf(sc, LobbyRow::Gear);
            if (!gear) { CHECK(RowOf(sc, LobbyRow::Tier) == nullptr); continue; }          // the formats that deal the gear by role
            const bool bots = f == Format::PlayersVsBots || f == Format::Zombies;
            const bool rolled = g == Gear::RandomTier || g == Gear::Mirror;
            CHECK(gear->tierChips.empty() == !(bots || rolled));
            CHECK(RowOf(sc, LobbyRow::Tier) == nullptr);                                    // never a second row outside Crypts
            if (!gear->tierChips.empty()) CHECK(gear->tierChips.size() == 8 && gear->kind == RowKind::Choice && !RowHasArrows(*gear));   // the chips come with the list (Gear is a dropdown: no arrows)
        }
    }
    // The chips carry the eight tiers, one on.
    const SettingRow tiers = Need(BuildLobbyScene(InLobby(true, Format::Duel, Gear::RandomTier)), LobbyRow::Gear);
    int on = 0; for (const RowChip& c : tiers.tierChips) on += c.on;
    CHECK(on == 1 && tiers.tierChips.front().label == "Beggar" && tiers.tierChips.back().label == "Knight");
    // A stale tier value reads as the top chip.
    CHECK(TierChips(200)[7].on && !TierChips(200)[0].on && TierChips(0)[0].on);
    // A click on chip i sets the tier to i, whatever the tier was; the rest of the settings stay.
    MatchConfig c; c.gear = Gear::RandomTier; c.tier = 5; c.damagePct = 150;
    for (size_t i = 0; i < 8; ++i) CHECK(WithTier(c, i).tier == i && WithTier(c, i).damagePct == 150 && WithTier(c, i).gear == Gear::RandomTier);
    CHECK(WithTier(c, 99).tier == 7);
    // Arrows: the Gear row keeps them beside its chips; a row of tick chips (Weapons) has none.
    SettingRow r; r.id = LobbyRow::Tier; CHECK(RowHasArrows(r));
    r.tierChips = TierChips(1); CHECK(!RowHasArrows(r));                                 // the Crypts card's Starting tier: chips alone
    r.id = LobbyRow::Gear; CHECK(RowHasArrows(r));
    r.chips = {{"Any", true}}; CHECK(!RowHasArrows(r));
}

// PR4: stepping Gear off the rolled gear drops the tier a host picked, so the Same for all build is not set by a tier nobody can see.
// The tier is kept while it stays in use.
static void TestGearStepResetsHiddenTier() {
    MatchConfig c; c.format = Format::FreeForAll; c.gear = Gear::Own; c.tier = 6;
    c = StepSetting(c, LobbyRow::Gear, 1);
    CHECK(c.gear == Gear::RandomTier && c.tier == 6);                                    // chips shown: the tier stays
    c = StepSetting(c, LobbyRow::Gear, 1);
    CHECK(c.gear == Gear::Mirror && c.tier == 6);
    const uint8_t keep = c.tier;
    c = StepSetting(c, LobbyRow::Gear, 1);
    CHECK(!TierInUse(c) && c.tier == MatchConfig{}.tier);                                // the chips are gone: the default tier again
    CHECK(keep == 6);
    MatchConfig bots; bots.format = Format::PlayersVsBots; bots.tier = 6;
    bots = StepSetting(bots, LobbyRow::Gear, 1);
    CHECK(TierInUse(bots) && bots.tier == 6);                                            // bots start at the tier whatever the gear
    // Stepping backwards does the same.
    MatchConfig back; back.format = Format::FreeForAll; back.gear = Gear::Mirror; back.tier = 6;
    back = StepSetting(back, LobbyRow::Gear, -1);
    CHECK(back.gear == Gear::RandomTier && back.tier == 6);
    back = StepSetting(back, LobbyRow::Gear, -1);
    CHECK(!TierInUse(back) && back.tier == MatchConfig{}.tier);
}

// PR4: the host pill adds the region once it is known ("Hosting · public · NA East"), the picked one or the guess.
static void TestPillRegion() {
    LobbyLocal h = InLobby(true);
    h.publicLobby = true;
    CHECK(BuildLobbyScene(h).pill.text == "Hosting " + kDot + " public");                // no region known: as before
    h.regionPick = Region::NaEast;
    CHECK(BuildLobbyScene(h).pill.text == "Hosting " + kDot + " public " + kDot + " NA East");
    h.publicLobby = false; h.regionPick = Region::Europe;
    CHECK(BuildLobbyScene(h).pill.text == "Hosting " + kDot + " friends " + kDot + " Europe");
    h.regionPick = Region::Auto; h.regionGuess = Region::Asia;
    CHECK(BuildLobbyScene(h).pill.text == "Hosting " + kDot + " friends " + kDot + " Asia");   // Auto: the guess
    LobbyLocal m = InLobby(false);
    m.regionPick = Region::Europe;
    CHECK(BuildLobbyScene(m).pill.text.find("Hosting") == std::string::npos);             // a member's pill says nothing of hosting
}

int main() {
    TestPrevNext();
    TestStepSetting();
    TestNewRows();
    TestWeaponsRow();
    TestCrossbowsRow();
    TestTeamPick();
    TestSelectFormat();
    TestPill();
    TestHeader();
    TestBrowser();
    TestPlayerLimit();
    TestBrowserPack();
    TestPasswordFilter();
    TestHostPassword();
    TestTeamsRow();
    TestTeamKitRow();
    TestTierNames();
    TestShuffleRow();
    TestTaggerSpeedRow();
    TestModeAvailability();
    TestDuelOutgrown();
    TestOutOfLobby();
    TestHostAgainstMember();
    TestRowsPerFormat();
    TestLabelsAndValues();
    TestGearHint(); TestHostKitRows(); TestBodiesRow();
    TestOtherHints();
    TestMyTeam();
    TestRoster();
    TestReady();
    TestModes();
    TestBrawl();
    TestMaps();
    TestModdedMaps();
    TestBuildMismatch();
    TestDamage();
    TestWeights();
    TestGroupsAndSections();
    TestClassLine();
    TestSummary();
    TestFilters();
    TestCarnageName();
    TestRegion();
    TestHiddenByFilters();
    TestCaptureTheFlagRemoved();
    TestSectionTable(); TestTierChipVisibility(); TestGearStepResetsHiddenTier(); TestPillRegion();
    if (g_fail) { std::printf("%d lobby scene checks failed\n", g_fail); return 1; }
    std::printf("all lobby scene tests passed\n");
    return 0;
}
