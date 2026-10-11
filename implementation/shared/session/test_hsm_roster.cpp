// AC-1: roster assigns at most 6 unique slots/entities by join order, reuses freed slots, rejects a 7th member.
#include "session/hsm_roster.h"
#include <cstdio>
#include <set>

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++g_fail; } } while (0)
using namespace hsm;

static void TestJoinOrderAndLimit() {
    Roster r;
    for (uint64_t k = 100; k < 106; ++k) {
        auto slot = r.Add(k, "p" + std::to_string(k));
        CHECK(slot.has_value() && *slot == k - 100);
    }
    CHECK(r.size() == kMaxFighters);
    CHECK(!r.Add(999, "seventh").has_value());          // 7th member rejected
    std::set<uint8_t> entities;
    for (const RosterMember& m : r.members()) { entities.insert(m.entity); CHECK(m.entity == m.slot + 1); }
    CHECK(entities.size() == kMaxFighters);                // unique entities 1..6
}

static void TestIdempotentAndReuse() {
    Roster r;
    CHECK(*r.Add(1, "host") == 0);
    CHECK(*r.Add(2, "a") == 1);
    CHECK(*r.Add(3, "b") == 2);
    CHECK(*r.Add(2, "a-again") == 1);                      // same key keeps its slot
    CHECK(r.size() == 3);
    CHECK(r.Remove(2) && !r.Remove(2));
    CHECK(*r.Add(4, "c") == 1);                            // lowest free slot reused
    CHECK(r.ByEntity(2) && r.ByEntity(2)->key == 4);
    CHECK(!r.ByEntity(7) && !r.Find(2) && r.BySlot(2)->key == 3);
}

int main() {
    TestJoinOrderAndLimit(); TestIdempotentAndReuse();
    std::printf(g_fail ? "%d FAILED\n" : "all roster tests passed\n", g_fail);
    return g_fail ? 1 : 0;
}
