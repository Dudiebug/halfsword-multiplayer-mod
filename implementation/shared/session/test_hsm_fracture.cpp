// FractureLog and FractureRate (session/hsm_fracture.h): dedupe, order, caps, joiner replay, send rate cap.
#include "session/hsm_fracture.h"
#include <cstdio>

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++g_fail; } } while (0)
using namespace hsm;

static ObjectEventMsg Fx(uint64_t obj, uint8_t body, uint16_t item) {
    ObjectEventMsg m; m.objId = obj; m.body = body; m.kind = ObjectEventKind::Fracture; m.item = item;
    m.x = 100; m.y = -200; m.z = 30; m.radius = 50; m.strain = 1.0e6f;
    return m;
}

static void TestLog() {
    FractureLog log;
    const uint64_t a = MakeLevelPropId(3), b = MakeSpawnedId(0, 5);
    CHECK(log.Record(Fx(a, 1, 4)));
    CHECK(!log.Record(Fx(a, 1, 4)));                    // the same cluster of the same body: once
    CHECK(log.Record(Fx(a, 1, 5)));                     // another cluster
    CHECK(log.Record(Fx(a, 2, 4)));                     // another body, same item
    CHECK(log.Record(Fx(b, 1, 4)));                     // another object
    CHECK(log.Size() == 4 && log.Objects() == 2);
    CHECK(log.Has(a, 1, 5) && !log.Has(a, 3, 5) && !log.Has(b, 1, 5));
    const auto of = log.Of(a);
    CHECK(of.size() == 3 && of[0].item == 4 && of[1].item == 5 && of[2].body == 2);
    // Joiner replay: every one, encoded, in the order recorded, and each decodes to what was recorded.
    const auto j = log.ForJoiner();
    CHECK(j.size() == 4);
    ObjectEventMsg o;
    CHECK(Decode(j[0].data(), j[0].size(), o) && o.objId == a && o.item == 4);
    CHECK(Decode(j[3].data(), j[3].size(), o) && o.objId == b);
    // Not a fracture, or an invalid object: refused.
    ObjectEventMsg d = Fx(a, 0, 1); d.kind = ObjectEventKind::Destroyed; CHECK(!log.Record(d));
    CHECK(!log.Record(Fx(0, 0, 1)));
    log.Forget(a);
    CHECK(log.Size() == 1 && log.Objects() == 1 && !log.Has(a, 1, 4));
    log.Forget(a); CHECK(log.Objects() == 1);           // forgetting twice changes nothing
    log.Clear(); CHECK(log.Size() == 0 && log.Objects() == 0);
}

static void TestCaps() {
    FractureLog log;
    const uint64_t a = MakeLevelPropId(1);
    for (size_t i = 0; i < kMaxFracturesPerObject; ++i) CHECK(log.Record(Fx(a, 0, uint16_t(i))));
    CHECK(!log.Record(Fx(a, 0, uint16_t(kMaxFracturesPerObject))));   // the object is full; the first breaks stay
    CHECK(log.Has(a, 0, 0) && log.Size() == kMaxFracturesPerObject);
    FractureLog many;
    for (size_t i = 0; i < kMaxFractureObjects; ++i) CHECK(many.Record(Fx(MakeLevelPropId(uint16_t(i)), 0, 1)));
    CHECK(!many.Record(Fx(MakeLevelPropId(uint16_t(kMaxFractureObjects)), 0, 1)));
    CHECK(many.Record(Fx(MakeLevelPropId(0), 0, 2)));                 // a known object still takes more
    CHECK(many.Objects() == kMaxFractureObjects);
}

static void TestRate() {
    FractureRate r;
    const uint64_t a = MakeLevelPropId(1), b = MakeLevelPropId(2);
    for (int i = 0; i < kFractureBurst; ++i) CHECK(r.Allow(a, 1000.0 + i));
    CHECK(!r.Allow(a, 1500.0));                         // over the burst inside the window
    CHECK(r.Allow(b, 1500.0));                          // per object
    CHECK(r.Allow(a, 1000.0 + kFractureWindowMs));      // the window passed
    CHECK(r.Allow(a, 900.0));                           // the clock went back: a new window, not a stuck one
    r.Clear();
    for (int i = 0; i < kFractureBurst; ++i) CHECK(r.Allow(a, 0.0));
    CHECK(!r.Allow(a, 0.0));
    // A bounded table: one past the cap for new objects is refused.
    FractureRate big;
    for (size_t i = 0; i < kMaxFractureObjects; ++i) CHECK(big.Allow(MakeLevelPropId(uint16_t(i)), 0.0));
    CHECK(!big.Allow(MakeLevelPropId(uint16_t(kMaxFractureObjects)), 0.0));
}

int main() {
    TestLog(); TestCaps(); TestRate();
    if (g_fail) { std::printf("%d FAILED\n", g_fail); return 1; }
    std::printf("all fracture tests passed\n");
    return 0;
}
