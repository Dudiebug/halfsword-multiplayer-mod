// Protocol rev 19 wire additions: Spawn.life, victimLife (HitClaim, SeverClaim, DeathReport), Welcome.flags, PeerMap, MeshHeard.
#include "hsm_owner.h"
#include "combat/hsm_sever.h"
#include <cstdio>

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++g_fail; } } while (0)
using namespace hsm;

static void TestSpawnLife() {
    Spawn s; s.entity = 3; s.life = 7; Spawn o;
    auto b = Encode(s);
    CHECK(Decode(b.data(), b.size(), o) && o.life == 7 && o.entity == 3);
    for (size_t cut = 0; cut < b.size(); ++cut) CHECK(!Decode(b.data(), cut, o));
    s.life = 255; b = Encode(s); CHECK(Decode(b.data(), b.size(), o) && o.life == 255);
}

static void TestDeathReport() {
    DeathReport r; r.victim = 2; r.killer = 1; r.victimLife = 9; DeathReport o;
    auto b = Encode(r);
    CHECK(Decode(b.data(), b.size(), o) && o.victimLife == 9 && o.victim == 2 && o.killer == 1);
    for (size_t cut = 0; cut < b.size(); ++cut) CHECK(!Decode(b.data(), cut, o));
}

static void TestClaims() {
    combat::HitClaim c; c.claimId = 5; c.attacker = 1; c.victim = 2; c.hitBone = "head"; c.victimLife = 4;
    HitClaimMsg o;
    auto b = Encode(HitClaimMsg{c});
    CHECK(Decode(b.data(), b.size(), o) && o.claim.victimLife == 4);
    auto fx = Encode(HitFxMsg{c}); HitFxMsg ofx;
    CHECK(Decode(fx.data(), fx.size(), ofx) && ofx.claim.victimLife == 4);
    for (size_t cut = 0; cut < b.size(); ++cut) CHECK(!Decode(b.data(), cut, o));

    combat::SeverClaim sc; sc.claimId = 6; sc.attacker = 1; sc.victim = 2; sc.victimLife = 11; sc.part = 3;
    sc.attachMarker = "DM Sphere Thigh_R 2"; sc.markers = {"DM Sphere Thigh_R 2"}; sc.frameBone = "thigh_r";
    sc.box1.local.rot = sync::Q4{0, 0, 0, 1}; sc.box1.extent = sync::V3{100, 500, 100}; sc.box2 = sc.box1;
    combat::SeverClaimMsg so;
    auto sb = Encode(combat::SeverClaimMsg{sc});
    CHECK(Decode(sb.data(), sb.size(), so) && so.claim.victimLife == 11 && so.claim.victim == 2);
    for (size_t cut = 0; cut < sb.size(); ++cut) CHECK(!Decode(sb.data(), cut, so));
    sc.attacker = sc.victim; combat::SeverNoticeMsg no;
    auto nb = Encode(combat::SeverNoticeMsg{sc});
    CHECK(Decode(nb.data(), nb.size(), no) && no.claim.victimLife == 11);
}

static void TestWelcomeFlags() {
    Welcome w; w.map = "m"; Welcome o;
    CHECK(w.flags == 0);
    auto b = Encode(w); CHECK(Decode(b.data(), b.size(), o) && o.flags == 0);
    w.flags = kWelcomeNoHostFighter | kWelcomeMesh; b = Encode(w);
    CHECK(Decode(b.data(), b.size(), o) && o.flags == 3 && o.map == "m");
    w.flags = 4; b = Encode(w); CHECK(!Decode(b.data(), b.size(), o));      // an unknown bit
    w.flags = 0x80; b = Encode(w); CHECK(!Decode(b.data(), b.size(), o));
    w.flags = 1; b = Encode(w);
    for (size_t cut = 0; cut < b.size(); ++cut) CHECK(!Decode(b.data(), cut, o));
}

static void TestPeerMap() {
    PeerMapMsg m; PeerMapMsg o;
    auto b = Encode(m); CHECK(Decode(b.data(), b.size(), o) && o.peers.empty());   // a lone host in the lobby
    for (uint8_t s = 0; s < 6; ++s) m.peers.push_back({s, 76561198000000000ull + s});
    b = Encode(m); CHECK(Decode(b.data(), b.size(), o) && o.peers.size() == 6 && o.peers[5].slot == 5 && o.peers[5].steamId == 76561198000000005ull);
    for (size_t cut = 0; cut < b.size(); ++cut) CHECK(!Decode(b.data(), cut, o));
    auto extra = b; extra.push_back(0); CHECK(!Decode(extra.data(), extra.size(), o));
    auto seven = b; seven[4] = 7; CHECK(!Decode(seven.data(), seven.size(), o));    // count byte follows the 4-byte header
    PeerMapMsg bad; bad.peers = {{6, 5}}; b = Encode(bad); CHECK(!Decode(b.data(), b.size(), o));      // slot out of range
    bad.peers = {{1, 0}}; b = Encode(bad); CHECK(!Decode(b.data(), b.size(), o));                      // no SteamID
    bad.peers = {{1, 5}, {1, 6}}; b = Encode(bad); CHECK(!Decode(b.data(), b.size(), o));              // duplicate slot
    PeerMapMsg many; for (int i = 0; i < 9; ++i) many.peers.push_back({uint8_t(i % 6), uint64_t(i + 1)});
    b = Encode(many); CHECK(Decode(b.data(), b.size(), o) && o.peers.size() == 6);                     // the encoder bounds it
}

static void TestMeshHeard() {
    MeshHeardMsg m; MeshHeardMsg o;
    auto b = Encode(m); CHECK(Decode(b.data(), b.size(), o) && o.mask == 0);
    m.mask = 0x3F; b = Encode(m); CHECK(Decode(b.data(), b.size(), o) && o.mask == 0x3F);
    m.mask = 0x40; b = Encode(m); CHECK(!Decode(b.data(), b.size(), o));
    m.mask = 0x3F; b = Encode(m);
    for (size_t cut = 0; cut < b.size(); ++cut) CHECK(!Decode(b.data(), cut, o));
    auto extra = b; extra.push_back(0); CHECK(!Decode(extra.data(), extra.size(), o));
    MsgType t; CHECK(PeekType(b.data(), b.size(), t) && t == MsgType::MeshHeard);
}

int main() {
    TestSpawnLife(); TestDeathReport(); TestClaims(); TestWelcomeFlags(); TestPeerMap(); TestMeshHeard();
    if (g_fail) { std::printf("%d failures\n", g_fail); return 1; }
    std::puts("ok");
    return 0;
}
