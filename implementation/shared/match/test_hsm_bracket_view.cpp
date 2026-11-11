// The Tab board's Tournament bracket (hud_detail::BracketRows, Bracket::PlacesOf): a line per stage, one per game that is no
// bye, played games as "X beat Y", the game being fought NOW, the one after NEXT, undecided places "?", the local player's games
// still to fight framed; only on the Tab board, never on the results board or in another mode.
#include "match/hsm_hud_scene.h"
#include <cstdio>
#include <string>

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++g_fail; } } while (0)
using namespace hsm;
using V = std::vector<uint8_t>;

static const char* kNames[] = {"", "Ann", "Bo", "Cy", "Di", "Ed", "Flo", "Gus", "Hal"};

static MatchInfo Tourney(int players, int playedGames) {
    MatchInfo m;
    m.format = uint8_t(Format::Tournament); m.scoreLimit = 1; m.phase = uint8_t(MatchPhase::Live); m.round = 1;
    Bracket b;
    V p; for (int i = 1; i <= players; ++i) p.push_back(uint8_t(i));
    b.Start(p, 4);
    Bracket::Game g;
    for (int i = 0; i < playedGames && b.Current(g); ++i) b.Report(g.a);
    m.queueOrder = b.seeds(); m.bracketWinners = b.GameWinners();
    for (int i = 1; i <= players; ++i) m.fighters.push_back({uint8_t(i), kNoTeam, 0, 0, i <= 8 ? std::string(kNames[i]) : "P" + std::to_string(i), 10, true});
    return m;
}
static Bracket Restored(const MatchInfo& m) { Bracket b; b.Restore(m.queueOrder, m.bracketWinners); return b; }
static std::string Name(int e) { return e > 0 ? kNames[e] : "?"; }
static int Count(const std::vector<BoardRow>& v, BoardRow::Kind k) { int n = 0; for (const BoardRow& r : v) n += r.kind == k ? 1 : 0; return n; }
static const BoardRow* Tagged(const std::vector<BoardRow>& v, const char* tag) { for (const BoardRow& r : v) if (r.tag == tag) return &r; return nullptr; }

static void TestPlacesOf() {
    const Bracket b = Restored(Tourney(4, 0));
    CHECK(b.Rounds() == 2);
    const Bracket::Places semi = b.PlacesOf(0, 0), fin = b.PlacesOf(1, 0);
    CHECK(semi.a > 0 && semi.b > 0 && semi.a != semi.b && semi.winner == -1);
    CHECK(fin.a == -1 && fin.b == -1 && fin.winner == -1);
    for (const Bracket::Places& p : {b.PlacesOf(-1, 0), b.PlacesOf(2, 0), b.PlacesOf(0, 2), b.PlacesOf(1, 1), b.PlacesOf(0, -1)})
        CHECK(p.a == -1 && p.b == -1 && p.winner == -1);
    const Bracket played = Restored(Tourney(4, 1));
    const Bracket::Places first = played.PlacesOf(0, 0);
    CHECK(first.winner == first.a && first.a > 0);
}

static void TestFresh() {
    // Four players: SEMI-FINAL, its two games (the first NOW, the second NEXT), FINAL and "? vs ?".
    const MatchInfo m = Tourney(4, 0);
    const Bracket b = Restored(m);
    const Bracket::Places s0 = b.PlacesOf(0, 0), s1 = b.PlacesOf(0, 1);
    const std::vector<BoardRow> v = hud_detail::BracketRows(b, m, uint8_t(s1.b));
    CHECK(v.size() == 5);
    if (v.size() != 5) return;
    CHECK(v[0].kind == BoardRow::Kind::Stage && v[0].name == "SEMI-FINAL" && v[0].tag.empty() && !v[0].local);
    CHECK(v[1].kind == BoardRow::Kind::Game && v[1].name == Name(s0.a) + " vs " + Name(s0.b) && v[1].tag == "NOW" && !v[1].local);
    CHECK(v[2].kind == BoardRow::Kind::Game && v[2].name == Name(s1.a) + " vs " + Name(s1.b) && v[2].tag == "NEXT" && v[2].local);
    CHECK(v[3].kind == BoardRow::Kind::Stage && v[3].name == "FINAL");
    CHECK(v[4].kind == BoardRow::Kind::Game && v[4].name == "? vs ?" && v[4].tag.empty() && !v[4].local);
    for (const BoardRow& r : v) if (r.kind == BoardRow::Kind::Game) CHECK(r.ink.r == style::kBone.r);
    // nobody's games framed for a spectator (entity 0)
    for (const BoardRow& r : hud_detail::BracketRows(b, m, 0)) CHECK(!r.local);
}

static void TestPlayed() {
    // The first semi-final played: "winner beat loser", dimmed, untagged, never framed; the other is NOW; the final has its first name.
    const MatchInfo m = Tourney(4, 1);
    const Bracket b = Restored(m);
    const Bracket::Places s0 = b.PlacesOf(0, 0), s1 = b.PlacesOf(0, 1);
    const std::vector<BoardRow> v = hud_detail::BracketRows(b, m, uint8_t(s0.a));
    CHECK(v.size() == 5);
    if (v.size() != 5) return;
    CHECK(v[1].name == Name(s0.a) + " beat " + Name(s0.b) && v[1].tag.empty() && !v[1].local && v[1].ink.r == style::kIron.r);
    CHECK(v[2].name == Name(s1.a) + " vs " + Name(s1.b) && v[2].tag == "NOW" && v[2].ink.r == style::kBone.r);
    CHECK(v[4].name == Name(s0.a) + " vs ?" && v[4].tag.empty() && v[4].local);   // the winner's final still to fight
    CHECK(!Tagged(v, "NEXT"));                                                      // the final has one player only so far
    // The second game won by its second player: the winner is named first.
    MatchInfo n = m;
    Bracket w = Restored(m);
    Bracket::Game g;
    CHECK(w.Current(g) && w.Report(g.b));
    n.bracketWinners = w.GameWinners();
    const std::vector<BoardRow> u = hud_detail::BracketRows(Restored(n), n, 0);
    CHECK(u.size() == 5 && u[2].name == Name(g.b) + " beat " + Name(g.a));
    CHECK(u.size() == 5 && u[4].name == Name(s0.a) + " vs " + Name(g.b) && u[4].tag == "NOW");
}

static void TestByesAndSizes() {
    // Three players: the top seed's bye is no line; its final reads "<top seed> vs ?".
    const MatchInfo m = Tourney(3, 0);
    const Bracket b = Restored(m);
    const std::vector<BoardRow> v = hud_detail::BracketRows(b, m, 0);
    CHECK(v.size() == 4 && Count(v, BoardRow::Kind::Stage) == 2 && Count(v, BoardRow::Kind::Game) == 2);
    if (v.size() == 4) CHECK(v[3].name.find(" vs ?") != std::string::npos && v[3].name != "? vs ?");
    for (const BoardRow& r : v) CHECK(r.name.find(" vs ") == std::string::npos || r.name.find("#") == std::string::npos);
    // Eight players: three stages and seven games, the first stage the quarter-finals.
    const MatchInfo e = Tourney(8, 0);
    const std::vector<BoardRow> w = hud_detail::BracketRows(Restored(e), e, 0);
    CHECK(w.size() == 10 && Count(w, BoardRow::Kind::Stage) == 3 && Count(w, BoardRow::Kind::Game) == 7);
    if (!w.empty()) CHECK(w[0].name == "QUARTER-FINAL");
    CHECK(Tagged(w, "NOW") && Tagged(w, "NEXT") && Tagged(w, "NOW") != Tagged(w, "NEXT"));
    // Everything played: every game reads "beat", nothing is NOW.
    const MatchInfo done = Tourney(4, 3);
    const std::vector<BoardRow> d = hud_detail::BracketRows(Restored(done), done, 1);
    CHECK(d.size() == 5 && !Tagged(d, "NOW") && !Tagged(d, "NEXT"));
    for (const BoardRow& r : d) if (r.kind == BoardRow::Kind::Game) CHECK(r.name.find(" beat ") != std::string::npos && !r.local);
}

static void TestOpenPlace() {
    // a leaver's place still open (#1429): "open place" in a game to fight; one who left after his game keeps his name
    MatchInfo m = Tourney(4, 1);
    const Bracket b = Restored(m);
    const Bracket::Places s0 = b.PlacesOf(0, 0), s1 = b.PlacesOf(0, 1);
    const int loser = s0.winner == s0.a ? s0.b : s0.a;
    std::erase_if(m.fighters, [&](const FighterEntry& f) { return f.entity == s1.b || f.entity == loser; });
    const std::vector<BoardRow> v = hud_detail::BracketRows(b, m, 0);
    CHECK(v.size() == 5);
    if (v.size() != 5) return;
    CHECK(v[1].name == Name(s0.winner) + " beat #" + std::to_string(loser));
    CHECK(v[2].name == Name(s1.a) + " vs open place" && v[2].tag == "NOW");
    CHECK(v[4].name == Name(s0.winner) + " vs ?");
}

static void TestBigBracketFits() {
    // Sixteen players: 19 bracket lines. Nothing played: cut at the end, never ending on a stage line.
    MatchInfo m = Tourney(16, 0);
    std::vector<BoardRow> v = hud_detail::BracketRows(Restored(m), m, 0);
    CHECK(v.size() == 19 && !v[1].played);
    hud_detail::FitBracket(v, 10);
    CHECK(v.size() == 9 && v[0].kind == BoardRow::Kind::Stage && Count(v, BoardRow::Kind::Game) == 8 && Tagged(v, "NOW") && Tagged(v, "NEXT"));
    // The first round played: its games go first (the oldest first), then its stage line; the rest is whole.
    m = Tourney(16, 8);
    v = hud_detail::BracketRows(Restored(m), m, 0);
    CHECK(v.size() == 19 && v[1].played && v[8].played && !v[10].played && !v[0].played);
    std::vector<BoardRow> w = v;
    hud_detail::FitBracket(w, 15);                          // four too many: the four oldest games go
    CHECK(w.size() == 15 && w[0].name == "ROUND OF 16" && w[1].name == v[5].name && w[4].name == v[8].name);
    w = v;
    hud_detail::FitBracket(w, 10);
    CHECK(w.size() == 10 && w[0].name == "QUARTER-FINAL" && w[1].tag == "NOW" && w[9].name == "? vs ?");
    for (const BoardRow& r : w) CHECK(!r.played);
    w = v; hud_detail::FitBracket(w, 19); CHECK(w.size() == 19 && w[1].played);   // it fits: untouched
    w = v; hud_detail::FitBracket(w, 0); CHECK(w.empty());
    // On the board: the fighters all show, the bracket takes the rows left.
    LocalHud l; l.me = 1; l.infoAtMs = 1000; l.tab = true;
    HudScene s = BuildHudScene(m, l, 1000);
    CHECK(kBoardRowBudget == 26 && s.board.rows.size() == 16 && s.board.bracket.size() == 10 && s.board.bracket[0].name == "QUARTER-FINAL");
    // more fighters than lines (bots listed with them): no bracket, nothing wraps round
    for (int i = 17; i <= 28; ++i) m.fighters.push_back({uint8_t(i), kNoTeam, 0, 0, "B" + std::to_string(i), 10, true});
    s = BuildHudScene(m, l, 1000);
    CHECK(s.board.rows.size() == 28 && s.board.bracket.empty());
}

static void TestOnTheBoard() {
    MatchInfo m = Tourney(4, 0);
    LocalHud l; l.me = 1; l.infoAtMs = 1000; l.tab = true;
    HudScene s = BuildHudScene(m, l, 1000);
    CHECK(s.board.visible && s.board.rows.size() == 4 && s.board.bracket.size() == 5);
    for (const BoardRow& r : s.board.rows) CHECK(r.kind == BoardRow::Kind::Fighter);
    // not held: no board at all
    l.tab = false;
    CHECK(!BuildHudScene(m, l, 1000).board.visible);
    // the results board (Tab held or not) keeps to the fighters
    MatchInfo res = Tourney(4, 3); res.phase = uint8_t(MatchPhase::Results);
    l.tab = true;
    HudScene r = BuildHudScene(res, l, 1000);
    CHECK(r.board.visible && r.board.bracket.empty() && r.board.rows.size() == 4);
    // another mode has no bracket
    MatchInfo duel; duel.format = uint8_t(Format::Duel); duel.scoreLimit = 2; duel.phase = uint8_t(MatchPhase::Live);
    duel.fighters = {{1, kNoTeam, 0, 0, "Ann", 0, true}, {2, kNoTeam, 0, 0, "Bo", 0, true}};
    HudScene d = BuildHudScene(duel, l, 1000);
    CHECK(d.board.visible && d.board.bracket.empty());
    // a broken bracket from the wire: no bracket lines
    MatchInfo bad = Tourney(4, 0); bad.bracketWinners.push_back(1);
    CHECK(BuildHudScene(bad, l, 1000).board.bracket.empty());
}

int main() {
    TestPlacesOf();
    TestFresh();
    TestPlayed();
    TestByesAndSizes();
    TestOpenPlace();
    TestBigBracketFits();
    TestOnTheBoard();
    if (g_fail) { std::printf("%d failure(s)\n", g_fail); return 1; }
    std::printf("hsm_bracket_view: ok\n");
    return 0;
}
