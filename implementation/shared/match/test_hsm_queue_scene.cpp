// Tournament and Pit Bridge Duels on screen: the mode cards, availability, the rows and their keys, the maps, the HUD line under
// the strip, the pill, the bench panel and the scoreboard (eliminated players dimmed).
#include "match/hsm_hud_scene.h"
#include "match/hsm_lobby_scene.h"
#include <cstdio>

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++g_fail; } } while (0)
using namespace hsm;
using V = std::vector<uint8_t>;

static LobbyPlayer Player(const char* name, bool host, bool me) { LobbyPlayer p; p.name = name; p.host = host; p.me = me; return p; }
static LobbyLocal InLobby(Format f, int players = 4) {
    LobbyLocal l;
    l.phase = LobbyPhase::InLobby; l.host = true; l.version = "0.2.0"; l.myName = "Ann";
    l.config.format = f; l.config.scoreLimit = DefaultScoreLimit(f);
    l.players = {Player("Ann", true, true)};
    for (int i = 1; i < players; ++i) l.players.push_back(Player("P", false, false));
    return l;
}
static std::vector<LobbyRow> Ids(const LobbyScene& s) { std::vector<LobbyRow> v; for (const SettingRow& r : s.settings) v.push_back(r.id); return v; }
static const SettingRow* RowOf(const LobbyScene& s, LobbyRow id) { for (const SettingRow& r : s.settings) if (r.id == id) return &r; return nullptr; }

static void TestCards() {
    LobbyScene s = BuildLobbyScene(InLobby(Format::Tournament));
    CHECK(s.modes.size() == kModeCardCount);
    const ModeCard *t = nullptr, *p = nullptr;
    for (const ModeCard& m : s.modes) { if (m.format == Format::Tournament && !m.brawl) t = &m; if (m.format == Format::PitBridgeDuel) p = &m; }
    CHECK(t && p);
    CHECK(t->name == "TOURNAMENT" && t->selected && t->enabled && t->group == ModeGroup::Versus && !t->description.empty());
    CHECK(p->name == "PIT BRIDGE DUELS" && !p->selected && p->enabled && p->group == ModeGroup::Versus && !p->description.empty());
    CHECK(std::string(FormatBlurb(Format::Tournament)) != FormatBlurb(Format::FreeForAll) && std::string(FormatBlurb(Format::PitBridgeDuel)) != FormatBlurb(Format::FreeForAll));
    // the cards sit in the versus part, before the co-op ones
    size_t at = 0, bots = 0;
    for (size_t i = 0; i < kModeOrderCount; ++i) { if (kModeOrder[i] == Format::Tournament) at = i; if (kModeOrder[i] == Format::PlayersVsBots) bots = i; }
    CHECK(at > 0 && at < bots && ModeGroupAt(at) == ModeGroup::Versus);
    for (Format f : {Format::Tournament, Format::PitBridgeDuel}) { bool in = false; for (Format k : kModeOrder) in = in || k == f; CHECK(in); }
    CHECK(FormatName(Format::Tournament) == "Tournament" && FormatName(Format::PitBridgeDuel) == "Pit bridge duels");
    CHECK(ModeName(Format::Tournament, Gear::Own) == "Tournament" && ModeName(uint8_t(Format::PitBridgeDuel), 0) == "Pit bridge duels");
}

static void TestAvailability() {
    CHECK(!ModeAvailable(Format::Tournament, 2).ok && std::string(ModeAvailable(Format::Tournament, 2).hint) == "Needs 3 or more players");
    CHECK(ModeAvailable(Format::Tournament, 3).ok && ModeAvailable(Format::Tournament, 8).ok);
    CHECK(!ModeAvailable(Format::PitBridgeDuel, 1).ok && ModeAvailable(Format::PitBridgeDuel, 2).ok && ModeAvailable(Format::PitBridgeDuel, 8).ok);
    LobbyScene two = BuildLobbyScene(InLobby(Format::Tournament, 2));
    for (const ModeCard& m : two.modes) if (m.format == Format::Tournament && !m.brawl) CHECK(!m.enabled && std::string(m.hint) == "Needs 3 or more players");
    CHECK(!two.start && two.waiting == "Needs 3 or more players");
    LobbyScene pit2 = BuildLobbyScene(InLobby(Format::PitBridgeDuel, 2));
    CHECK(pit2.start);
    LobbyScene pit1 = BuildLobbyScene(InLobby(Format::PitBridgeDuel, 1));
    CHECK(!pit1.start);
}

static void TestRows() {
    using R = LobbyRow;
    const LobbyScene t = BuildLobbyScene(InLobby(Format::Tournament));
    // no limit (one round a fight) and no time limit (a bracket has no clock); the rest as every round format
    CHECK((Ids(t) == std::vector<LobbyRow>{R::Respawn, R::FreeCam, R::Gear, R::Tier, R::Damage, R::DeadWeight, R::WeaponWeight, R::Crossbows, R::Sync}));
    CHECK(RowOf(t, R::Limit) == nullptr);
    bool oneRound = false;
    for (const ModeCard& m : t.modes) if (m.format == Format::Tournament) oneRound = m.description.find("One round a fight") != std::string::npos;
    CHECK(oneRound);
    const LobbyScene p = BuildLobbyScene(InLobby(Format::PitBridgeDuel));
    CHECK((Ids(p) == std::vector<LobbyRow>{R::Limit, R::Time, R::Respawn, R::FreeCam, R::Gear, R::Tier, R::Damage, R::DeadWeight, R::WeaponWeight, R::Crossbows, R::Sync}));
    const SettingRow* win = RowOf(p, R::Limit);
    CHECK(win && win->label == "Wins to take the match" && win->value == "5");
    // random gear adds the Weapons row, as everywhere
    LobbyLocal g = InLobby(Format::Tournament); g.config.gear = Gear::RandomTier;
    CHECK(RowOf(BuildLobbyScene(g), R::Weapons) != nullptr);
    // the steppers: a Tournament limit only steps to 1 (one round a fight); wins 3, 5, 7, 10, 15
    MatchConfig c; c.format = Format::Tournament; c.scoreLimit = 2;
    c = StepSetting(c, R::Limit, 1); CHECK(c.scoreLimit == 1 && LimitToKey(c.format, c.scoreLimit) == 1);
    c = StepSetting(c, R::Limit, -1); CHECK(c.scoreLimit == 1);
    CHECK(LimitToKey(Format::Tournament, 2) == 3 && LimitFromKey(Format::Tournament, 3) == 2);   // an old lobby key still reads
    MatchConfig w; w.format = Format::PitBridgeDuel; w.scoreLimit = 5;
    w = StepSetting(w, R::Limit, 1); CHECK(w.scoreLimit == 7);
    w = StepSetting(w, R::Limit, 1); w = StepSetting(w, R::Limit, 1); CHECK(w.scoreLimit == 15);
    w = StepSetting(w, R::Limit, 1); CHECK(w.scoreLimit == 3);
    // picking a card starts at its own default
    MatchConfig d; d.format = Format::FreeForAll; d.scoreLimit = 20;
    CHECK(SelectMode(d, Format::Tournament, false).scoreLimit == 1 && SelectMode(d, Format::PitBridgeDuel, false).scoreLimit == 5);
}

static void TestMaps() {
    const MapInfo* pit = FindMap(kPitMapPath);
    CHECK(pit != nullptr && MapAllowsFormat(*pit, Format::PitBridgeDuel) && MapAllowsFormat(*pit, Format::Tournament));
    for (const MapInfo& m : kMaps) {
        CHECK(MapAllowsFormat(m, Format::PitBridgeDuel) == (&m == pit));
        CHECK(MapAllowsFormat(m, Format::Tournament) == m.arena);
    }
    CHECK(EffectiveMap(Format::PitBridgeDuel, "/Game/Maps/Arenas/Map_Arena_Alley") == size_t(pit - kMaps));   // any other pick falls to the Pit
    CHECK(EffectiveMap(Format::PitBridgeDuel, "") == size_t(pit - kMaps));
    LobbyScene s = BuildLobbyScene(InLobby(Format::PitBridgeDuel));
    int allowed = 0;
    for (const MapCard& m : s.maps) { allowed += m.allowed ? 1 : 0; CHECK(m.allowed == (m.name == "Pit")); if (!m.allowed) CHECK(m.reason == "Pit bridge duels need the Pit"); }
    CHECK(allowed == 1);
    LobbyLocal mod = InLobby(Format::PitBridgeDuel);
    LobbyModMap mm; mm.path = "/Game/Mods/X"; mm.display = "X"; mod.modMaps = {mm};
    for (const MapCard& m : BuildLobbyScene(mod).maps) if (m.modded) CHECK(!m.allowed);
    LobbyScene t = BuildLobbyScene(InLobby(Format::Tournament));
    for (const MapCard& m : t.maps) if (!m.allowed) CHECK(m.reason == "Tournament needs an arena");
    CHECK(BrowserModes().size() == 16 && BrowserModes()[11] == "Tournament" && BrowserModes()[12] == "Pit bridge duels");
    CHECK(ModeMatches("Tournament", BrowserModes()[11]) && ModeMatches("Pit bridge duels", BrowserModes()[12]));
}

// ---------------------------------------------------------------- the HUD

static MatchInfo Tourney(int players, int playedGames, uint8_t me = 1) {
    MatchInfo m;
    m.format = uint8_t(Format::Tournament); m.scoreLimit = 2; m.phase = uint8_t(MatchPhase::Live); m.round = 1; m.respawnDelayS = 5;
    Bracket b;
    V p; for (int i = 1; i <= players; ++i) p.push_back(uint8_t(i));
    b.Start(p, 4);
    Bracket::Game g;
    for (int i = 0; i < playedGames && b.Current(g); ++i) b.Report(g.a);
    m.queueOrder = b.seeds(); m.bracketWinners = b.GameWinners();
    const char* names[] = {"", "Ann", "Bo", "Cy", "Di", "Ed", "Flo", "Gus", "Hal"};
    for (int i = 1; i <= players; ++i) m.fighters.push_back({uint8_t(i), kNoTeam, 0, 0, names[i], 10, true});
    (void)me;
    return m;
}

static void TestBracketLine() {
    MatchInfo m = Tourney(8, 0);
    Bracket b; b.Restore(m.queueOrder, m.bracketWinners);
    Bracket::Game g, next;
    CHECK(b.Current(g) && b.NextGame(next));
    const char* names[] = {"", "Ann", "Bo", "Cy", "Di", "Ed", "Flo", "Gus", "Hal"};
    std::string want = std::string("Quarter-final 1/4: ") + names[g.a] + " vs " + names[g.b] + " - first to 2   Next: " + names[next.a] + " vs " + names[next.b];
    CHECK(QueueLine(m) == want);
    m.fightWinsA = 1;
    CHECK(QueueLine(m).find(" - first to 2 (1-0)   Next: ") != std::string::npos);
    CHECK(QueueStage(m) == "Quarter-final 1/4");
    // later: the final alone has no number and no next
    MatchInfo f = Tourney(4, 2);
    CHECK(QueueStage(f) == "Final" && QueueFinal(f));
    CHECK(QueueLine(f).rfind("Final: ", 0) == 0 && QueueLine(f).find("Next:") == std::string::npos);
    // a decided bracket says nothing; so does a match not running
    MatchInfo d = Tourney(2, 1);
    CHECK(QueueLine(d).empty() && QueueStage(d).empty() && !QueueFinal(d));
    MatchInfo idle = Tourney(4, 0); idle.phase = uint8_t(MatchPhase::Idle);
    CHECK(QueueLine(idle).empty());
    idle.phase = uint8_t(MatchPhase::Results);
    CHECK(QueueLine(idle).empty());
    // no bracket on the wire yet
    MatchInfo none = Tourney(4, 0); none.queueOrder.clear(); none.bracketWinners.clear();
    CHECK(QueueLine(none).empty());
}

static void TestPitLine() {
    MatchInfo m;
    m.format = uint8_t(Format::PitBridgeDuel); m.scoreLimit = 5; m.phase = uint8_t(MatchPhase::Live); m.round = 4;
    m.queueOrder = {2, 1, 3, 4}; m.streak = 3;
    m.fighters = {{1, kNoTeam, 0, 0, "Ann"}, {2, kNoTeam, 0, 0, "Bo"}, {3, kNoTeam, 0, 0, "Cy"}, {4, kNoTeam, 0, 0, "Di"}};
    CHECK(QueueLine(m) == "Bo: 3 in a row   Bo vs Ann   Up next: Cy");
    CHECK(QueueStage(m) == "Fight 4");
    m.streak = 0;
    CHECK(QueueLine(m) == "Bo vs Ann   Up next: Cy");
    m.queueOrder = {2, 1};
    CHECK(QueueLine(m) == "Bo vs Ann");
    m.queueOrder = {2};
    CHECK(QueueLine(m).empty());
}

static void TestRoles() {
    MatchInfo m = Tourney(4, 0);
    uint8_t a, b;
    CHECK(QueuePair(m, a, b) && a != b);
    int fighting = 0, waiting = 0, out = 0;
    for (uint8_t e = 1; e <= 4; ++e) {
        const QueueRole r = RoleInQueue(m, e);
        fighting += r == QueueRole::Fighting; waiting += r == QueueRole::Waiting; out += r == QueueRole::Out;
        CHECK((r == QueueRole::Fighting) == (e == a || e == b));
    }
    CHECK(fighting == 2 && waiting == 2 && out == 0);
    CHECK(RoleInQueue(m, 9) == QueueRole::Out);                          // a late joiner: not in this bracket
    MatchInfo after = Tourney(4, 1);                                     // one game played: its loser is out
    Bracket br; br.Restore(after.queueOrder, after.bracketWinners);
    int outs = 0;
    for (uint8_t e = 1; e <= 4; ++e) outs += RoleInQueue(after, e) == QueueRole::Out ? 1 : 0;
    CHECK(outs == 1);
    MatchInfo duel; duel.format = uint8_t(Format::Duel);
    CHECK(RoleInQueue(duel, 1) == QueueRole::None);
    MatchInfo pit; pit.format = uint8_t(Format::PitBridgeDuel); pit.queueOrder = {5, 6, 7};
    CHECK(RoleInQueue(pit, 5) == QueueRole::Fighting && RoleInQueue(pit, 6) == QueueRole::Fighting && RoleInQueue(pit, 7) == QueueRole::Waiting);
    CHECK(RoleInQueue(pit, 1) == QueueRole::Waiting);                    // a joiner the queue has not listed yet waits too
}

static LocalHud Me(uint8_t e) { LocalHud l; l.me = e; l.infoAtMs = 1000; return l; }

static void TestScene() {
    MatchInfo m = Tourney(4, 0);
    HudScene s = BuildHudScene(m, Me(1), 1000);
    CHECK(s.info.visible && s.info.text == QueueLine(m));
    CHECK(s.pill.text.rfind("SEMI-FINAL ", 0) == 0);
    // the final lights the pill
    HudScene fin = BuildHudScene(Tourney(4, 2), Me(1), 1000);
    CHECK(fin.pill.text == "FINAL" && fin.pill.color.r == style::kWarn.r && fin.pill.color.g == style::kWarn.g);
    // every other format has no line
    MatchInfo duel; duel.format = uint8_t(Format::Duel); duel.phase = uint8_t(MatchPhase::Live); duel.scoreLimit = 3; duel.round = 2;
    duel.fighters = {{1, kNoTeam, 0, 0, "a", 0, true}, {2, kNoTeam, 0, 0, "b", 0, true}};
    CHECK(!BuildHudScene(duel, Me(1), 1000).info.visible);
    // the pit
    MatchInfo pit;
    pit.format = uint8_t(Format::PitBridgeDuel); pit.scoreLimit = 5; pit.phase = uint8_t(MatchPhase::Live); pit.round = 4; pit.queueOrder = {2, 1, 3}; pit.streak = 2;
    pit.fighters = {{1, kNoTeam, 0, 0, "Ann", 0, true}, {2, kNoTeam, 0, 0, "Bo", 0, true}, {3, kNoTeam, 0, 0, "Cy", 0, false}};
    HudScene ps = BuildHudScene(pit, Me(1), 1000);
    CHECK(ps.info.visible && ps.info.text == "Bo: 2 in a row   Bo vs Ann   Up next: Cy" && ps.pill.text == "FIGHT 4");
    // results show no line
    pit.phase = uint8_t(MatchPhase::Results); pit.winner = 2; pit.timeLeftMs = 5000;
    CHECK(!BuildHudScene(pit, Me(1), 1000).info.visible);
}

static void TestBenchPanel() {
    // on the bench in the Pit: no body, so no "dead" flag needed
    MatchInfo pit;
    pit.format = uint8_t(Format::PitBridgeDuel); pit.scoreLimit = 5; pit.phase = uint8_t(MatchPhase::Live); pit.round = 2; pit.queueOrder = {2, 1, 3, 4, 5};
    pit.fighters = {{1, kNoTeam, 0, 0, "A", 0, true}, {2, kNoTeam, 0, 0, "B", 0, true}, {3, kNoTeam, 0, 0, "C", 0, false}, {4, kNoTeam, 0, 0, "D", 0, false}, {5, kNoTeam, 0, 0, "E", 0, false}};
    HudScene next = BuildHudScene(pit, Me(3), 1000);
    CHECK(next.dead.visible && next.dead.title == "ON THE BENCH" && next.dead.detail == "you fight next");
    HudScene later = BuildHudScene(pit, Me(4), 1000);
    CHECK(later.dead.visible && later.dead.detail == "#2 in line");
    CHECK(!BuildHudScene(pit, Me(1), 1000).dead.visible);                // fighting and alive: no panel
    // the bracket: knocked out
    MatchInfo t = Tourney(4, 1);
    uint8_t loser = 0;
    for (uint8_t e = 1; e <= 4; ++e) if (RoleInQueue(t, e) == QueueRole::Out) loser = e;
    CHECK(loser != 0);
    HudScene out = BuildHudScene(t, Me(loser), 1000);
    CHECK(out.dead.visible && out.dead.title == "OUT OF THE BRACKET");
    uint8_t waiting = 0;
    for (uint8_t e = 1; e <= 4; ++e) if (RoleInQueue(t, e) == QueueRole::Waiting) waiting = e;
    if (waiting) CHECK(BuildHudScene(t, Me(waiting), 1000).dead.title == "ON THE BENCH");
    // a countdown or results show no panel
    t.phase = uint8_t(MatchPhase::Countdown);
    CHECK(!BuildHudScene(t, Me(loser), 1000).dead.visible);
}

static void TestBoard() {
    MatchInfo t = Tourney(4, 1);
    t.scoreLimit = 2;
    for (FighterEntry& f : t.fighters) f.alive = false;                    // benched fighters are not "alive" in the match
    LocalHud l = Me(1); l.tab = true;
    HudScene s = BuildHudScene(t, l, 1000);
    CHECK(s.board.visible && s.board.title.find("TOURNAMENT") != std::string::npos && s.board.title.find("ONE ROUND A FIGHT") != std::string::npos && s.board.title.find("BEST OF") == std::string::npos);
    int out = 0, waiting = 0, plain = 0;
    for (const BoardRow& r : s.board.rows) {
        if (r.tag == "OUT") { ++out; CHECK(r.dead && r.ink.r == style::kIron.r); }
        else if (r.tag == "WAIT") { ++waiting; CHECK(!r.dead && r.ink.r == style::kBone.r); }
        else ++plain;
    }
    CHECK(out == 1 && out + waiting + plain == 4);                         // one beaten, never the waiting winners dimmed
    CHECK(waiting >= 1);
    // the Pit: waiting is not dead
    MatchInfo pit;
    pit.format = uint8_t(Format::PitBridgeDuel); pit.scoreLimit = 5; pit.phase = uint8_t(MatchPhase::Live); pit.queueOrder = {1, 2, 3};
    pit.fighters = {{1, kNoTeam, 0, 0, "A", 0, true}, {2, kNoTeam, 0, 0, "B", 0, true}, {3, kNoTeam, 0, 0, "C", 0, false}};
    HudScene ps = BuildHudScene(pit, l, 1000);
    CHECK(ps.board.visible && ps.board.title.find("FIRST TO 5") != std::string::npos);
    for (const BoardRow& r : ps.board.rows) CHECK(!r.dead);
}

int main() {
    TestCards();
    TestAvailability();
    TestRows();
    TestMaps();
    TestBracketLine();
    TestPitLine();
    TestRoles();
    TestScene();
    TestBenchPanel();
    TestBoard();
    if (g_fail) { std::printf("%d failure(s)\n", g_fail); return 1; }
    std::puts("ok");
    return 0;
}
