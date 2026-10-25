// Unit tests for hsm_protocol.h. Build: g++ -std=c++20 -Wall -Wextra -O1 test_hsm_protocol.cpp
#include "hsm_protocol.h"
#include "match/hsm_match.h"
#include <cstdio>
#include <limits>

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++g_fail; } } while (0)

using namespace hsm;

static void TestHello() {
    Hello h; h.buildKey = 0x1122334455667788ull; h.name = "player"; h.modBuild = "0.6.4 (v0.6.3-12-gdb82570)";
    auto b = Encode(h); Hello o;
    CHECK(Decode(b.data(), b.size(), o));
    CHECK(o.buildKey == h.buildKey && o.name == "player" && o.revision == kProtocolRevision && o.modBuild == h.modBuild);
    for (size_t cut = 0; cut < b.size(); ++cut) CHECK(!Decode(b.data(), cut, o));      // truncation
    auto extra = b; extra.push_back(0); CHECK(!Decode(extra.data(), extra.size(), o));  // trailing bytes
    auto bad = b; bad[2] = kProtocolRevision + 1; CHECK(!Decode(bad.data(), bad.size(), o));
    Hello longName; longName.name = std::string(100, 'x');
    auto lb = Encode(longName); CHECK(Decode(lb.data(), lb.size(), o) && o.name.size() == kMaxPlayerName);
    Hello longBuild; longBuild.modBuild = std::string(200, 'b');   // rev 17: the mod build, cut to kMaxModBuild
    auto bb = Encode(longBuild); CHECK(Decode(bb.data(), bb.size(), o) && o.modBuild == std::string(kMaxModBuild, 'b') && o.name.empty());
    Hello noBuild; noBuild.name = "x";
    auto nb = Encode(noBuild); CHECK(Decode(nb.data(), nb.size(), o) && o.modBuild.empty() && o.name == "x");
    CHECK(nb.size() == Encode(Hello{}).size() + noBuild.name.size());   // an empty build costs its length byte only
}

static void TestInput() {
    InputFrame f; f.seq = 42; f.clientTimeMs = 1000; f.axes[size_t(Axis::MoveForward)] = 1.f; f.axes[size_t(Axis::TurnMouse)] = -3.5f;
    f.buttons = 1u << size_t(Action::GrabLeft) | 1u << size_t(Action::Thrust); f.controlYaw = 90; f.controlPitch = -10;
    auto b = Encode(f); InputFrame o;
    CHECK(Decode(b.data(), b.size(), o));
    CHECK(o.seq == 42 && o.axes[size_t(Axis::TurnMouse)] == -3.5f && o.buttons == f.buttons && o.controlYaw == 90);
    InputFrame big = f; big.axes[0] = kAxisLimit * 2; auto bb = Encode(big); CHECK(!Decode(bb.data(), bb.size(), o));
    InputFrame nan = f; nan.axes[1] = std::numeric_limits<float>::quiet_NaN(); auto nb = Encode(nan); CHECK(!Decode(nb.data(), nb.size(), o));
    InputFrame inf = f; inf.controlYaw = std::numeric_limits<float>::infinity(); auto ib = Encode(inf); CHECK(!Decode(ib.data(), ib.size(), o));
    InputFrame btn = f; btn.buttons = 1u << kActionCount; auto bt = Encode(btn); CHECK(!Decode(bt.data(), bt.size(), o));
    for (size_t cut = 0; cut < b.size(); ++cut) CHECK(!Decode(b.data(), cut, o));
}

static void TestSnapshot() {
    Snapshot s; s.tick = 7; s.ackInputSeq = 41;
    for (int i = 0; i < 2; ++i) {
        WillieState w; w.entity = uint8_t(i + 1); w.flags = 2; w.health = 80; w.partHealth[3] = 12.5f; w.dismembered = 1u << 4;
        for (int k = 0; k < 20; ++k) { BodyState bs; bs.px = float(k); bs.rot = PackQuat(0, 0, 0.7071f, 0.7071f); w.bodies.push_back(bs); }
        s.willies.push_back(w);
    }
    auto b = Encode(s); Snapshot o;
    CHECK(Decode(b.data(), b.size(), o));
    CHECK(o.willies.size() == 2 && o.willies[1].entity == 2 && o.willies[1].bodies.size() == 20 && o.willies[0].bodies[19].px == 19.f);
    CHECK(o.willies[0].partHealth[3] == 12.5f && o.willies[0].dismembered == 16u);
    for (size_t cut = 0; cut < b.size(); cut += 7) CHECK(!Decode(b.data(), cut, o));
    auto bad = b; bad[12] = uint8_t(kMaxWillies + 1); CHECK(!Decode(bad.data(), bad.size(), o));   // willie count byte
}

static void TestEventAndReject() {
    Event e; e.seq = 3; e.kind = EventKind::Dismembered; e.a = 1; e.b = 5; e.text = "x";
    auto b = Encode(e); Event o; CHECK(Decode(b.data(), b.size(), o) && o.kind == EventKind::Dismembered && o.b == 5);
    auto bad = b; bad[8] = 99; CHECK(!Decode(bad.data(), bad.size(), o));
    Reject r; r.reason = RejectReason::BuildMismatch; auto rb = Encode(r); Reject ro;
    CHECK(Decode(rb.data(), rb.size(), ro) && ro.reason == RejectReason::BuildMismatch);
    Welcome w; w.snapshotHz = 0; auto wb = Encode(w); Welcome wo; CHECK(!Decode(wb.data(), wb.size(), wo));
    w.snapshotHz = 30; w.map = "/Game/Maps/Arenas/Map_Arena_Yard"; wb = Encode(w); CHECK(Decode(wb.data(), wb.size(), wo) && wo.map == w.map);
    MsgType t; CHECK(PeekType(rb.data(), rb.size(), t) && t == MsgType::Reject);
    uint8_t junk[3] = {1, 2, 3}; CHECK(!PeekType(junk, 3, t));
}

static void TestQuatAndSeq() {
    Quat16 q = PackQuat(0, 0, -0.7071f, -0.7071f);   // negative w is canonicalized
    float x, y, z, w; UnpackQuat(q, x, y, z, w);
    CHECK(w > 0 && std::fabs(z - 0.7071f) < 1e-3f);
    Quat16 zero = PackQuat(0, 0, 0, 0); UnpackQuat(zero, x, y, z, w); CHECK(w == 1.f);
    Quat16 nanq = PackQuat(std::numeric_limits<float>::quiet_NaN(), 0, 0, 1); UnpackQuat(nanq, x, y, z, w); CHECK(w == 1.f);
    CHECK(SeqNewer(5, 4) && !SeqNewer(4, 5) && SeqNewer(1, 0xFFFFFFF0u) && !SeqNewer(7, 7));
}

static void TestTravel() {
    Travel t; t.seq = 4; t.level = "/Game/Maps/Arenas/Map_Arena_Yard.Map_Arena_Yard"; t.absolute = true; t.options = "";
    t.settings.push_back({"Free Mode Activated", {1}});
    t.settings.push_back({"Free Mode Foes Amount", {3, 0, 0, 0}});
    auto b = Encode(t); Travel o;
    CHECK(Decode(b.data(), b.size(), o));
    CHECK(o.seq == 4 && o.level == t.level && o.absolute && o.settings.size() == 2 && o.settings[1].value[0] == 3 && o.settings[0].name == "Free Mode Activated");
    for (size_t cut = 0; cut < b.size(); ++cut) CHECK(!Decode(b.data(), cut, o));
    Travel empty = t; empty.level.clear(); auto eb = Encode(empty); CHECK(!Decode(eb.data(), eb.size(), o));      // level required
    Travel noname = t; noname.settings[0].name.clear(); auto nb = Encode(noname); CHECK(!Decode(nb.data(), nb.size(), o));
    Travel big = t; big.settings[0].value.assign(40, 7); auto bb = Encode(big);                                         // value truncated to limit on encode
    CHECK(Decode(bb.data(), bb.size(), o) && o.settings[0].value.size() == kMaxSettingBytes);
    auto bad = b; bad[4 + 4 + 1 + t.level.size()] = 2; CHECK(!Decode(bad.data(), bad.size(), o));                      // absolute flag must be 0/1
    Ready r; r.travelSeq = 4; r.level = t.level; auto rb = Encode(r); Ready ro;
    CHECK(Decode(rb.data(), rb.size(), ro) && ro.travelSeq == 4 && ro.level == t.level);
    MsgType mt; CHECK(PeekType(b.data(), b.size(), mt) && mt == MsgType::Travel);
}

static void TestSpawnDespawn() {
    Spawn s; s.entity = 3; s.kind = EntityKind::Fighter; s.ownerSlot = 2; s.team = 1; s.blobId = 77; s.x = 120.5f; s.y = -40; s.z = 90; s.yaw = 180;
    auto b = Encode(s); Spawn o;
    CHECK(Decode(b.data(), b.size(), o) && o.entity == 3 && o.kind == EntityKind::Fighter && o.ownerSlot == 2 && o.team == 1 && o.blobId == 77 && o.x == 120.5f && o.yaw == 180);
    for (size_t cut = 0; cut < b.size(); ++cut) CHECK(!Decode(b.data(), cut, o));
    Spawn zero = s; zero.entity = 0; auto zb = Encode(zero); CHECK(!Decode(zb.data(), zb.size(), o));
    auto badKind = b; badKind[5] = 9; CHECK(!Decode(badKind.data(), badKind.size(), o));
    Spawn team = s; team.team = 8; auto tb = Encode(team); CHECK(!Decode(tb.data(), tb.size(), o));
    Spawn far = s; far.x = 1e9f; auto fb = Encode(far); CHECK(!Decode(fb.data(), fb.size(), o));
    Spawn ffa = s; ffa.team = -1; auto ab = Encode(ffa); CHECK(Decode(ab.data(), ab.size(), o) && o.team == -1);
    Despawn d; d.entity = 4; auto db = Encode(d); Despawn od; CHECK(Decode(db.data(), db.size(), od) && od.entity == 4);
    Despawn d0; auto d0b = Encode(d0); CHECK(!Decode(d0b.data(), d0b.size(), od));
}

static void TestMatchInfoAndKill() {
    MatchInfo m; m.format = 2; m.scoreLimit = 12; m.timeLimitS = 300; m.respawnDelayS = 4; m.friendlyFire = true;
    m.phase = 2; m.timeLeftMs = 123456; m.winner = -1000;
    m.scores = {{0, 3}, {1, -2}, {2, 7}};
    m.fighters = {{1, 0, 3, 1, "host"}, {2, 1, 0, 2, "ann"}, {9, 2, 7, 0, "Bot"}};
    CHECK(m.gear == 0 && m.tier == 2 && m.round == 0 && m.roundGapLeftMs == 0 && m.roundWinner == 0);
    CHECK(m.fighters[0].pingMs == 0 && m.fighters[0].alive);                   // defaults: no ping known, alive
    m.gear = 2; m.tier = 5; m.round = 3; m.roundGapLeftMs = 4200; m.roundWinner = 9;   // rev 13: the rounds plan's fields
    m.fighters[0].pingMs = 87; m.fighters[1].alive = false; m.fighters[2].pingMs = 65535;
    CHECK(!m.fighters[0].down);
    m.fighters[2].down = true;                                                  // rev 17: the draw check has it down
    auto b = Encode(m); MatchInfo o;
    CHECK(Decode(b.data(), b.size(), o));
    CHECK(o.gear == 2 && o.tier == 5 && o.round == 3 && o.roundGapLeftMs == 4200 && o.roundWinner == 9);
    CHECK(o.fighters[0].pingMs == 87 && o.fighters[0].alive && !o.fighters[1].alive && o.fighters[1].pingMs == 0);
    CHECK(o.fighters[2].alive && o.fighters[2].pingMs == 65535);
    CHECK(o.fighters[2].down && !o.fighters[0].down && !o.fighters[1].down);
    { MatchInfo dd = m; dd.fighters[2].alive = false; auto e = Encode(dd); MatchInfo y;   // each bit on its own
      CHECK(Decode(e.data(), e.size(), y) && !y.fighters[2].alive && y.fighters[2].down); }
    for (uint8_t g = 0; g < kGearCount; ++g)
        for (uint8_t tier = 0; tier <= 7; ++tier) {
            MatchInfo x = m; x.gear = g; x.tier = tier; x.round = uint16_t(1000 + tier); x.roundGapLeftMs = 0xFFFFFFFFu;
            auto e = Encode(x); MatchInfo y;
            CHECK(Decode(e.data(), e.size(), y) && y.gear == g && y.tier == tier && y.round == 1000 + tier && y.roundGapLeftMs == 0xFFFFFFFFu);
        }
    CHECK(o.format == 2 && o.scoreLimit == 12 && o.timeLimitS == 300 && o.respawnDelayS == 4 && o.friendlyFire && o.phase == 2);
    CHECK(o.timeLeftMs == 123456 && o.winner == -1000 && o.scores.size() == 3 && o.scores[1].score == -2 && o.scores[2].key == 2);
    CHECK(o.fighters.size() == 3 && o.fighters[2].entity == 9 && o.fighters[2].team == 2 && o.fighters[2].kills == 7 && o.fighters[1].deaths == 2 && o.fighters[1].name == "ann");
    auto bad = [&](auto mutate) { MatchInfo x = m; mutate(x); auto e = Encode(x); MatchInfo y; return !Decode(e.data(), e.size(), y); };
    // Every match format the host can run must reach the client (Duel and Zombies were dropped, test 2).
    for (uint8_t f = 0; f < kFormatCount; ++f) {
        MatchInfo x = m; x.format = f; auto e = Encode(x); MatchInfo y;
        CHECK(Decode(e.data(), e.size(), y) && y.format == f);
    }
    CHECK(bad([](MatchInfo& x) { x.format = kFormatCount; }));
    CHECK(bad([](MatchInfo& x) { x.format = 255; }));
    CHECK(bad([](MatchInfo& x) { x.phase = 5; }));
    CHECK(bad([](MatchInfo& x) { x.scoreLimit = 0; }));
    CHECK(bad([](MatchInfo& x) { x.scoreLimit = 101; }));
    CHECK(bad([](MatchInfo& x) { x.timeLimitS = 3601; }));
    CHECK(bad([](MatchInfo& x) { x.respawnDelayS = 61; }));
    CHECK(bad([](MatchInfo& x) { x.fighters[0].entity = 0; }));
    CHECK(bad([](MatchInfo& x) { x.fighters[0].team = 8; }));
    CHECK(bad([](MatchInfo& x) { x.fighters.resize(kMaxWillies + 1, {1, 0, 0, 0, ""}); }));
    CHECK(bad([](MatchInfo& x) { x.scores.resize(kMaxWillies + 1); }));
    CHECK(bad([](MatchInfo& x) { x.gear = kGearCount; }));
    CHECK(bad([](MatchInfo& x) { x.tier = 8; }));
    { auto e = Encode(m); CHECK(e.back() == 3); e.back() = 4; MatchInfo y; CHECK(!Decode(e.data(), e.size(), y)); }   // alive: bits 0-1 only
    { auto e = Encode(m); e.pop_back(); MatchInfo y; CHECK(!Decode(e.data(), e.size(), y)); }                        // truncated
    b.push_back(0); CHECK(!Decode(b.data(), b.size(), o));                  // trailing byte
    Kill k; k.killer = 3; k.victim = 4; k.flags = 1;
    auto kb = Encode(k); Kill ko; CHECK(Decode(kb.data(), kb.size(), ko) && ko.killer == 3 && ko.victim == 4 && ko.flags == 1);
    k.victim = 0; kb = Encode(k); CHECK(!Decode(kb.data(), kb.size(), ko));   // victim required
    // Spec revision (rev 17): flag 8 is "gave up" (Duel, held G); 16 and 32 a drawn round; 64 is the first unknown bit.
    k.victim = 4; k.flags = 8; kb = Encode(k); CHECK(Decode(kb.data(), kb.size(), ko) && ko.flags == 8);
    k.flags = 16; kb = Encode(k); CHECK(Decode(kb.data(), kb.size(), ko) && ko.flags == 16);
    k.flags = 63; kb = Encode(k); CHECK(Decode(kb.data(), kb.size(), ko) && ko.flags == 63);
    k.flags = 64; kb = Encode(k); CHECK(!Decode(kb.data(), kb.size(), ko));
    CHECK(kMaxWillies >= 16 && kProtocolRevision == 17);   // rev 17: drawn rounds, flail head, mod build in Hello and Welcome, fighters down in MatchInfo; rev 16: GiveUp; rev 15: PaintFx Voice, HitSound, Armor, ArmorOn; rev 14: pose codec v2 (grip state); rev 13: prop HitClaims, MatchInfo rounds/gear/ping/alive; rev 12: SeverNotice, PropCensus
    // rev 9: Welcome says who simulates the fighters (0 host, 1 owner); nothing else is a mode
    Welcome sm; sm.snapshotHz = 60; sm.map = "m"; Welcome smo;
    CHECK(sm.syncMode == 0);                                                   // host authority unless the host says so
    auto smb = Encode(sm); CHECK(Decode(smb.data(), smb.size(), smo) && smo.syncMode == 0);
    sm.syncMode = 1; smb = Encode(sm); CHECK(Decode(smb.data(), smb.size(), smo) && smo.syncMode == 1 && smo.map == "m" && smo.snapshotHz == 60);
    sm.modBuild = "0.6.4"; smb = Encode(sm);   // rev 17: Welcome carries the host's mod build
    CHECK(Decode(smb.data(), smb.size(), smo) && smo.modBuild == "0.6.4" && smo.syncMode == 1);
    for (size_t cut = 0; cut < smb.size(); ++cut) CHECK(!Decode(smb.data(), cut, smo));
    sm.modBuild.clear();
    sm.syncMode = 2; smb = Encode(sm); CHECK(!Decode(smb.data(), smb.size(), smo));
    sm.syncMode = 0xFF; smb = Encode(sm); CHECK(!Decode(smb.data(), smb.size(), smo));
    sm.syncMode = 1; smb = Encode(sm); smb.pop_back(); CHECK(!Decode(smb.data(), smb.size(), smo));   // the mode byte is required
    CHECK(uint8_t(MsgType::Pose) == 22 && uint8_t(MsgType::Vitals) == 23 && uint8_t(MsgType::HitClaim) == 24 && uint8_t(MsgType::HitFx) == 25);
}

static void TestRelayInput() {
    RelayInput r; r.entity = 3; r.frame.seq = 77; r.frame.clientTimeMs = 9; r.frame.axes[0] = 1.f; r.frame.buttons = 5; r.frame.controlYaw = 90.f;
    std::vector<uint8_t> b = Encode(r);
    RelayInput back;
    CHECK(Decode(b.data(), b.size(), back) && back.entity == 3 && back.frame.seq == 77 && back.frame.buttons == 5 && back.frame.controlYaw == 90.f);
    for (size_t n = 0; n < b.size(); ++n) CHECK(!Decode(b.data(), n, back));
    RelayInput z = r; z.entity = 0; std::vector<uint8_t> bz = Encode(z); CHECK(!Decode(bz.data(), bz.size(), back));
    RelayInput big = r; big.entity = uint8_t(kMaxWillies + 1); std::vector<uint8_t> bb = Encode(big); CHECK(!Decode(bb.data(), bb.size(), back));
    RelayInput nan = r; nan.frame.axes[1] = NAN; std::vector<uint8_t> bn = Encode(nan); CHECK(!Decode(bn.data(), bn.size(), back));
    InputFrame f; CHECK(!Decode(b.data(), b.size(), f));                   // not an Input message
}

static void TestOwnerStateAndDeath() {
    OwnerState o; o.state.entity = 2; o.state.health = 80; o.state.flags = 1; o.state.bodies.resize(23);
    o.state.bodies[5].px = 12.5f;
    std::vector<uint8_t> b = Encode(o);
    OwnerState back;
    CHECK(Decode(b.data(), b.size(), back) && back.state.entity == 2 && back.state.bodies.size() == 23 && back.state.bodies[5].px == 12.5f);
    for (size_t n = 0; n < b.size(); ++n) CHECK(!Decode(b.data(), n, back));
    OwnerState z = o; z.state.entity = 0; std::vector<uint8_t> bz = Encode(z); CHECK(!Decode(bz.data(), bz.size(), back));
    OwnerState nan = o; nan.state.health = NAN; std::vector<uint8_t> bn = Encode(nan); CHECK(!Decode(bn.data(), bn.size(), back));
    DeathReport d; d.victim = 2; d.killer = 1;
    std::vector<uint8_t> db = Encode(d);
    DeathReport dback;
    CHECK(Decode(db.data(), db.size(), dback) && dback.victim == 2 && dback.killer == 1);
    for (size_t n = 0; n < db.size(); ++n) CHECK(!Decode(db.data(), n, dback));
    DeathReport env = d; env.killer = 0; std::vector<uint8_t> eb = Encode(env); CHECK(Decode(eb.data(), eb.size(), dback) && dback.killer == 0);
    DeathReport bad = d; bad.victim = 0; std::vector<uint8_t> bb = Encode(bad); CHECK(!Decode(bb.data(), bb.size(), dback));
    DeathReport big = d; big.killer = uint8_t(kMaxWillies + 1); std::vector<uint8_t> gb = Encode(big); CHECK(!Decode(gb.data(), gb.size(), dback));
    GiveUpMsg g; g.entity = 3;
    std::vector<uint8_t> ub = Encode(g);
    GiveUpMsg gback; MsgType gt;
    CHECK(Decode(ub.data(), ub.size(), gback) && gback.entity == 3 && PeekType(ub.data(), ub.size(), gt) && gt == MsgType::GiveUp);
    for (size_t n = 0; n < ub.size(); ++n) CHECK(!Decode(ub.data(), n, gback));
    std::vector<uint8_t> ul = ub; ul.push_back(0); CHECK(!Decode(ul.data(), ul.size(), gback));   // trailing byte
    for (uint8_t e : {uint8_t(0), uint8_t(kMaxWillies + 1)}) { GiveUpMsg x; x.entity = e; std::vector<uint8_t> xb = Encode(x); CHECK(!Decode(xb.data(), xb.size(), gback)); }
    CHECK(!Decode(db.data(), db.size(), gback));                                // another message's bytes
}

int main() {
    TestOwnerStateAndDeath();
    TestRelayInput();
    TestMatchInfoAndKill();
    TestTravel();
    TestSpawnDespawn();
    TestHello(); TestInput(); TestSnapshot(); TestEventAndReject(); TestQuatAndSeq();
    std::printf(g_fail ? "%d FAILED\n" : "all protocol tests passed\n", g_fail);
    return g_fail ? 1 : 0;
}
