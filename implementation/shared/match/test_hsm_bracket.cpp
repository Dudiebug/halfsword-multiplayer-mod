// The Tournament bracket: sizes 2..8 and beyond, byes for the top seeds, play order, forfeits before and during a game,
// determinism by seed, the final, and the wire round trip.
#include "match/hsm_bracket.h"
#include <cstdio>
#include <set>

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++g_fail; } } while (0)
using namespace hsm;

static std::vector<uint8_t> Players(int n) {
    std::vector<uint8_t> p;
    for (int i = 1; i <= n; ++i) p.push_back(uint8_t(i));
    return p;
}

// Plays the whole bracket, the first fighter of every game winning; returns the games played in order.
static std::vector<Bracket::Game> PlayAll(Bracket& b) {
    std::vector<Bracket::Game> played;
    Bracket::Game g;
    while (b.Current(g)) { played.push_back(g); CHECK(b.Report(g.a)); }
    return played;
}

static void TestSeedPlaces() {
    CHECK((BracketSeedPlaces(2) == std::vector<int>{1, 2}));
    CHECK((BracketSeedPlaces(4) == std::vector<int>{1, 4, 2, 3}));
    CHECK((BracketSeedPlaces(8) == std::vector<int>{1, 8, 4, 5, 2, 7, 3, 6}));
    CHECK(BracketSize(0) == 2 && BracketSize(2) == 2 && BracketSize(3) == 4 && BracketSize(5) == 8 && BracketSize(8) == 8 && BracketSize(9) == 16);
}

static void TestSizesTwoToEight() {
    for (int n = 2; n <= 8; ++n) {
        Bracket b;
        CHECK(b.Start(Players(n), 7u + uint32_t(n)));
        CHECK(b.valid() && b.Size() == BracketSize(size_t(n)) && b.seeds().size() == size_t(n));
        // every player plays: n - 1 real games decide a bracket of n
        std::set<uint8_t> inFirst;
        const std::vector<Bracket::Game> played = PlayAll(b);
        CHECK(int(played.size()) == n - 1);
        CHECK(b.Done() && b.Champion() != 0);
        for (const Bracket::Game& g : played) { CHECK(g.a != g.b && g.a != 0 && g.b != 0); }
        // the champion lost nothing: he is in the running, everybody else is out
        int running = 0;
        for (int e = 1; e <= n; ++e) running += b.InRunning(uint8_t(e)) ? 1 : 0;
        CHECK(running == 1 && b.InRunning(b.Champion()));
        // the last game is the final
        CHECK(played.back().round == b.Rounds() - 1 && b.StageName(played.back().round) == "Final");
        // play order: rounds never go back
        for (size_t i = 1; i < played.size(); ++i) CHECK(played[i].round >= played[i - 1].round);
    }
}

static void TestByesGoToTheTopSeeds() {
    // 5 players in 8 places: seeds 1, 2 and 3 meet a bye; only seeds 4 and 5 play in round 0.
    Bracket b;
    CHECK(b.Start(Players(5), 3));
    const std::vector<uint8_t>& s = b.seeds();
    Bracket::Game g;
    CHECK(b.Current(g) && g.round == 0);
    CHECK(g.a == s[3] && g.b == s[4]);                      // seed 4 against seed 5
    CHECK(b.PlayedGames(0) == 1 && b.GamesInRound(0) == 4);
    CHECK(b.IsBye(0, 0) && !b.IsBye(0, 1) && b.IsBye(0, 2) && b.IsBye(0, 3));
    CHECK(b.GameNumber(0, 1) == 1 && b.GameNumber(0, 0) == 0);
    // after the game, round 1: seed 1 against the winner, seed 2 against seed 3
    CHECK(b.Report(g.b));
    CHECK(b.Current(g) && g.round == 1 && g.index == 0 && g.a == s[0] && g.b == s[4]);
    Bracket::Game next;
    CHECK(b.NextGame(next) && next.round == 1 && next.index == 1 && next.a == s[1] && next.b == s[2]);
    // 3 players: seed 1 has the bye
    Bracket c;
    CHECK(c.Start(Players(3), 3));
    CHECK(c.Current(g) && g.round == 0 && g.a == c.seeds()[1] && g.b == c.seeds()[2]);
    CHECK(c.PlayedGames(0) == 1 && c.GamesInRound(0) == 2);
    const uint8_t winner = g.a;
    CHECK(c.Report(winner));
    CHECK(c.Current(g) && g.round == 1 && g.a == c.seeds()[0] && g.b == winner);
    // 4 players: no byes
    Bracket d;
    CHECK(d.Start(Players(4), 3));
    for (int i = 0; i < 2; ++i) CHECK(!d.IsBye(0, i));
    CHECK(d.PlayedGames(0) == 2 && d.StageName(0) == "Semi-final" && d.StageName(1) == "Final");
    // 8: quarter-finals; 9 needs a round of 16
    Bracket e8, e9;
    CHECK(e8.Start(Players(8), 1) && e8.StageName(0) == "Quarter-final" && e8.Rounds() == 3);
    CHECK(e9.Start(Players(9), 1) && e9.StageName(0) == "Round of 16" && e9.Rounds() == 4 && e9.PlayedGames(0) == 1);
}

static void TestSeedingIsDeterministicAndShuffles() {
    Bracket a, b, c;
    CHECK(a.Start(Players(8), 42) && b.Start(Players(8), 42));
    CHECK(a.seeds() == b.seeds());
    CHECK(a.GameWinners() == b.GameWinners());
    // the order the players are given in does not matter
    std::vector<uint8_t> rev = Players(8);
    std::reverse(rev.begin(), rev.end());
    CHECK(c.Start(rev, 42) && c.seeds() == a.seeds());
    // another seed gives another order for at least one of a few seeds
    bool differs = false;
    for (uint32_t s = 1; s < 20; ++s) { Bracket d; d.Start(Players(8), s); differs = differs || d.seeds() != a.seeds(); }
    CHECK(differs);
    // all the players are in the seeds exactly once
    std::set<uint8_t> all(a.seeds().begin(), a.seeds().end());
    CHECK(all.size() == 8);
}

static void TestRejectsBadStarts() {
    Bracket b;
    CHECK(!b.Start({1, 1}, 0) && !b.valid());
    CHECK(!b.Start({0, 2}, 0));
    CHECK(!b.Start({1, kBracketEmpty}, 0));
    CHECK(!b.Start(Players(17), 0));
    CHECK(b.Start(Players(16), 0) && b.Rounds() == 4);
    // one player is the champion at once; none has no champion
    Bracket one, none;
    CHECK(one.Start({5}, 0) && one.Done() && one.Champion() == 5);
    CHECK(none.Start({}, 0) && none.Done() && none.Champion() == 0);
    Bracket::Game g;
    CHECK(!one.Current(g) && !none.Current(g));
    // a wrong winner is refused
    Bracket x;
    CHECK(x.Start(Players(4), 0));
    CHECK(x.Current(g) && !x.Report(0) && !x.Report(99));
}

static void TestForfeitBeforeAPairing() {
    // 4 players: seed 3 leaves before anything is played; seed 2 meets seed 3's place and advances at once.
    Bracket b;
    CHECK(b.Start(Players(4), 11));
    const std::vector<uint8_t> s = b.seeds();
    Bracket::Game g;
    CHECK(b.Current(g) && g.a == s[0] && g.b == s[3]);      // seed 1 against seed 4
    CHECK(b.Remove(s[2]));                                  // seed 3 leaves: seed 2 has a bye
    CHECK(!b.InRunning(s[2]) && b.Contains(s[2]) == false);
    CHECK(b.Current(g) && g.a == s[0] && g.b == s[3]);      // the first game is unchanged
    CHECK(b.Report(s[3]));
    CHECK(b.Current(g) && g.round == 1 && g.a == s[3] && g.b == s[1]);
    CHECK(b.StageName(g.round) == "Final");
    CHECK(b.Report(s[1]) && b.Done() && b.Champion() == s[1]);
}

static void TestForfeitDuringAPairing() {
    // 4 players: a player of the game being fought leaves; the opponent takes it and the next game is up.
    Bracket b;
    CHECK(b.Start(Players(4), 5));
    const std::vector<uint8_t> s = b.seeds();
    Bracket::Game g;
    CHECK(b.Current(g));
    const uint8_t leaver = g.a, stays = g.b;
    CHECK(b.Remove(leaver));
    CHECK(!b.InRunning(leaver) && b.InRunning(stays));
    CHECK(b.Current(g) && g.round == 0 && g.a != leaver && g.b != leaver && g.a != stays && g.b != stays);   // the other semi-final
    CHECK(b.Report(g.a));
    CHECK(b.Current(g) && g.round == 1 && g.a == stays);           // the opponent advanced to the final
    // a second Remove of the same player does nothing
    CHECK(!b.Remove(leaver));
    CHECK(!b.Remove(99));
}

static void TestForfeitsEverywhere() {
    // a winner who leaves between two games; his next opponent advances
    Bracket b;
    CHECK(b.Start(Players(4), 9));
    Bracket::Game g;
    CHECK(b.Current(g));
    const uint8_t w = g.a;
    CHECK(b.Report(w));
    CHECK(b.Remove(w));                                     // won, then left before the final
    CHECK(b.Current(g) && g.round == 0);
    CHECK(b.Report(g.b));
    CHECK(!b.Current(g) && b.Done() && b.Champion() != w && b.Champion() != 0);
    // a loser leaving changes nothing
    Bracket c;
    CHECK(c.Start(Players(4), 9));
    CHECK(c.Current(g));
    const uint8_t loser = g.b;
    CHECK(c.Report(g.a));
    CHECK(!c.Remove(loser));
    CHECK(c.Current(g) && g.round == 0);
    // the champion cannot be removed
    CHECK(c.Report(g.a));
    CHECK(c.Current(g) && c.Report(g.a) && c.Done());
    const uint8_t champ = c.Champion();
    CHECK(!c.Remove(champ) && c.Champion() == champ);
    // everybody but one leaves: the last one is the champion
    Bracket d;
    CHECK(d.Start(Players(5), 2));
    for (uint8_t e : {1, 2, 3, 4}) CHECK(d.Remove(e));
    CHECK(d.Done() && d.Champion() == 5);
    // the last one leaves too: nobody
    Bracket e;
    CHECK(e.Start(Players(2), 2));
    CHECK(e.Remove(1) && e.Done() && e.Champion() == 2);
}

static void TestAllLeaveBeforeTheFirstGame() {
    for (int n = 2; n <= 8; ++n) {
        Bracket b;
        CHECK(b.Start(Players(n), 1));
        for (int e = 1; e < n; ++e) CHECK(b.Remove(uint8_t(e)));
        CHECK(b.Done() && b.Champion() == uint8_t(n));
    }
}

static void TestWireRoundTrip() {
    for (int n = 2; n <= 8; ++n)
        for (int stop = 0; stop <= n; ++stop) {
            Bracket b;
            CHECK(b.Start(Players(n), uint32_t(n * 31)));
            Bracket::Game g;
            for (int i = 0; i < stop && b.Current(g); ++i) b.Report(i % 2 ? g.b : g.a);
            if (stop == 2) b.Remove(b.seeds()[0]);
            Bracket r;
            CHECK(r.Restore(b.seeds(), b.GameWinners()));
            CHECK(r.GameWinners() == b.GameWinners() && r.Done() == b.Done() && r.Champion() == b.Champion());
            Bracket::Game x, y;
            CHECK(r.Current(x) == b.Current(y));
            if (r.Current(x) && b.Current(y)) CHECK(x.round == y.round && x.index == y.index && x.a == y.a && x.b == y.b);
            for (int e = 1; e <= n; ++e) CHECK(r.InRunning(uint8_t(e)) == b.InRunning(uint8_t(e)));
        }
    // a player who left before his first game is a 0 in the seeds
    Bracket b;
    CHECK(b.Start(Players(4), 3));
    const uint8_t gone = b.seeds()[2];
    CHECK(b.Remove(gone));
    Bracket r;
    CHECK(r.Restore(b.seeds(), b.GameWinners()));
    CHECK(!r.InRunning(gone) || b.InRunning(gone));
    std::vector<uint8_t> seeds = b.seeds();
    seeds[2] = 0;
    Bracket z;
    CHECK(z.Restore(seeds, b.GameWinners()));
    CHECK(!z.Contains(gone));
    Bracket::Game x, y;
    CHECK(z.Current(x) && b.Current(y) && x.a == y.a && x.b == y.b);
}

static void TestWireValidation() {
    CHECK(ValidBracketWire({}, {}));
    CHECK(!ValidBracketWire({1}, {}));                         // one seed is no bracket
    CHECK(!ValidBracketWire({1, 2}, {}));                      // 1 game, 0 winners
    CHECK(ValidBracketWire({1, 2}, {0}));
    CHECK(ValidBracketWire({1, 2}, {1}) && ValidBracketWire({1, 2}, {2}));
    CHECK(!ValidBracketWire({1, 2}, {3}));                     // not in the game
    CHECK(!ValidBracketWire({1, 2}, {kBracketEmpty - 1}));
    CHECK(!ValidBracketWire({1, 1}, {0}));                     // twice
    CHECK(!ValidBracketWire({1, kBracketEmpty}, {0}));
    CHECK(!ValidBracketWire({1, 2, 3}, {0, 0}));               // 3 seeds make 3 games
    CHECK(ValidBracketWire({1, 2, 3}, {0, 0, 0}));
    // a decided later game needs its feeders decided
    Bracket b;
    CHECK(b.Start(Players(4), 0));
    std::vector<uint8_t> w = b.GameWinners();
    CHECK(w.size() == 3);
    std::vector<uint8_t> bad = w;
    bad[2] = b.seeds()[0];
    CHECK(!ValidBracketWire(b.seeds(), bad));
    // a bye settles to the player behind it, who may have left since (empty); a game nobody has played cannot be empty
    CHECK(ValidBracketWire({1, 2, 3}, {kBracketEmpty, 0, 0}));
    CHECK(!ValidBracketWire({1, 2, 3}, {0, 0, kBracketEmpty}));
    CHECK(!ValidBracketWire({1, 2, 3}, {3, 0, 0}));               // a bye goes to the player behind it, nobody else
    // too many seeds, a winner list of the wrong size
    CHECK(!ValidBracketWire(Players(17), std::vector<uint8_t>(31, 0)));
    CHECK(!ValidBracketWire(Players(5), std::vector<uint8_t>(6, 0)));
    CHECK(ValidBracketWire(Players(5), std::vector<uint8_t>(7, 0)));
    // a bracket decided all the way to the champion
    Bracket full;
    CHECK(full.Start(Players(8), 1));
    PlayAll(full);
    CHECK(ValidBracketWire(full.seeds(), full.GameWinners()));
    // changing any one winner of a decided bracket to a player who is not in that game breaks it
    for (size_t i = 0; i < full.GameWinners().size(); ++i) {
        std::vector<uint8_t> t = full.GameWinners();
        t[i] = 200;
        CHECK(!ValidBracketWire(full.seeds(), t));
    }
}

static void TestStageNumbers() {
    Bracket b;
    CHECK(b.Start(Players(8), 6));
    Bracket::Game g;
    CHECK(b.Current(g) && b.GameNumber(g.round, g.index) == 1 && b.PlayedGames(g.round) == 4);
    CHECK(b.Report(g.a) && b.Current(g) && b.GameNumber(g.round, g.index) == 2);
    CHECK(b.StageName(-1).empty() && b.StageName(3).empty());
}

int main() {
    TestSeedPlaces();
    TestSizesTwoToEight();
    TestByesGoToTheTopSeeds();
    TestSeedingIsDeterministicAndShuffles();
    TestRejectsBadStarts();
    TestForfeitBeforeAPairing();
    TestForfeitDuringAPairing();
    TestForfeitsEverywhere();
    TestAllLeaveBeforeTheFirstGame();
    TestWireRoundTrip();
    TestWireValidation();
    TestStageNumbers();
    if (g_fail) { std::printf("%d failure(s)\n", g_fail); return 1; }
    std::puts("ok");
    return 0;
}
