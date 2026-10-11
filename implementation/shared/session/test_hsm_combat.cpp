// Combat event wire format and dismemberment diffing: round trips, strict decoding, and diff semantics.
#include "session/hsm_combat.h"
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++g_fail; } } while (0)
using namespace hsm;

static CombatEvent Sample(uint8_t kind) {
    CombatEvent e;
    e.kind = kind; e.entity = 3; e.partsMask = (1u << 2) | (1u << 14);
    e.x = 12.5f; e.y = -340.25f; e.z = 1e6f; e.bone = 9; e.size = 4; e.sharp = 1;
    return e;
}

static void TestRoundTrip() {
    for (uint8_t kind : {kCombatDismember, kCombatBlood}) {
        CombatEvent in = Sample(kind);
        std::vector<uint8_t> wire = Encode(in);
        CHECK(wire.size() == 4 + 1 + 1 + 4 + 12 + 3);          // header + body
        MsgType t{};
        CHECK(PeekType(wire.data(), wire.size(), t) && t == MsgType::CombatEvent);
        CombatEvent out;
        CHECK(Decode(wire.data(), wire.size(), out));
        CHECK(out.kind == in.kind && out.entity == in.entity && out.partsMask == in.partsMask);
        CHECK(out.x == in.x && out.y == in.y && out.z == in.z);
        CHECK(out.bone == in.bone && out.size == in.size && out.sharp == in.sharp);
    }
    CombatEvent zero; zero.entity = 1;                          // zero location, sharp 0 is valid
    std::vector<uint8_t> wire = Encode(zero);
    CombatEvent out;
    CHECK(Decode(wire.data(), wire.size(), out) && out.partsMask == 0 && out.sharp == 0);
}

static void TestRejectsBadFields() {
    CombatEvent base = Sample(kCombatBlood);
    auto rejects = [](const CombatEvent& e) {
        std::vector<uint8_t> wire = Encode(e);
        CombatEvent out;
        return !Decode(wire.data(), wire.size(), out);
    };
    CombatEvent m = base; m.kind = 2;                 CHECK(rejects(m));   // unknown kind
    m = base; m.kind = 255;                           CHECK(rejects(m));
    m = base; m.entity = 0;                           CHECK(rejects(m));   // entity 0 is never valid
    m = base; m.partsMask = 1u << 15;                 CHECK(rejects(m));   // no such Enum_DismembermentPart
    m = base; m.partsMask = 0xFFFFFFFFu;              CHECK(rejects(m));
    m = base; m.sharp = 2;                            CHECK(rejects(m));
    m = base; m.x = std::numeric_limits<float>::quiet_NaN();      CHECK(rejects(m));
    m = base; m.y = std::numeric_limits<float>::infinity();       CHECK(rejects(m));
    m = base; m.z = -std::numeric_limits<float>::infinity();      CHECK(rejects(m));
    m = base; m.x = 2e7f;                             CHECK(rejects(m));   // beyond the coordinate bound
    m = base; m.x = kCombatMaxCoord;                  CHECK(!rejects(m));  // at the bound is fine
    m = base; m.partsMask = kCombatPartMask;          CHECK(!rejects(m));
}

static void TestMalformedWire() {
    std::vector<uint8_t> wire = Encode(Sample(kCombatDismember));
    CombatEvent out;
    // every strict prefix is a truncation and must fail
    for (size_t len = 0; len < wire.size(); ++len) CHECK(!Decode(wire.data(), len, out));
    // trailing bytes must fail
    std::vector<uint8_t> extra = wire; extra.push_back(0);
    CHECK(!Decode(extra.data(), extra.size(), out));
    // wrong message type and bad magic/revision must fail
    std::vector<uint8_t> other = wire; other[3] = uint8_t(MsgType::Kill);
    CHECK(!Decode(other.data(), other.size(), out));
    std::vector<uint8_t> badMagic = wire; badMagic[0] ^= 0xFF;
    CHECK(!Decode(badMagic.data(), badMagic.size(), out));
    std::vector<uint8_t> badRev = wire; badRev[2] ^= 0x01;
    CHECK(!Decode(badRev.data(), badRev.size(), out));
    // a NaN bit pattern on the wire is rejected even though the length is right
    std::vector<uint8_t> nan = wire;
    const uint32_t nanBits = 0x7FC00000u;
    for (int i = 0; i < 4; ++i) nan[4 + 1 + 1 + 4 + i] = uint8_t(nanBits >> (8 * i));   // x
    CHECK(!Decode(nan.data(), nan.size(), out));
    // rewriting the kind byte to blood (1) keeps the event valid, so only the field values decide acceptance
    std::vector<uint8_t> blood = wire; blood[4] = kCombatBlood;
    CHECK(Decode(blood.data(), blood.size(), out) && out.kind == kCombatBlood);
    // full truncation loop on a blood event too
    std::vector<uint8_t> bw = Encode(Sample(kCombatBlood));
    for (size_t len = 0; len < bw.size(); ++len) CHECK(!Decode(bw.data(), len, out));
}

static void TestDismemberDiff() {
    SeveredMasks reported;
    SeveredMasks now;
    now[1] = 0;
    CHECK(DismemberDiff(reported, now).empty());                    // nothing severed yet

    now[1] = 1u << 3;                                               // part 3 severed on entity 1
    auto ev = DismemberDiff(reported, now);
    CHECK(ev.size() == 1 && ev[0].kind == kCombatDismember && ev[0].entity == 1 && ev[0].partsMask == (1u << 3));
    CHECK(DismemberDiff(reported, now).empty());                    // same state again: never twice

    now[1] = (1u << 3) | (1u << 7);                                 // part 7 added
    ev = DismemberDiff(reported, now);
    CHECK(ev.size() == 1 && ev[0].partsMask == (1u << 7));

    now[1] = 1u << 7;                                               // bit 3 cleared on the host
    CHECK(DismemberDiff(reported, now).empty());                    // cleared bits are never reported
    now[1] = (1u << 3) | (1u << 7);                                 // and coming back is not a new cut
    CHECK(DismemberDiff(reported, now).empty());
    CHECK(reported[1] == ((1u << 3) | (1u << 7)));

    SeveredMasks two;
    two[5] = 1u << 0; two[2] = 1u << 14;                            // several entities in one pass
    ev = DismemberDiff(reported, two);
    CHECK(ev.size() == 2 && ev[0].entity == 2 && ev[0].partsMask == (1u << 14));
    CHECK(ev[1].entity == 5 && ev[1].partsMask == 1u);
}

static void TestDismemberDiffIgnoresInvalid() {
    SeveredMasks reported;
    SeveredMasks now;
    now[0] = 0x7FFF;                                                // entity 0 is not a Willie
    now[4] = 1u << 15;                                              // bit beyond the part enum
    CHECK(DismemberDiff(reported, now).empty());
    now[4] = (1u << 15) | (1u << 1);
    auto ev = DismemberDiff(reported, now);
    CHECK(ev.size() == 1 && ev[0].entity == 4 && ev[0].partsMask == (1u << 1));
    CHECK(reported.count(0) == 0);
    // every emitted event must decode, so the host never sends something the client refuses
    for (const CombatEvent& e : ev) {
        std::vector<uint8_t> wire = Encode(e);
        CombatEvent back;
        CHECK(Decode(wire.data(), wire.size(), back));
    }
}

int main() {
    TestRoundTrip(); TestRejectsBadFields(); TestMalformedWire(); TestDismemberDiff(); TestDismemberDiffIgnoresInvalid();
    std::printf(g_fail ? "%d FAILED\n" : "all combat tests passed\n", g_fail);
    return g_fail ? 1 : 0;
}
