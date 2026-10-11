// World object sync: wire format (round trip, truncation, non-finite, duplicate ids, unknown kinds, cap),
// host tracker (id stability and reuse, tombstones, sleeping cadence, distance priority), client view
// (stale and wrapped ticks, create/remove reports).
#include "session/hsm_world.h"
#include <cstdio>
#include <limits>

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++g_fail; } } while (0)
using namespace hsm;

static WorldObject Obj(uint16_t id, WorldKind kind, float x, uint8_t flags = 0) {
    WorldObject o; o.id = id; o.kind = uint8_t(kind); o.px = x; o.py = 1.5f; o.pz = -2.f;
    o.rot = PackQuat(0, 0, 0.7071f, 0.7071f); o.vx = 0.25f; o.vy = 0; o.vz = -1; o.flags = flags;
    return o;
}
static WorldSample Smp(uint64_t key, float x, bool sleeping = false) {
    WorldSample s; s.key = key; s.px = x; s.py = 1.5f; s.pz = -2.f; s.vx = 0.25f; s.sleeping = sleeping;
    return s;
}
static bool Dec(const std::vector<uint8_t>& b, WorldState& s) { return Decode(b.data(), b.size(), s); }
static const WorldObject* ById(const WorldState& s, uint16_t id) {
    for (const WorldObject& o : s.objects) if (o.id == id) return &o;
    return nullptr;
}
static uint16_t IdAtX(const WorldState& s, float x) {
    for (const WorldObject& o : s.objects) if (o.px == x && !(o.flags & kWorldRemoved)) return o.id;
    return 0;
}
static int TombCount(const WorldState& s, uint16_t id) {
    const WorldObject* o = ById(s, id);
    return (o && (o->flags & kWorldRemoved)) ? 1 : 0;
}

// ---------------------------------------------------------------- wire format
static void TestWeaponBindingTravels() {
    WorldTracker t; std::vector<WorldPos> none;
    WorldSample s; s.key = 0x0305; s.kind = WorldKind::Weapon; s.owner = 3; s.slot = 5;
    WorldState out = t.Update(1, {s}, none);
    std::vector<uint8_t> bytes = Encode(out);
    WorldState back;
    CHECK(Decode(bytes.data(), bytes.size(), back) && back.objects.size() == 1);
    CHECK(back.objects[0].owner == 3 && back.objects[0].slot == 5);           // the client binds the puppet's weapon by these
}

static void TestRoundTrip() {
    WorldState st; st.tick = 4242;
    st.objects.push_back(Obj(1, WorldKind::Weapon, 1.25f, kWorldHeld));
    st.objects.push_back(Obj(2, WorldKind::Prop, -7.5f, kWorldSleeping));
    st.objects.push_back(Obj(3, WorldKind::Prop, 100.f, 0));
    WorldObject tomb; tomb.id = 9; tomb.flags = kWorldRemoved; st.objects.push_back(tomb);
    auto b = Encode(st); WorldState o;
    CHECK(Dec(b, o));
    CHECK(o.tick == 4242 && o.objects.size() == 4);
    for (size_t i = 0; i < 3; ++i) {
        const WorldObject& a = st.objects[i]; const WorldObject& c = o.objects[i];
        CHECK(a.id == c.id && a.kind == c.kind && a.px == c.px && a.py == c.py && a.pz == c.pz);
        CHECK(a.rot.x == c.rot.x && a.rot.y == c.rot.y && a.rot.z == c.rot.z && a.rot.w == c.rot.w);
        CHECK(a.vx == c.vx && a.vy == c.vy && a.vz == c.vz && a.flags == c.flags);
    }
    CHECK(o.objects[3].id == 9 && o.objects[3].flags == kWorldRemoved);
    CHECK(b.size() == kWorldHeaderBytes + 4 * kWorldObjectBytes);
    WorldState empty; empty.tick = 5; auto eb = Encode(empty); CHECK(Dec(eb, o) && o.tick == 5 && o.objects.empty());
}

static void TestTruncationAndTrailing() {
    WorldState st; st.tick = 1;
    st.objects.push_back(Obj(1, WorldKind::Prop, 2.f)); st.objects.push_back(Obj(2, WorldKind::Weapon, 3.f, kWorldSleeping));
    auto b = Encode(st); WorldState o;
    for (size_t cut = 0; cut < b.size(); ++cut) CHECK(!Decode(b.data(), cut, o));
    auto extra = b; extra.push_back(0); CHECK(!Dec(extra, o));
    auto badRev = b; badRev[2] = uint8_t(kProtocolRevision + 1); CHECK(!Dec(badRev, o));
    auto badType = b; badType[3] = uint8_t(MsgType::Snapshot); CHECK(!Dec(badType, o));
    CHECK(Dec(b, o));
}

static void TestNonFiniteRejected() {
    WorldState st; st.tick = 1; st.objects.push_back(Obj(1, WorldKind::Prop, 0.f));
    WorldState o; CHECK(Dec(Encode(st), o));
    const float nan = std::numeric_limits<float>::quiet_NaN(), inf = std::numeric_limits<float>::infinity();
    WorldState a = st; a.objects[0].px = nan; CHECK(!Dec(Encode(a), o));
    WorldState b = st; b.objects[0].pz = inf; CHECK(!Dec(Encode(b), o));
    WorldState c = st; c.objects[0].vy = nan; CHECK(!Dec(Encode(c), o));
    WorldState d = st; d.objects[0].vz = -inf; CHECK(!Dec(Encode(d), o));
}

static void TestMalformedObjectsRejected() {
    WorldState st; st.tick = 1;
    st.objects.push_back(Obj(4, WorldKind::Prop, 0.f)); st.objects.push_back(Obj(5, WorldKind::Weapon, 1.f));
    WorldState o;
    WorldState dup = st; dup.objects[1].id = 4; CHECK(!Dec(Encode(dup), o));                       // duplicate id
    WorldState kind = st; kind.objects[1].kind = uint8_t(WorldKind::Count); CHECK(!Dec(Encode(kind), o));   // unknown kind
    WorldState zero = st; zero.objects[1].id = 0; CHECK(!Dec(Encode(zero), o));                    // id 0 reserved
    WorldState flags = st; flags.objects[1].flags = 8; CHECK(!Dec(Encode(flags), o));              // unknown flag bit
    WorldState tomb = st; tomb.objects[1].flags = kWorldRemoved | kWorldHeld; CHECK(!Dec(Encode(tomb), o));  // tombstone with state
    WorldState ok = st; ok.objects[1].flags = kWorldRemoved; CHECK(Dec(Encode(ok), o) && o.objects[1].flags == kWorldRemoved);
}

static void TestCapAndPacketSize() {
    WorldState full; full.tick = 7;
    for (size_t i = 0; i < kMaxWorldObjects; ++i) full.objects.push_back(Obj(uint16_t(i + 1), WorldKind::Prop, float(i)));
    auto b = Encode(full); WorldState o;
    CHECK(b.size() == kWorldHeaderBytes + kMaxWorldObjects * kWorldObjectBytes && b.size() <= kMaxPacket);
    CHECK(Dec(b, o) && o.objects.size() == kMaxWorldObjects);
    WorldState over = full; over.objects.push_back(Obj(999, WorldKind::Prop, 5.f));
    auto ob = Encode(over); CHECK(Dec(ob, o) && o.objects.size() == kMaxWorldObjects);   // encoder clamps to the cap
    // A packet whose count claims cap+1 objects is rejected even though the bytes for one more object are present.
    WorldState one; one.tick = 7; one.objects.push_back(Obj(999, WorldKind::Prop, 5.f));
    auto single = Encode(one);
    auto raw = b;
    raw[8] = uint8_t((kMaxWorldObjects + 1) & 0xFF); raw[9] = uint8_t((kMaxWorldObjects + 1) >> 8);
    raw.insert(raw.end(), single.begin() + long(kWorldHeaderBytes), single.end());
    CHECK(!Dec(raw, o));
    std::vector<uint8_t> huge(kMaxPacket + 1, 0); CHECK(!Dec(huge, o));
}

// ---------------------------------------------------------------- host tracker
static void TestTrackerIdStabilityAndReuse() {
    WorldTracker t; std::vector<WorldPos> none;
    WorldState s1 = t.Update(1, {Smp(100, 0.f), Smp(200, 1.f)}, none);
    uint16_t a = IdAtX(s1, 0.f), b = IdAtX(s1, 1.f);
    CHECK(a != 0 && b != 0 && a != b && s1.objects.size() == 2);
    WorldState s1b = t.Update(2, {Smp(100, 0.f), Smp(200, 1.f)}, none);       // same keys again: ids unchanged
    CHECK(IdAtX(s1b, 0.f) == a && IdAtX(s1b, 1.f) == b && t.tracked() == 2);

    WorldState s2 = t.Update(2, {Smp(200, 1.f)}, none);                       // key 100 removed: tombstone for a
    CHECK(IdAtX(s2, 1.f) == b && TombCount(s2, a) == 1);
    WorldState s3 = t.Update(3, {Smp(200, 1.f), Smp(300, 3.f)}, none);        // tombstone still sent: a not reused
    uint16_t c = IdAtX(s3, 3.f);
    CHECK(c != 0 && c != a && c != b && TombCount(s3, a) == 1);
    WorldState s4 = t.Update(4, {Smp(200, 1.f), Smp(300, 3.f)}, none);        // third and last tombstone send
    CHECK(TombCount(s4, a) == 1 && t.pendingTombstones() == 0);
    WorldState s5 = t.Update(5, {Smp(200, 1.f), Smp(300, 3.f), Smp(400, 4.f)}, none);   // freed id reused (lowest)
    CHECK(TombCount(s5, a) == 0 && IdAtX(s5, 4.f) == a && IdAtX(s5, 3.f) == c);
    CHECK(t.tracked() == 3);
}

static void TestSleepingCadenceAndMovingEveryTick() {
    for (uint32_t start : {1u, 0xFFFFFFFFu}) {       // second run crosses the uint32 tick wrap
        WorldTracker t; std::vector<WorldPos> none;
        std::vector<uint32_t> sentAt; uint16_t sleepId = 0; bool movingEveryTick = true;
        for (uint32_t i = 0; i <= 22; ++i) {
            float sx = i >= 11 ? 5.f : 0.f;          // sleeping object moves once at i=11
            bool asleep = i < 22;                    // and wakes at i=22
            WorldState s = t.Update(start + i, {Smp(7, sx, asleep), Smp(8, 50.f)}, none);
            if (i == 0) sleepId = IdAtX(s, 0.f);
            if (IdAtX(s, 50.f) == 0) movingEveryTick = false;
            if (sleepId && ById(s, sleepId)) sentAt.push_back(i);
        }
        CHECK(sleepId != 0 && movingEveryTick);
        CHECK((sentAt == std::vector<uint32_t>{0, 10, 11, 21, 22}));   // first, resend at +10, change, resend, wake
    }
}

static void TestDistancePriority() {
    WorldTracker t; std::vector<WorldPos> players{{0.f, 0.f, 0.f}};
    std::vector<WorldSample> live;
    for (uint64_t k = 0; k < kMaxWorldObjects + 2; ++k) live.push_back(Smp(k, float(k)));
    WorldState s = t.Update(1, live, players);
    CHECK(s.objects.size() == kMaxWorldObjects);
    bool farExcluded = true;
    for (const WorldObject& o : s.objects) if (o.px >= float(kMaxWorldObjects)) farExcluded = false;
    CHECK(farExcluded);                                                 // the two farthest did not fit
    // Next tick key 0 is removed (its tombstone must go out first, even though it was nearest) and the rest stay.
    std::vector<WorldSample> live2;
    for (uint64_t k = 1; k < kMaxWorldObjects + 2; ++k) live2.push_back(Smp(k, float(k)));
    WorldState s2 = t.Update(2, live2, players);
    CHECK(s2.objects.size() == kMaxWorldObjects);
    CHECK(TombCount(s2, IdAtX(s, 0.f)) == 1);
    bool nearestKept = true;
    for (const WorldObject& o : s2.objects) if (!(o.flags & kWorldRemoved) && o.px >= float(kMaxWorldObjects)) nearestKept = false;
    CHECK(IdAtX(s2, 127.f) != 0);
    CHECK(nearestKept);                                                 // 1..127 kept, 128 and 129 dropped
}

static void TestHostOutputAlwaysDecodes() {
    WorldTracker t; std::vector<WorldPos> none;
    WorldSample nanSample = Smp(1, std::numeric_limits<float>::quiet_NaN());
    WorldSample badKind = Smp(3, 4.f); badKind.kind = WorldKind::Count;
    WorldState s = t.Update(1, {nanSample, Smp(2, 3.f), badKind, Smp(2, 9.f)}, none);
    CHECK(s.objects.size() == 1 && s.objects[0].px == 3.f);             // bad samples skipped, duplicate key: first wins
    WorldState o; CHECK(Dec(Encode(s), o));
}

// ---------------------------------------------------------------- client view
static void TestViewCreateUpdateRemove() {
    WorldView v;
    WorldState s10; s10.tick = 10; s10.objects.push_back(Obj(5, WorldKind::Prop, 1.f));
    WorldView::Changes c1 = v.Apply(s10);
    CHECK(c1.created.size() == 1 && c1.created[0] == 5 && c1.removed.empty());
    WorldState s11; s11.tick = 11; s11.objects.push_back(Obj(5, WorldKind::Prop, 2.f));
    WorldView::Changes c2 = v.Apply(s11);
    CHECK(c2.created.empty() && c2.removed.empty() && v.Find(5) && v.Find(5)->px == 2.f);
    WorldState s12; s12.tick = 12; WorldObject tomb; tomb.id = 5; tomb.flags = kWorldRemoved; s12.objects.push_back(tomb);
    WorldView::Changes c3 = v.Apply(s12);
    CHECK(c3.removed.size() == 1 && c3.removed[0] == 5 && !v.Find(5) && v.size() == 0);
    WorldView::Changes c4 = v.Apply(s12);                               // repeated tombstone: nothing new
    CHECK(c4.removed.empty() && c4.created.empty());
    WorldState s9; s9.tick = 9; WorldObject tomb9; tomb9.id = 77; tomb9.flags = kWorldRemoved; s9.objects.push_back(tomb9);
    CHECK(v.Apply(s9).removed.empty());                                 // tombstone for an unknown id is ignored
}

static void TestViewIgnoresStaleTicks() {
    WorldView v;
    WorldState s20; s20.tick = 20; s20.objects.push_back(Obj(6, WorldKind::Prop, 1.f));
    v.Apply(s20);
    WorldState s19; s19.tick = 19; s19.objects.push_back(Obj(6, WorldKind::Prop, 9.f));
    WorldState s20b; s20b.tick = 20; s20b.objects.push_back(Obj(6, WorldKind::Prop, 8.f));
    v.Apply(s19); v.Apply(s20b);
    CHECK(v.Find(6) && v.Find(6)->px == 1.f);                           // older and equal ticks ignored
    WorldState s40; s40.tick = 40; s40.objects.push_back(Obj(7, WorldKind::Prop, 1.f)); v.Apply(s40);
    WorldState s39; s39.tick = 39; WorldObject tomb; tomb.id = 7; tomb.flags = kWorldRemoved; s39.objects.push_back(tomb);
    CHECK(v.Apply(s39).removed.empty() && v.Find(7));                   // stale tombstone does not remove a newer object
}

static void TestViewWrapAround() {
    WorldView v;
    WorldState a; a.tick = 0xFFFFFFF0u; a.objects.push_back(Obj(8, WorldKind::Prop, 1.f)); v.Apply(a);
    WorldState b; b.tick = 4; b.objects.push_back(Obj(8, WorldKind::Prop, 2.f)); v.Apply(b);      // 20 ticks later across the wrap
    CHECK(v.Find(8) && v.Find(8)->px == 2.f);
    WorldState c; c.tick = 0xFFFFFFF8u; c.objects.push_back(Obj(8, WorldKind::Prop, 3.f)); v.Apply(c);   // older across the wrap
    CHECK(v.Find(8) && v.Find(8)->px == 2.f);
    WorldState d; d.tick = 5; WorldObject tomb; tomb.id = 8; tomb.flags = kWorldRemoved; d.objects.push_back(tomb);
    CHECK(v.Apply(d).removed.size() == 1 && !v.Find(8));
}

static void TestTrackerToViewEndToEnd() {
    // Host packets go through Encode/Decode into the view; a removed object is removed on the client.
    WorldTracker t; WorldView v; std::vector<WorldPos> none;
    WorldState s1 = t.Update(1, {Smp(1, 1.f), Smp(2, 2.f)}, none);
    WorldState s1d; CHECK(Dec(Encode(s1), s1d)); v.Apply(s1d);
    CHECK(v.size() == 2);
    WorldState s2 = t.Update(2, {Smp(2, 2.f)}, none);
    WorldState s2d; CHECK(Dec(Encode(s2), s2d)); v.Apply(s2d);
    CHECK(v.size() == 1 && v.Find(2) && v.Find(2)->px == 2.f);
}

int main() {
    TestWeaponBindingTravels();
    TestRoundTrip(); TestTruncationAndTrailing(); TestNonFiniteRejected(); TestMalformedObjectsRejected();
    TestCapAndPacketSize(); TestTrackerIdStabilityAndReuse(); TestSleepingCadenceAndMovingEveryTick();
    TestDistancePriority(); TestHostOutputAlwaysDecodes(); TestViewCreateUpdateRemove();
    TestViewIgnoresStaleTicks(); TestViewWrapAround(); TestTrackerToViewEndToEnd();
    std::printf(g_fail ? "%d FAILED\n" : "all world tests passed\n", g_fail);
    return g_fail ? 1 : 0;
}
