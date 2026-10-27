// Deterministic fuzz harness for every hsm_protocol decoder (run under ASan/UBSan by run_fuzz.sh).
// Properties: no decoder reads out of bounds or invokes UB on arbitrary bytes; whatever decodes successfully
// re-encodes to bytes that decode to an identical encoding (canonical round trip).
// -DHSM_FUZZ_NEGATIVE_CONTROL adds a deliberately broken decoder so the gate can prove it detects bugs.
#include "combat/hsm_equip.h"
#include "combat/hsm_fx.h"
#include "combat/hsm_sever.h"
#include "hsm_owner.h"
#include "hsm_protocol.h"
#include "session/hsm_lease.h"
#include "session/hsm_prop_census.h"
#include "session/hsm_report.h"
#include "sync/hsm_grab.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>

using namespace hsm;

namespace {
uint64_t g_state = 0x9E3779B97F4A7C15ull;
uint64_t Rand() { g_state ^= g_state << 13; g_state ^= g_state >> 7; g_state ^= g_state << 17; return g_state; }
size_t Below(size_t n) { return n ? size_t(Rand() % n) : 0; }

int g_roundTripFailures = 0;

template <class M>
void Try(const uint8_t* d, size_t n) {
    M m;
    if (!Decode(d, n, m)) return;
    std::vector<uint8_t> again = Encode(m);
    M m2;
    if (!Decode(again.data(), again.size(), m2) || Encode(m2) != again) {
        if (++g_roundTripFailures <= 5) std::printf("round-trip mismatch for message type %u (size %zu)\n", unsigned(d[3]), n);
    }
}

void TryAll(const uint8_t* d, size_t n) {
    Try<Hello>(d, n); Try<Welcome>(d, n); Try<Reject>(d, n); Try<InputFrame>(d, n); Try<Snapshot>(d, n);
    Try<Event>(d, n); Try<Travel>(d, n); Try<Ready>(d, n); Try<Spawn>(d, n); Try<Despawn>(d, n); Try<BlobChunk>(d, n);
    Try<MatchInfo>(d, n); Try<Kill>(d, n);
    Try<PoseMsg>(d, n); Try<VitalsMsg>(d, n); Try<HitClaimMsg>(d, n); Try<HitFxMsg>(d, n);
    Try<combat::SeverClaimMsg>(d, n); Try<combat::SeverNoticeMsg>(d, n); Try<combat::StuckBladeMsg>(d, n);
    Try<GrabStateMsg>(d, n); Try<PropStateMsg>(d, n); Try<PropLeaseMsg>(d, n);
    Try<combat::PaintFxMsg>(d, n); Try<combat::EquipStateMsg>(d, n); Try<PropCensusMsg>(d, n); Try<GiveUpMsg>(d, n); Try<ReportShareMsg>(d, n); Try<PeerMapMsg>(d, n); Try<MeshHeardMsg>(d, n); Try<DeathReport>(d, n);
    Ping p; bool pong; DecodePing(d, n, p, pong);
    MsgType t; PeekType(d, n, t);
#ifdef HSM_FUZZ_NEGATIVE_CONTROL
    // Known-bad decoder: trusts a length byte from the packet and reads past the end of the buffer.
    if (n >= 5) { volatile uint8_t sink = 0; size_t len = d[4]; for (size_t i = 0; i < len + 8; ++i) sink ^= d[5 + i]; (void)sink; }
#endif
}

std::vector<std::vector<uint8_t>> Seeds() {
    std::vector<std::vector<uint8_t>> s;
    Hello h; h.buildKey = 0x8EFA122909712000ull; h.name = "seed"; h.modBuild = "0.6.4 (v0.6.3-12-gdb82570)"; s.push_back(Encode(h));
    Welcome w; w.map = "/Game/Maps/Arenas/Map_Arena_Yard"; w.modBuild = "0.6.4"; w.flags = kWelcomeMesh; s.push_back(Encode(w));
    Reject r; r.reason = RejectReason::Full; s.push_back(Encode(r));
    InputFrame f; f.seq = 7; f.axes[0] = 1; f.buttons = 5; s.push_back(Encode(f));
    Snapshot sn; WillieState ws; ws.entity = 1; ws.bodies.resize(23); sn.willies.push_back(ws); s.push_back(Encode(sn));
    Event e; e.kind = EventKind::Died; e.a = 2; s.push_back(Encode(e));
    Travel t; t.level = "/Game/Maps/Map_Hub_Tavern_Frank"; t.settings.push_back({"Free Mode Activated", {1}}); s.push_back(Encode(t));
    Ready rd; rd.level = "x"; s.push_back(Encode(rd));
    s.push_back(EncodePing(5, false));
    Spawn sp; sp.entity = 2; sp.blobId = 9; sp.x = 10; sp.life = 3; s.push_back(Encode(sp));
    PeerMapMsg pmm; pmm.peers = {{1, 76561198000000001ull}, {2, 76561198000000002ull}}; s.push_back(Encode(pmm));
    MeshHeardMsg mh; mh.mask = 0x06; s.push_back(Encode(mh));
    DeathReport dr; dr.victim = 2; dr.killer = 1; dr.victimLife = 3; s.push_back(Encode(dr));
    Despawn dp; dp.entity = 2; s.push_back(Encode(dp));
    BlobChunk bc; bc.blobId = 9; bc.total = 300; bc.count = 1; bc.crc = 1; bc.data.assign(300, 7); s.push_back(Encode(bc));
    MatchInfo mi; mi.format = 1; mi.scoreLimit = 10; mi.phase = 2; mi.scores = {{0, 3}, {1, 4}}; mi.fighters = {{1, 0, 2, 1, "a"}, {2, 1, 0, 3, "bb"}}; s.push_back(Encode(mi));
    MatchInfo md; md.format = 4; md.scoreLimit = 3; md.phase = 2; md.gear = 2; md.tier = 4; md.round = 2; md.roundGapLeftMs = 3000; md.roundWinner = 1;
    md.scores = {{1, 1}}; md.fighters = {{1, -1, 1, 0, "a", 40, true}, {2, -1, 0, 1, "bb", 120, false}}; s.push_back(Encode(md));
    Kill kl; kl.killer = 1; kl.victim = 2; s.push_back(Encode(kl));
    sync::PoseFrame pf; pf.tsMs = 5000; pf.stepUs = 16667;
    for (int b = 0; b < sync::kPoseBones; ++b) { pf.bones[size_t(b)].pos = sync::V3{ 10.0 * b, 5, 90 }; pf.bones[size_t(b)].rot = sync::Q4{ 0, 0, 0, 1 }; }
    pf.weapons[0].present = true; pf.weapons[0].classHash = 7; pf.weapons[0].rot = sync::Q4{ 0, 0, std::sin(0.2), std::cos(0.2) };
    pf.weapons[0].hasHead = true; pf.weapons[0].head.pos = sync::V3{ 60, 5, 40 }; pf.weapons[0].head.vel = sync::V3{ 300, 0, -200 };   // a flail
    PoseMsg pm; pm.entity = 2; pm.rootX = 1; pm.rootZ = 90; pm.yawDeg = 30; pm.pose = sync::EncodePose(pf); s.push_back(Encode(pm));
    VitalsMsg vm; vm.entity = 2; vm.flags = 2; vm.health = 50; vm.partHealth[3] = 9; vm.severed = 4; s.push_back(Encode(vm));
    combat::HitClaim hc; hc.claimId = 3; hc.victimLife = 2; hc.attacker = 1; hc.victim = 2; hc.hitBone = "head"; hc.strikeComponent = "Blade";
    hc.source = combat::StrikeSource::WeaponRight; hc.hasHitBox = true; hc.hitBoxExtent = sync::V3{ 5, 5, 5 };
    s.push_back(Encode(HitClaimMsg{ hc })); s.push_back(Encode(HitFxMsg{ hc }));
    combat::HitClaim rock = hc;
    combat::StruckByProp(rock, MakeLevelPropId(14), sync::V3{ 300, 0, -50 }, combat::Xform{ {}, sync::Q4{ 0, 0, 0, 1 }, { 1, 1, 1 } });
    s.push_back(Encode(HitClaimMsg{ rock })); s.push_back(Encode(HitFxMsg{ rock }));
    combat::SeverClaim sc; sc.claimId = 4; sc.victimLife = 2; sc.attacker = 1; sc.victim = 2; sc.part = 9; sc.masterMesh = "CharacterMesh0";
    sc.attachMarker = "DM Sphere Thigh_R 2"; sc.markers = {"DM Sphere Thigh_R 2", "DM Sphere Thigh_R 3"}; sc.frameBone = "thigh_r";
    sc.box1.local.rot = sync::Q4{ 0, 0, 0, 1 }; sc.box1.extent = sync::V3{ 100, 500, 100 }; sc.box1.local.pos = sync::V3{ 0, 500, 0 };
    sc.box2 = sc.box1; sc.box2.local.pos = sync::V3{ 0, -500, 0 }; sc.source = combat::StrikeSource::WeaponRight; sc.weaponComponent = "Blade";
    s.push_back(Encode(combat::SeverClaimMsg{ sc }));
    combat::SeverClaim own = sc; own.attacker = own.victim; s.push_back(Encode(combat::SeverNoticeMsg{ own }));
    combat::StuckBladeMsg sb; sb.attacker = 1; sb.victim = 2; sb.stuck = true; sb.seq = 3; sb.bone = "spine_03"; sb.weaponComponent = "Blade";
    sb.weaponInBone.rot = sync::Q4{ 0, 0, 0, 1 }; s.push_back(Encode(sb));
    sb.stuck = false; sb.bone = ""; s.push_back(Encode(sb));
    GrabStateMsg gm; gm.grabber = 1; gm.seq = 4; gm.hands[0].held = true; gm.hands[0].target = 2; gm.hands[0].grabId = 9;
    gm.hands[0].bone = "lowerarm_l"; gm.hands[0].grip = sync::V3{ 1.5, -2.25, 10 }; s.push_back(Encode(gm));
    PropStateMsg ps; ps.propId = MakeDropPropId(3, 1, 5); ps.lessee = 2; ps.senderTimeMs = 77; ps.px = 120; ps.pz = 40;
    ps.rot = PackQuat(0, 0, 0.3f, 0.95f); ps.vx = 300; ps.wz = 720; s.push_back(Encode(ps));
    PropLeaseMsg pl; pl.propId = ps.propId; pl.action = LeaseAction::Grant; pl.level = LeaseLevel::Touch; pl.slot = 2; s.push_back(Encode(pl));
    PropLeaseMsg lv; lv.propId = MakeLevelPropId(3); lv.action = LeaseAction::Request; lv.level = LeaseLevel::Hold; lv.slot = 1; s.push_back(Encode(lv));
    PropStateMsg pe = ps; pe.propId = MakeDropPropId(16, 0, 127, 0xFFFFFFFFu); s.push_back(Encode(pe));   // the top drop epoch
    PropLeaseMsg le = pl; le.propId = pe.propId; le.action = LeaseAction::Request; le.level = LeaseLevel::Hold; s.push_back(Encode(le));
    combat::PaintFxMsg bl; bl.entity = 3; bl.kind = combat::FxKind::Blood; bl.seq = 7; bl.bone = "lowerarm_r"; bl.bodyPart = 2;
    bl.local.pos = sync::V3{ 4, -2, 7.5 }; bl.local.rot = sync::Q4{ 0, 0, std::sin(0.3), std::cos(0.3) }; bl.rate = 12.5; bl.damageRate = 1;
    bl.loopCount = 4; bl.extraVelocity = true; bl.velocity = sync::V3{ -30, 12, 400 }; bl.bleeding = 37; s.push_back(Encode(bl));
    combat::PaintFxMsg wd; wd.entity = 16; wd.kind = combat::FxKind::Wound; wd.bone = "head"; wd.extent = sync::V3{ 3, 3, 3 };
    wd.damageRate = 1.5; s.push_back(Encode(wd));
    combat::EquipStateMsg eq; eq.entity = 2; eq.seq = 9; eq.hand = {1, 4}; eq.sheathed = 0x04; s.push_back(Encode(eq));
    PropCensusMsg pc; pc.seq = 3; pc.part = 1; pc.parts = 2;
    pc.entries = {CensusEntry{0, 0x1234u, 10, -20, 30}, CensusEntry{9, 0xBEEFu, -500, 0.5f, 120}}; s.push_back(Encode(pc));
    GiveUpMsg gu; gu.entity = 2; s.push_back(Encode(gu));
    ReportShareMsg rs; rs.fromSlot = 1; rs.id = "r7Kq2"; rs.token = "Ab3_x-9QwertyUiop12"; s.push_back(Encode(rs));
    return s;
}
}  // namespace

int main(int argc, char** argv) {
    long iterations = argc > 1 ? std::atol(argv[1]) : 300000;
    auto seeds = Seeds();
    std::vector<uint8_t> buf;
    for (long i = 0; i < iterations; ++i) {
        if (Rand() % 4 == 0) {                       // pure random bytes, sometimes with a valid header
            buf.resize(Below(600));
            for (uint8_t& b : buf) b = uint8_t(Rand());
            if (buf.size() >= 4 && Rand() % 2) { buf[0] = 0x48; buf[1] = 0x4D; buf[2] = kProtocolRevision; buf[3] = uint8_t(1 + Below(uint8_t(MsgType::MeshHeard))); }
        } else {                                     // mutate a valid message
            buf = seeds[Below(seeds.size())];
            int edits = 1 + int(Below(4));
            for (int k = 0; k < edits; ++k) {
                switch (Below(4)) {
                    case 0: if (!buf.empty()) buf[Below(buf.size())] ^= uint8_t(1u << Below(8)); break;
                    case 1: if (!buf.empty()) buf.resize(Below(buf.size())); break;
                    case 2: buf.insert(buf.begin() + long(Below(buf.size() + 1)), uint8_t(Rand())); break;
                    case 3: if (!buf.empty()) buf[Below(buf.size())] = uint8_t(Rand()); break;
                }
            }
        }
        // Copy into an exact-size heap block so ASan sees any read past the end.
        uint8_t* exact = static_cast<uint8_t*>(std::malloc(buf.size() ? buf.size() : 1));
        if (!buf.empty()) std::memcpy(exact, buf.data(), buf.size());
        TryAll(exact, buf.size());
        std::free(exact);
    }
    std::printf("fuzz: %ld inputs, %d round-trip failures\n", iterations, g_roundTripFailures);
    return g_roundTripFailures ? 1 : 0;
}
