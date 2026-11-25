// BodyStateView A-B-A: a former streamer's late state must not apply again after the stream changed hands twice.
// Run via implementation/shared/run_tests.sh test_hsm_lease_aba.
#include "session/hsm_lease.h"
#include <cstdio>

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++g_fail; } } while (0)
using namespace hsm;

static BodyStateMsg State(uint64_t id, uint8_t lessee, uint32_t t) {
    BodyStateMsg m;
    m.objId = id;
    m.lessee = lessee;
    m.senderTimeMs = t;
    return m;
}

int main() {
    const uint64_t id = MakeWeaponPropId(3, 5);
    BodyStateView v;
    v.OnGrant(id, 2, LeaseLevel::Touch);
    CHECK(v.Apply(State(id, 2, 5000)));
    v.OnGrant(id, 3, LeaseLevel::Touch);
    CHECK(v.Apply(State(id, 3, 90)));            // B's clock is its own
    v.OnGrant(id, 2, LeaseLevel::Touch);          // back to A
    CHECK(!v.Apply(State(id, 2, 4000)));          // A's late packet from before: dropped
    CHECK(!v.Apply(State(id, 2, 5000)));          // a duplicate of A's last: dropped
    CHECK(v.Apply(State(id, 2, 5100)));           // A's new one
    CHECK(!v.Apply(State(id, 2, 5050)));          // and the usual staleness
    v.OnGrant(id, 4, LeaseLevel::Touch);          // a streamer never heard from: first state applies whatever its clock
    CHECK(v.Apply(State(id, 4, 1)));
    // Why a state was dropped: a repeat of the same time stamp is a duplicate, an older one is out of order.
    {
        BodyStateView w;
        const uint64_t oid = MakeWeaponPropId(3, 6);
        CHECK(w.Check(State(oid, 0, 100)) == StateVerdict::Applied);
        CHECK(w.Check(State(oid, 0, 100)) == StateVerdict::Duplicate);
        CHECK(w.Check(State(oid, 0, 90)) == StateVerdict::OutOfOrder);
        CHECK(w.Check(State(oid, 0, 110)) == StateVerdict::Applied);
        CHECK(w.Check(State(oid, 0, 100)) == StateVerdict::OutOfOrder);   // older than the newest, though seen before
        CHECK(w.Check(State(oid, 5, 120)) == StateVerdict::NotStreamer);
        w.OnGrant(oid, 2, LeaseLevel::Touch);                              // the A-B-A memory classifies too
        CHECK(w.Check(State(oid, 2, 1)) == StateVerdict::Applied);
        w.OnGrant(oid, 0, LeaseLevel::Free);
        CHECK(w.Check(State(oid, 0, 110)) == StateVerdict::Duplicate);
        CHECK(w.Check(State(oid, 0, 105)) == StateVerdict::OutOfOrder);
    }
    if (g_fail) { std::printf("%d failure(s)\n", g_fail); return 1; }
    std::printf("test_hsm_lease_aba: ok\n");
    return 0;
}
