// The Pit Bridge Duels queue: the winner stays, the loser goes to the back, a leaver is removed, a late joiner goes to the
// back, a draw sends both to the back in order, the streak, determinism and the wire.
#include "match/hsm_kingqueue.h"
#include <cstdio>

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++g_fail; } } while (0)
using namespace hsm;
using V = std::vector<uint8_t>;

static KingQueue Of(const V& order) {
    KingQueue q;
    CHECK(q.Restore(order, 0));
    return q;
}

static void TestWinnerStays() {
    KingQueue q = Of({1, 2, 3, 4});
    uint8_t a, b;
    CHECK(q.Pair(a, b) && a == 1 && b == 2);
    CHECK(q.Report(1));                                   // 1 beat 2: 2 goes to the back, 1 faces 3
    CHECK(q.order() == V({1, 3, 4, 2}));
    CHECK(q.Pair(a, b) && a == 1 && b == 3);
    CHECK(q.Report(3));                                   // the challenger wins: he stays, the old king goes to the back
    CHECK(q.order() == V({3, 4, 2, 1}));
    CHECK(q.Pair(a, b) && a == 3 && b == 4);
}

static void TestTwoPlayersRepeat() {
    KingQueue q = Of({1, 2});
    CHECK(q.Report(2) && q.order() == V({2, 1}));
    CHECK(q.Report(1) && q.order() == V({1, 2}));
    CHECK(q.Report(1) && q.order() == V({1, 2}));
}

static void TestStreak() {
    KingQueue q = Of({1, 2, 3});
    CHECK(q.streak() == 0 && q.King() == 0);
    CHECK(q.Report(1) && q.streak() == 1 && q.King() == 1);
    CHECK(q.Report(1) && q.streak() == 2);
    CHECK(q.Report(1) && q.streak() == 3 && q.King() == 1);
    CHECK(q.Report(3) && q.streak() == 1 && q.King() == 3);      // a new king starts at 1
    CHECK(q.Report(3) && q.streak() == 2);
    // a draw ends it
    CHECK(q.Draw() && q.streak() == 0 && q.King() == 0);
    CHECK(q.Report(q.order()[0]) && q.streak() == 1);
    // the king leaving ends it
    const uint8_t king = q.King();
    CHECK(q.Remove(king) && q.streak() == 0 && q.King() == 0);
    // a leaver who is not the king keeps it
    KingQueue r = Of({1, 2, 3, 4});
    CHECK(r.Report(1) && r.Report(1) && r.streak() == 2);
    CHECK(r.Remove(4) && r.streak() == 2 && r.King() == 1);
    // it saturates instead of wrapping
    KingQueue s;
    CHECK(s.Restore({1, 2}, 255));
    CHECK(s.Report(1) && s.streak() == 255);
}

static void TestDraw() {
    KingQueue q = Of({1, 2, 3, 4});
    CHECK(q.Draw() && q.order() == V({3, 4, 1, 2}));
    uint8_t a, b;
    CHECK(q.Pair(a, b) && a == 3 && b == 4);
    KingQueue two = Of({1, 2});
    CHECK(two.Draw() && two.order() == V({1, 2}));                  // two players: the same two again
    KingQueue three = Of({1, 2, 3});
    CHECK(three.Draw() && three.order() == V({3, 1, 2}));
    KingQueue one = Of({1});
    CHECK(!one.Draw() && !one.Report(1));
    KingQueue none;
    uint8_t x, y;
    CHECK(!none.Pair(x, y) && !none.Draw());
}

static void TestLeaversAndJoiners() {
    KingQueue q = Of({1, 2, 3, 4});
    CHECK(q.Remove(3) && q.order() == V({1, 2, 4}));
    CHECK(!q.Remove(3) && !q.Remove(99));
    CHECK(q.Remove(1) && q.order() == V({2, 4}));                   // a fighter leaves: the next in line steps up
    CHECK(q.Add(7) && q.order() == V({2, 4, 7}));
    CHECK(!q.Add(7) && !q.Add(2) && !q.Add(0));                     // once only
    CHECK(q.Report(4) && q.order() == V({4, 7, 2}));
    // a fight won by somebody outside the pair changes nothing
    CHECK(!q.Report(2) && q.order() == V({4, 7, 2}));
    // full
    KingQueue full;
    V all;
    for (uint8_t e = 1; e <= kMaxQueuePlayers; ++e) all.push_back(e);
    CHECK(kMaxQueuePlayers == 32 && full.Start(all, 1));
    CHECK(!full.Add(33));
    all.push_back(33);
    CHECK(!KingQueue().Start(all, 1));
}

static void TestLoserGoesBehindTheWaiting() {
    // 5 players: the loser waits behind everyone who was already waiting, whoever he was
    KingQueue q = Of({1, 2, 3, 4, 5});
    CHECK(q.Report(2));
    CHECK(q.order() == V({2, 3, 4, 5, 1}));
    CHECK(q.Report(3));
    CHECK(q.order() == V({3, 4, 5, 1, 2}));
}

static void TestDeterminism() {
    V p{1, 2, 3, 4, 5, 6};
    KingQueue a, b, c;
    CHECK(a.Start(p, 99) && b.Start(p, 99));
    CHECK(a.order() == b.order());
    V rev(p.rbegin(), p.rend());
    CHECK(c.Start(rev, 99) && c.order() == a.order());
    bool differs = false;
    for (uint32_t s = 1; s < 20; ++s) { KingQueue d; d.Start(p, s); differs = differs || d.order() != a.order(); }
    CHECK(differs);
    CHECK(a.order().size() == 6);
    KingQueue bad;
    CHECK(!bad.Start({1, 1}, 0) && !bad.Start({0, 1}, 0) && bad.order().empty());
}

static void TestWire() {
    CHECK(ValidQueueWire({}, 0));
    CHECK(ValidQueueWire({3, 1, 2}, 0) && ValidQueueWire({3, 1, 2}, 4));
    CHECK(!ValidQueueWire({3, 1, 3}, 0));
    CHECK(!ValidQueueWire({3, 0, 1}, 0));
    CHECK(!ValidQueueWire({}, 1));                                  // a streak with nobody
    CHECK(!ValidQueueWire(V(17, 1), 0));
    KingQueue q = Of({1, 2, 3});
    q.Report(2); q.Report(2);
    KingQueue r;
    CHECK(r.Restore(q.order(), q.streak()));
    CHECK(r.order() == q.order() && r.streak() == q.streak() && r.King() == 2);
}

int main() {
    TestWinnerStays();
    TestTwoPlayersRepeat();
    TestStreak();
    TestDraw();
    TestLeaversAndJoiners();
    TestLoserGoesBehindTheWaiting();
    TestDeterminism();
    TestWire();
    if (g_fail) { std::printf("%d failure(s)\n", g_fail); return 1; }
    std::puts("ok");
    return 0;
}
