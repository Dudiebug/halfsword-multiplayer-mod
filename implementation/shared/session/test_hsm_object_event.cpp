// ObjectEvent (session/hsm_object_event.h, rev 26): round trip per kind, wire sizes, every truncation, strict rejections.
#include "session/hsm_object_event.h"
#include <cstdio>

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++g_fail; } } while (0)
using namespace hsm;

static ObjectEventMsg Ev(ObjectEventKind k) {
    ObjectEventMsg m; m.objId = MakeSpawnedId(2, 9); m.body = 3; m.kind = k;
    if (k == ObjectEventKind::JointBroke) m.constraint = 7;
    if (k == ObjectEventKind::Detached) m.otherId = MakeSpawnedId(0, 4);
    if (k == ObjectEventKind::Attached) m.otherId = MakeLevelPropId(12);
    if (k == ObjectEventKind::Custom) { m.modId = 0xC0FFEEu; m.data = {1, 2, 3, 250}; }
    return m;
}
static bool Dec(const std::vector<uint8_t>& b, ObjectEventMsg& o) { return Decode(b.data(), b.size(), o); }

static void TestRoundTrips() {
    const struct { ObjectEventKind k; size_t size; } kinds[] = {
        {ObjectEventKind::JointBroke, 15}, {ObjectEventKind::Detached, 22}, {ObjectEventKind::Attached, 22},
        {ObjectEventKind::Destroyed, 14}, {ObjectEventKind::Custom, 19 + 4}};
    for (const auto& k : kinds) {
        const ObjectEventMsg m = Ev(k.k);
        const auto b = Encode(m);
        CHECK(b.size() == k.size && b[3] == 47);
        ObjectEventMsg o;
        CHECK(Dec(b, o));
        CHECK(o.objId == m.objId && o.body == 3 && o.kind == k.k && o.constraint == m.constraint && o.otherId == m.otherId &&
              o.modId == m.modId && o.data == m.data);
        CHECK(Encode(o) == b);
        for (size_t n = 0; n < b.size(); ++n) { ObjectEventMsg t; CHECK(!Decode(b.data(), n, t)); }   // every truncation
        auto longer = b; longer.push_back(0); CHECK(!Dec(longer, o));
        auto wrongRev = b; wrongRev[2] = uint8_t(kProtocolRevision + 1); CHECK(!Dec(wrongRev, o));
    }
    // The three id kinds as the object; body and constraint extremes.
    for (uint64_t id : {MakeWeaponPropId(3, 4), MakeLevelPropId(0xFFFF), MakeSpawnedId(8, 0xFFFFFFFFu)}) {
        ObjectEventMsg m = Ev(ObjectEventKind::Attached); m.objId = id; m.otherId = MakeSpawnedId(1, 1);
        ObjectEventMsg o; CHECK(Dec(Encode(m), o) && o.objId == id);
    }
    ObjectEventMsg m = Ev(ObjectEventKind::JointBroke); m.body = kMaxObjectBodies - 1; m.constraint = kMaxConstraintIndex;
    ObjectEventMsg o; CHECK(Dec(Encode(m), o) && o.body == kMaxObjectBodies - 1 && o.constraint == kMaxConstraintIndex);
    // Custom data: empty and the largest size round-trip; the encoder truncates anything longer.
    m = Ev(ObjectEventKind::Custom); m.data.clear(); CHECK(Dec(Encode(m), o) && o.data.empty() && Encode(m).size() == 19);
    m.data.assign(kMaxCustomEventBytes, 0xEE); CHECK(Dec(Encode(m), o) && o.data.size() == kMaxCustomEventBytes);
    m.data.assign(kMaxCustomEventBytes + 10, 0xEE); CHECK(Dec(Encode(m), o) && o.data.size() == kMaxCustomEventBytes);
}

static void TestRejections() {
    auto bad = [](const ObjectEventMsg& m) { ObjectEventMsg o; return !Dec(Encode(m), o); };
    ObjectEventMsg m = Ev(ObjectEventKind::Destroyed);
    m.objId = 0; CHECK(bad(m));
    m.objId = (uint64_t(1) << 62) | (uint64_t(1) << 40); CHECK(bad(m));
    m.objId = (uint64_t(1) << 62) | (uint64_t(kMaxFighters) << 56); CHECK(bad(m));
    m = Ev(ObjectEventKind::Destroyed); m.body = kMaxObjectBodies; CHECK(bad(m));
    m = Ev(ObjectEventKind::JointBroke); m.constraint = kMaxConstraintIndex + 1; CHECK(bad(m));
    m = Ev(ObjectEventKind::Detached); m.otherId = 0; CHECK(bad(m)); m.otherId = m.objId; CHECK(bad(m)); m.otherId = (uint64_t(1) << 62) | (uint64_t(1) << 33); CHECK(bad(m));
    m = Ev(ObjectEventKind::Attached); m.otherId = 0; CHECK(bad(m)); m.otherId = m.objId; CHECK(bad(m));
    m = Ev(ObjectEventKind::Custom); m.modId = 0; CHECK(bad(m));
    // Raw bytes: an unknown kind, a Custom length past the cap or past the end.
    ObjectEventMsg o;
    auto b = Encode(Ev(ObjectEventKind::Destroyed));
    b[13] = 0; CHECK(!Dec(b, o)); b[13] = 6; CHECK(!Dec(b, o)); b[13] = 255; CHECK(!Dec(b, o)); b[13] = 4; CHECK(Dec(b, o));
    b = Encode(Ev(ObjectEventKind::Custom));
    b[18] = uint8_t(kMaxCustomEventBytes + 1); CHECK(!Dec(b, o));
    b[18] = 5; CHECK(!Dec(b, o));                                  // claims 5 bytes, has 4
    b[18] = 3; CHECK(!Dec(b, o));                                  // claims 3, has 4: a trailing byte
    b[18] = 4; CHECK(Dec(b, o));
    // A length past the cap with the bytes really there is still refused.
    b = Encode(Ev(ObjectEventKind::Custom)); b.resize(19); b[18] = uint8_t(kMaxCustomEventBytes + 1); b.resize(19 + kMaxCustomEventBytes + 1, 9);
    CHECK(!Dec(b, o)); b[18] = uint8_t(kMaxCustomEventBytes); b.pop_back(); CHECK(Dec(b, o));
}

int main() {
    TestRoundTrips(); TestRejections();
    if (g_fail) { std::printf("%d FAILED\n", g_fail); return 1; }
    std::printf("all object event tests passed\n");
    return 0;
}
