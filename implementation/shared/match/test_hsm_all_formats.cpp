// Every Format value 0..kFormatCount-1 against every table that has to know it. Formats arrived in separate branches (Capture the
// flag 10, Tag 11, Infection 12, Crossbow TDM 13, Tournament 14, Pit Bridge Duels 15), each adding its own lines to the name,
// blurb, card, map, availability and settings tables; a merge that drops one line leaves a format that plays but cannot be
// picked, named or explained. This iterates the enum, so a hole in any table fails here with the format's number.
#include "match/hsm_hud_text.h"
#include "match/hsm_lobby_scene.h"
#include "session/hsm_autoflow.h"
#include "session/hsm_migration.h"
#include <cstdio>
#include <set>
#include <string>

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++g_fail; } } while (0)
#define CHECKF(c, f) do { if (!(c)) { std::printf("FAIL %s:%d: %s (format %d)\n", __FILE__, __LINE__, #c, int(f)); ++g_fail; } } while (0)
using namespace hsm;

static LobbyLocal InLobby(Format f, int players) {
    LobbyLocal l;
    l.phase = LobbyPhase::InLobby; l.host = true; l.version = "0.2.0"; l.myName = "Ann";
    l.config.format = f; l.config.scoreLimit = DefaultScoreLimit(f);
    LobbyPlayer me; me.name = "Ann"; me.host = true; me.me = true;
    l.players = {me};
    for (int i = 1; i < players; ++i) { LobbyPlayer p; p.name = "P" + std::to_string(i); l.players.push_back(p); }
    return l;
}

static bool HasCard(Format f) {
    for (Format k : kModeOrder) if (k == f) return true;
    return false;
}

static void TestTables() {
    CHECK(kFormatCount == 16 && uint8_t(Format::CaptureTheFlag) == 10 && uint8_t(Format::Tag) == 11 && uint8_t(Format::Infection) == 12 &&
          uint8_t(Format::CrossbowTDM) == 13 && uint8_t(Format::Tournament) == 14 && uint8_t(Format::PitBridgeDuel) == 15);
    // One card per format but ThreeTeams (the Teams card's 3 teams), none twice; then Brawl and Progression.
    CHECK(kModeOrderCount == kFormatCount - 1 && kModeCardCount == kModeOrderCount + 2);
    std::set<uint8_t> carded;
    for (Format k : kModeOrder) CHECK(carded.insert(uint8_t(k)).second);
    std::set<std::string> names, blurbs;
    for (uint8_t i = 0; i < kFormatCount; ++i) {
        const Format f = Format(i);
        const bool teams3 = f == Format::ThreeTeams;
        CHECKF(HasCard(f) == !teams3, i);
        // Name and blurb: each format has its own (ThreeTeams is spelled as Teams), neither empty, neither the free-for-all fallback.
        const std::string name = FormatName(f), blurb = FormatBlurb(f);
        CHECKF(!name.empty() && !blurb.empty(), i);
        if (f != Format::FreeForAll) { CHECKF(name != "Free-for-all", i); CHECKF(blurb != FormatBlurb(Format::FreeForAll), i); }
        if (!teams3) { CHECKF(names.insert(name).second, i); CHECKF(blurbs.insert(blurb).second, i); }
        // The browser's mode chip has an entry that matches the name a lobby of this format advertises.
        bool listed = false;
        for (const std::string& m : BrowserModes()) listed = listed || ModeMatches(ModeName(f, Gear::Own, 2), m);
        CHECKF(listed, i);
        // A map to play on: the effective map for an empty pick allows the format, and some listed map allows it.
        const size_t em = EffectiveMap(f, "");
        CHECKF(em < MapCount() && MapAllowsFormat(kMaps[em], f), i);
        int maps = 0;
        for (const MapInfo& m : kMaps) maps += m.pick && MapAllowsFormat(m, f) ? 1 : 0;
        CHECKF(maps > 0, i);
        CHECKF(std::string(MapRefusal(f)) != "" , i);
        // A rule for how many players it needs: available for some count in 1..8, and the refusal says why.
        int ok = 0;
        for (int n = 1; n <= 8; ++n) {
            const ModeAvail a = ModeAvailable(f, n);
            ok += a.ok ? 1 : 0;
            CHECKF(a.ok || std::string(a.hint) != "", i);
        }
        CHECKF(ok > 0, i);
        // Settings: a non-empty list ending with Sync, each row once, each row with a label.
        MatchConfig c; c.format = f; c.scoreLimit = DefaultScoreLimit(f);
        const std::vector<LobbyRow> rows = SettingsFor(c);
        CHECKF(!rows.empty() && rows.back() == LobbyRow::Sync, i);
        std::set<uint8_t> seen;
        const LobbyLocal l = InLobby(f, 4);
        for (LobbyRow r : rows) { CHECKF(seen.insert(uint8_t(r)).second, i); CHECKF(!lobby_detail::Row(l, r).label.empty(), i); CHECKF(r <= LobbyRow::Sync, i); }
        // The scene lists the format's own card as selected, with the rows the list names.
        const LobbyScene s = BuildLobbyScene(InLobby(f, 4));
        CHECKF(s.settings.size() == rows.size() || f == Format::FreeForAll, i);
        // Sanitize keeps the format (ThreeTeams becomes Teams with 3) and is idempotent.
        const MatchConfig san = Sanitize(c);
        CHECKF(teams3 ? san.format == Format::TwoTeams : san.format == f, i);
        const MatchConfig san2 = Sanitize(san);
        CHECKF(san2.format == san.format && san2.scoreLimit == san.scoreLimit && san2.teams == san.teams, i);
        CHECKF(san.scoreLimit >= 1, i);
        // Table consistency: teams, rounds, scoring.
        if (HasTeams(f) || IsBotFormat(f)) CHECKF(TeamCount(f, 3) >= 2, i); else CHECKF(TeamCount(f, 3) == 0, i);
        if (IsTeamRound(f) || HasRoles(f)) CHECKF(HasTeams(f), i);
        if (IsQueueFormat(f) || IsTag(f)) CHECKF(IsRoundFormat(f) && !HasTeams(f) && ScoresPerFighter(f), i);
        if (IsCtf(f) || IsInfection(f)) CHECKF(HasTeams(f) && !ScoresPerFighter(f) && TeamCount(f, 3) == 2, i);
        CHECKF(!(ScoresPerFighter(f) && HasTeams(f)), i);
        CHECKF(DefaultScoreLimit(f) >= 1, i);
    }
    CHECK(FormatName(Format(kFormatCount)) == "Free-for-all");
}

// A MatchInfo of each format, as the host sends it with that format's own fields, goes through the wire; the fields a format does
// not use are zero and the decoder holds the queue fields to the formats that have them.
static void TestWire() {
    for (uint8_t i = 0; i < kFormatCount; ++i) {
        const Format f = Format(i);
        MatchInfo m;
        m.format = i; m.phase = uint8_t(MatchPhase::Live); m.scoreLimit = DefaultScoreLimit(f); m.teams = uint8_t(std::max(TeamCount(f, 2), 2));
        m.fighters.resize(3);
        for (uint8_t e = 0; e < 3; ++e) { m.fighters[e].entity = uint8_t(e + 1); m.fighters[e].alive = 1; m.fighters[e].team = HasTeams(f) ? int8_t(e % 2) : kNoTeam; m.fighters[e].name = "p"; }
        Objective flag; flag.kind = ObjectiveKind::Flag; flag.owner = 0; flag.state = ObjectiveState::Home;
        if (IsCtf(f)) { m.objectives = {flag, flag}; m.objectives[1].owner = 1; }
        if (IsTag(f)) { m.tagIt = 2; m.fuseLeftMs = 12000; }
        if (f == Format::Tournament) { m.queueOrder = {1, 2, 3}; m.bracketWinners = {0, 0, 0}; }
        if (f == Format::PitBridgeDuel) { m.queueOrder = {1, 2, 3}; m.streak = 1; }
        const std::vector<uint8_t> b = Encode(m);
        MatchInfo back;
        CHECKF(Decode(b.data(), b.size(), back), i);
        CHECKF(back.format == i && back.objectives.size() == m.objectives.size() && back.queueOrder == m.queueOrder &&
               back.bracketWinners == m.bracketWinners && back.streak == m.streak && back.tagIt == m.tagIt && back.fuseLeftMs == m.fuseLeftMs, i);
        // The queue fields belong to the two queue formats only.
        if (!IsQueueFormat(f)) {
            MatchInfo x = m; x.queueOrder = {1, 2};
            const std::vector<uint8_t> xb = Encode(x);
            CHECKF(!Decode(xb.data(), xb.size(), back), i);
        }
    }
}

// A host migration hands the new host the old one's scores (ScoreCarryFrom -> Match::Start). Whatever the format, the new match
// starts, plays through its countdown and a minute of ticks and ends or runs on without a crash or a negative score; the
// queue formats and Tag start a fresh bracket, queue and "it", Infection deals its zeros again (its scores are headcounts), and
// Capture the flag's captures stay (they are the team scores).
static void TestMigration() {
    for (uint8_t i = 0; i < kFormatCount; ++i) {
        const Format f = Format(i);
        MatchInfo old;
        old.format = i; old.phase = uint8_t(MatchPhase::Live); old.scoreLimit = DefaultScoreLimit(f); old.teams = 2;
        old.fighters.resize(4);
        for (uint8_t e = 0; e < 4; ++e) {
            FighterEntry& fe = old.fighters[e];
            fe.entity = uint8_t(e + 1); fe.alive = 1; fe.steamId = 1000 + e; fe.name = "p";
            fe.team = HasTeams(f) ? int8_t(e % 2) : kNoTeam; fe.kills = e; fe.deaths = 1;
        }
        if (ScoresPerFighter(f)) { for (uint8_t e = 0; e < 4; ++e) old.scores.push_back({int16_t(e + 1), int16_t(e % 2)}); }
        else if (HasTeams(f) && !IsInfection(f)) old.scores = {{int16_t(0), int16_t(2)}, {int16_t(1), int16_t(1)}};
        const ScoreCarry carry = ScoreCarryFrom(old);
        CHECKF(carry.format == f, i);
        MatchConfig c; c.format = f; c.scoreLimit = DefaultScoreLimit(f); c.timeLimitS = 0; c.respawnDelayS = 3;
        c = Sanitize(c);
        std::vector<FighterInfo> list;
        for (uint8_t e = 0; e < 4; ++e) list.push_back({uint8_t(e + 1), HasTeams(f) ? (IsInfection(f) ? int8_t(e == 0 ? kInfectedTeam : kSurvivorTeam) : int8_t(e % 2)) : kNoTeam, true, 1000u + e});
        Match m;
        m.SetBracketSeed(12345u);
        m.Start(c, list, 0, carry);
        uint32_t t = 0;
        for (int step = 0; step < 600; ++step) { t += 100; m.Tick(t); }   // a minute
        CHECKF(m.phase() != MatchPhase::Idle, i);
        for (uint8_t e = 1; e <= 4; ++e) CHECKF(m.Score(int(e)) >= 0 || !ScoresPerFighter(f), i);
        if (IsCtf(f)) CHECKF(m.Score(0) == 2 && m.Score(1) == 1, i);          // the captures stay
        if (IsInfection(f)) CHECKF(carry.empty() || carry.teams.size() <= 2, i);
        if (IsQueueFormat(f)) CHECKF(m.PairA() != 0 && m.PairB() != 0 && m.PairA() != m.PairB(), i);   // a fresh pairing is seated
    }
}

// server_format words for every format the server loop can run, and the fallbacks for a player count the format cannot start with.
static void TestServerFormat() {
    struct W { const char* word; Format f; int teams; };
    const W words[] = {{"ffa", Format::FreeForAll, 2}, {"duel", Format::Duel, 2}, {"lms", Format::LastManStanding, 2}, {"teams", Format::TwoTeams, 2},
                       {"teamduel", Format::TeamDuel, 2}, {"riot", Format::Riot, 2}, {"juggernaut", Format::Juggernaut, 2},
                       {"ctf", Format::CaptureTheFlag, 2}, {"tag", Format::Tag, 2}, {"infection", Format::Infection, 2},
                       {"crossbowtdm", Format::CrossbowTDM, 2}, {"tournament", Format::Tournament, 2}, {"pitbridge", Format::PitBridgeDuel, 2},
                       {"survival", Format::PlayersVsBots, 2}, {"zombies", Format::Zombies, 2}, {"CTF", Format::CaptureTheFlag, 2}, {"PitBridge", Format::PitBridgeDuel, 2}};
    for (const W& w : words) {
        Format f = Format::FreeForAll; int teams = 0;
        CHECK(ParseServerFormat(w.word, &f, &teams) && f == w.f && teams == w.teams);
    }
    Format f = Format::FreeForAll;
    CHECK(!ParseServerFormat("capture", &f) && !ParseServerFormat("", &f) && !ParseServerFormat("pit bridge", &f));
    // Every format with a card but ThreeTeams has a word.
    std::set<uint8_t> reachable;
    for (const W& w : words) reachable.insert(uint8_t(w.f));
    for (uint8_t i = 0; i < kFormatCount; ++i) CHECKF(reachable.count(i) || Format(i) == Format::ThreeTeams, i);
    // A player count that cannot start the format falls back to Last Man Standing (Duel: not exactly two; the 3-player formats: two).
    CHECK(ServerFormatFor(Format::Duel, 3) == Format::LastManStanding && ServerFormatFor(Format::Duel, 2) == Format::Duel);
    for (Format r : {Format::Riot, Format::Juggernaut, Format::Tournament}) {
        CHECK(ServerFormatFor(r, 2) == Format::LastManStanding && ServerFormatFor(r, 3) == r && ServerFormatFor(r, 8) == r);
    }
    for (Format r : {Format::CaptureTheFlag, Format::Tag, Format::Infection, Format::CrossbowTDM, Format::PitBridgeDuel, Format::TeamDuel})
        CHECK(ServerFormatFor(r, 2) == r && ServerFormatFor(r, 6) == r);
}

int main() {
    TestTables();
    TestWire();
    TestMigration();
    TestServerFormat();
    if (g_fail) { std::printf("%d failure(s)\n", g_fail); return 1; }
    std::printf("all-formats tests passed\n");
    return 0;
}
