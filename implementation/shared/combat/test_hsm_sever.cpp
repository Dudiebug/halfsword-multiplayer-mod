// Unit tests for hsm_sever.h. Run via implementation/shared/run_tests.sh test_hsm_sever.
#include "combat/hsm_sever.h"
#include "hsm_protocol.h"
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++g_fail; } } while (0)

using namespace hsm;
using namespace hsm::combat;

static bool Near(double a, double b, double tol = 1e-6) { return std::fabs(a - b) <= tol; }
static bool NearV3(const V3& a, const V3& b, double tol = 1e-6) {
    return Near(a.x, b.x, tol) && Near(a.y, b.y, tol) && Near(a.z, b.z, tol);
}
static bool EqV3(const V3& a, const V3& b) { return a.x == b.x && a.y == b.y && a.z == b.z; }
static bool EqQ4(const Q4& a, const Q4& b) { return a.x == b.x && a.y == b.y && a.z == b.z && a.w == b.w; }
static bool EqXform(const Xform& a, const Xform& b) { return EqV3(a.pos, b.pos) && EqQ4(a.rot, b.rot) && EqV3(a.scale, b.scale); }
static Q4 AxisAngleDeg(double ax, double ay, double az, double degrees) {
    double half = degrees * M_PI / 180.0 / 2.0;
    double s = std::sin(half), c = std::cos(half);
    return {ax * s, ay * s, az * s, c};
}
// The quaternion rotating v (test-local, independent of the library).
static V3 TRot(const Q4& q, const V3& v) {
    double ww = q.w * q.w, xx = q.x * q.x, yy = q.y * q.y, zz = q.z * q.z;
    double xy = q.x * q.y, xz = q.x * q.z, yz = q.y * q.z, wx = q.w * q.x, wy = q.w * q.y, wz = q.w * q.z;
    return {(ww + xx - yy - zz) * v.x + 2 * (xy - wz) * v.y + 2 * (xz + wy) * v.z,
            2 * (xy + wz) * v.x + (ww - xx + yy - zz) * v.y + 2 * (yz - wx) * v.z,
            2 * (xz - wy) * v.x + 2 * (yz + wx) * v.y + (ww - xx - yy + zz) * v.z};
}

// ---------------------------------------------------------------- the part table
// Willie_BP.cpp:3269 (Dismember Function Delayed): Spawn Bone = switch(Currently Dismembered Part){...}.
static void TestSpawnBoneTable() {
    const char* want[15] = {"neck_01", "neck_01", "pelvis", "upperarm_r", "lowerarm_r", "hand_r", "upperarm_l", "lowerarm_l",
                            "hand_l", "thigh_r", "calf_r", "foot_r", "thigh_l", "calf_l", "foot_l"};
    for (uint8_t p = 0; p < 15; ++p) CHECK(SeverSpawnBone(p) && std::strcmp(SeverSpawnBone(p), want[p]) == 0);
    CHECK(SeverSpawnBone(15) == nullptr);
    CHECK(SeverSpawnBone(255) == nullptr);
    CHECK(kSeverParts == 15);
}

// ---------------------------------------------------------------- capture and the bone frame
static SeverCapture SampleCapture(const Xform& bone) {
    SeverCapture in;
    in.claimId = 7; in.attacker = 1; in.victim = 2; in.attackerTsMs = 123456; in.part = 4;
    in.masterMesh = "CharacterMesh0";
    in.attachMarker = "DM Sphere Lowerarm_R 3";
    in.markers = {"DM Sphere Lowerarm_R 3", "DM Sphere Lowerarm_R 4", "DM Sphere Hand_R 1"};
    in.frameBone = "lowerarm_r";
    in.boneWorld = bone;
    in.box1World.pos = bone.FromLocal({0, 500, 0});
    in.box1World.rot = AxisAngleDeg(0, 0, 1, 30.0);
    in.box2World.pos = bone.FromLocal({0, -500, 0});
    in.box2World.rot = AxisAngleDeg(1, 0, 0, -45.0);
    in.box1Extent = {100, 500, 100};
    in.box2Extent = {100, 500, 100};
    in.source = StrikeSource::WeaponRight;
    in.weaponComponent = "Blade";
    return in;
}

static void TestCaptureAndReplayFrame() {
    Xform bone;
    bone.pos = {120, -40, 95};
    bone.rot = AxisAngleDeg(0, 1, 0, 35.0);
    bone.scale = {0.9, 0.9, 0.9};
    SeverCapture in = SampleCapture(bone);
    SeverClaim c;
    CHECK(MakeSeverClaim(in, c));
    CHECK(c.claimId == 7 && c.attacker == 1 && c.victim == 2 && c.attackerTsMs == 123456 && c.part == 4);
    CHECK(c.masterMesh == "CharacterMesh0" && c.attachMarker == "DM Sphere Lowerarm_R 3" && c.frameBone == "lowerarm_r");
    CHECK(c.markers.size() == 3 && c.markers[2] == "DM Sphere Hand_R 1");
    CHECK(c.source == StrikeSource::WeaponRight && c.weaponComponent == "Blade");
    // In the bone frame the boxes sit where the capture put them, independent of the bone's world pose.
    CHECK(NearV3(c.box1.local.pos, {0, 500, 0}, 1e-6));
    CHECK(NearV3(c.box2.local.pos, {0, -500, 0}, 1e-6));
    CHECK(EqV3(c.box1.extent, in.box1Extent) && EqV3(c.box2.extent, in.box2Extent));

    // Same bone: the world transform comes back.
    Xform w1 = SeverBoxWorld(c.box1, bone);
    CHECK(NearV3(w1.pos, in.box1World.pos, 1e-6));
    V3 axis{1, 0, 0};
    CHECK(NearV3(TRot(w1.rot, axis), TRot(in.box1World.rot, axis), 1e-9));

    // The victim's bone is elsewhere and turned: the box lands at the same spot of that bone, turned with it.
    Xform victim;
    victim.pos = {-800, 300, 40};
    victim.rot = AxisAngleDeg(0, 0, 1, -120.0);
    victim.scale = {1, 1, 1};
    Xform w2 = SeverBoxWorld(c.box2, victim);
    CHECK(NearV3(w2.pos, victim.FromLocal({0, -500, 0}), 1e-6));
    // box rotation relative to the bone is preserved: bone^-1 * box is the same on both screens.
    V3 relAttacker = TRot(Q4{-bone.rot.x, -bone.rot.y, -bone.rot.z, bone.rot.w}, TRot(in.box2World.rot, axis));
    V3 relVictim = TRot(Q4{-victim.rot.x, -victim.rot.y, -victim.rot.z, victim.rot.w}, TRot(w2.rot, axis));
    CHECK(NearV3(relAttacker, relVictim, 1e-9));
}

static void TestCaptureRefusesAndTrims() {
    Xform bone;
    bone.rot = {0, 0, 0, 1};
    SeverClaim c;
    SeverCapture in = SampleCapture(bone);
    in.part = 15;
    CHECK(!MakeSeverClaim(in, c));
    in = SampleCapture(bone); in.attachMarker = "";
    CHECK(!MakeSeverClaim(in, c));
    in = SampleCapture(bone); in.attachMarker = std::string(kMaxClaimString + 1, 'a');
    CHECK(!MakeSeverClaim(in, c));
    in = SampleCapture(bone); in.frameBone = "";
    CHECK(!MakeSeverClaim(in, c));
    in = SampleCapture(bone); in.frameBone = std::string(kMaxClaimString + 1, 'b');
    CHECK(!MakeSeverClaim(in, c));
    in = SampleCapture(bone); in.attacker = in.victim;
    CHECK(!MakeSeverClaim(in, c));
    in = SampleCapture(bone); in.masterMesh = std::string(kMaxClaimString + 1, 'm');
    CHECK(!MakeSeverClaim(in, c));
    in = SampleCapture(bone); in.weaponComponent = std::string(kMaxClaimString + 1, 'w');
    CHECK(!MakeSeverClaim(in, c));
    in = SampleCapture(bone); in.box1Extent = {-1, 5, 5};
    CHECK(!MakeSeverClaim(in, c));
    in = SampleCapture(bone); in.box2World.pos = {kSeverMaxBoxOffset + 1, 0, 0};
    CHECK(!MakeSeverClaim(in, c));

    // Markers: names that would not survive the wire are dropped (a truncated name resolves to nothing), empty ones too,
    // and at most kSeverMaxMarkers are kept, in order.
    in = SampleCapture(bone);
    in.markers.clear();
    in.markers.push_back(std::string(kMaxClaimString + 1, 'x'));
    in.markers.push_back("");
    for (int i = 0; i < 20; ++i) in.markers.push_back("DM Sphere Calf_L " + std::to_string(i));
    CHECK(MakeSeverClaim(in, c));
    CHECK(c.markers.size() == kSeverMaxMarkers);
    CHECK(c.markers.front() == "DM Sphere Calf_L 0" && c.markers.back() == "DM Sphere Calf_L 15");
    // No overlapped markers at all is still a claim (the attach marker carries the cut).
    in.markers.clear();
    CHECK(MakeSeverClaim(in, c) && c.markers.empty());
    // A name of exactly kMaxClaimString survives.
    in = SampleCapture(bone); in.attachMarker = std::string(kMaxClaimString, 'k'); in.markers = {std::string(kMaxClaimString, 'k')};
    CHECK(MakeSeverClaim(in, c) && c.markers.size() == 1 && c.attachMarker.size() == kMaxClaimString);
}

// ---------------------------------------------------------------- the SeverClaim message
static SeverClaim SampleClaim() {
    Xform bone;
    bone.pos = {10, 20, 30};
    bone.rot = AxisAngleDeg(0, 0, 1, 90.0);
    SeverClaim c;
    SeverCapture in = SampleCapture(bone);
    in.markers.clear();
    for (size_t i = 0; i < kSeverMaxMarkers; ++i) in.markers.push_back(std::string(kMaxClaimString - 2, char('A' + i)) + "_" + char('a' + i));
    MakeSeverClaim(in, c);
    return c;
}

static bool SameClaim(const SeverClaim& a, const SeverClaim& b) {
    return a.claimId == b.claimId && a.attacker == b.attacker && a.victim == b.victim && a.attackerTsMs == b.attackerTsMs &&
           a.part == b.part && a.masterMesh == b.masterMesh && a.attachMarker == b.attachMarker && a.markers == b.markers &&
           a.frameBone == b.frameBone && EqXform(a.box1.local, b.box1.local) && EqV3(a.box1.extent, b.box1.extent) &&
           EqXform(a.box2.local, b.box2.local) && EqV3(a.box2.extent, b.box2.extent) && a.source == b.source &&
           a.weaponComponent == b.weaponComponent;
}

static void TestSeverRoundTrip() {
    SeverClaim c = SampleClaim();
    CHECK(c.markers.size() == kSeverMaxMarkers);
    std::vector<uint8_t> b = Encode(SeverClaimMsg{c});
    CHECK(b.size() <= kMaxPacket);
    SeverClaimMsg out;
    CHECK(Decode(b.data(), b.size(), out));
    CHECK(SameClaim(c, out.claim));

    // Every source survives; Body is allowed (a kick can carry a cut only through a stuck weapon, but the field is honest).
    for (StrikeSource s : {StrikeSource::Body, StrikeSource::WeaponRight, StrikeSource::WeaponLeft}) {
        c.source = s;
        b = Encode(SeverClaimMsg{c});
        CHECK(Decode(b.data(), b.size(), out) && out.claim.source == s);
    }
}

// Patches the byte at offset `at` of a freshly encoded sample (the layout is fixed by the encoder: see the offsets below).
static bool DecodePatched(const SeverClaim& c, size_t at, uint8_t v) {
    std::vector<uint8_t> b = Encode(SeverClaimMsg{c});
    if (at >= b.size()) return false;
    b[at] = v;
    SeverClaimMsg out;
    return Decode(b.data(), b.size(), out);
}

static void TestSeverMalformed() {
    SeverClaim c = SampleClaim();
    std::vector<uint8_t> b = Encode(SeverClaimMsg{c});
    SeverClaimMsg out;
    for (size_t n = 0; n < b.size(); ++n) CHECK(!Decode(b.data(), n, out));   // every truncation
    std::vector<uint8_t> longer = b; longer.push_back(0);
    CHECK(!Decode(longer.data(), longer.size(), out));                          // trailing byte
    std::vector<uint8_t> wrongType = b; wrongType[3] = uint8_t(MsgType::HitClaim);
    CHECK(!Decode(wrongType.data(), wrongType.size(), out));
    std::vector<uint8_t> wrongRev = b; wrongRev[2] = uint8_t(kProtocolRevision + 1);
    CHECK(!Decode(wrongRev.data(), wrongRev.size(), out));
    std::vector<uint8_t> huge(kMaxPacket + 1, 0); std::memcpy(huge.data(), b.data(), b.size());
    CHECK(!Decode(huge.data(), huge.size(), out));

    // Fixed offsets: header 4, claimId 4 -> attacker at 8, victim at 9, ts 10..13, part at 14.
    CHECK(DecodePatched(c, 8, 3));                    // control: a different valid attacker still decodes
    CHECK(!DecodePatched(c, 8, 0));
    CHECK(!DecodePatched(c, 8, uint8_t(kMaxWillies + 1)));
    CHECK(!DecodePatched(c, 9, 0));
    CHECK(!DecodePatched(c, 9, uint8_t(kMaxWillies + 1)));
    CHECK(!DecodePatched(c, 8, c.victim));            // attacker == victim
    CHECK(DecodePatched(c, 14, 14));                  // control: the last part
    CHECK(!DecodePatched(c, 14, 15));

    // Field-level rejects through the encoder.
    SeverClaim bad = c; bad.attachMarker = "";
    b = Encode(SeverClaimMsg{bad}); CHECK(!Decode(b.data(), b.size(), out));
    bad = c; bad.frameBone = "";
    b = Encode(SeverClaimMsg{bad}); CHECK(!Decode(b.data(), b.size(), out));
    bad = c; bad.markers.push_back("one too many");
    b = Encode(SeverClaimMsg{bad}); CHECK(!Decode(b.data(), b.size(), out));
    bad = c; bad.markers[3] = "";
    b = Encode(SeverClaimMsg{bad}); CHECK(!Decode(b.data(), b.size(), out));
    bad = c; bad.source = StrikeSource(3);
    b = Encode(SeverClaimMsg{bad}); CHECK(!Decode(b.data(), b.size(), out));
    bad = c; bad.box1.extent.y = -1;
    b = Encode(SeverClaimMsg{bad}); CHECK(!Decode(b.data(), b.size(), out));
    bad = c; bad.box2.extent.z = kSeverMaxExtent * 2;
    b = Encode(SeverClaimMsg{bad}); CHECK(!Decode(b.data(), b.size(), out));
    bad = c; bad.box1.local.pos.x = kSeverMaxBoxOffset * 2;
    b = Encode(SeverClaimMsg{bad}); CHECK(!Decode(b.data(), b.size(), out));
    bad = c; bad.box2.local.pos.z = -kSeverMaxBoxOffset * 2;
    b = Encode(SeverClaimMsg{bad}); CHECK(!Decode(b.data(), b.size(), out));
    bad = c; bad.box1.local.rot.w = std::nan("");
    b = Encode(SeverClaimMsg{bad}); CHECK(!Decode(b.data(), b.size(), out));
    bad = c; bad.box2.local.rot = {0, 0, 0, 0};     // not a rotation
    b = Encode(SeverClaimMsg{bad}); CHECK(!Decode(b.data(), b.size(), out));
    bad = c; bad.box1.local.scale = {1, 0, 1};       // a flat box places nothing
    b = Encode(SeverClaimMsg{bad}); CHECK(!Decode(b.data(), b.size(), out));
    // The boundary values themselves decode.
    bad = c; bad.box1.extent = {0, kSeverMaxExtent, 0}; bad.box2.local.pos = {kSeverMaxBoxOffset, -kSeverMaxBoxOffset, 0};
    b = Encode(SeverClaimMsg{bad}); CHECK(Decode(b.data(), b.size(), out));
}

// ---------------------------------------------------------------- attacker-side limiter
static void TestSeverLimiter() {
    SeverLimiter l;
    CHECK(l.Allow(2, 4, 1000.0));
    CHECK(!l.Allow(2, 4, 1000.0));
    CHECK(!l.Allow(2, 4, 1000.0 + kSeverRepeatMs - 1));
    CHECK(l.Allow(2, 5, 1001.0));                     // another part
    CHECK(l.Allow(3, 4, 1001.0));                     // another victim
    CHECK(l.Allow(2, 4, 1000.0 + kSeverRepeatMs));    // the window has passed
    CHECK(!l.Allow(2, 4, 1000.0 + kSeverRepeatMs + 1));
    CHECK(!l.Allow(2, 15, 5000.0));                   // no such part
    l.Forget(2);
    CHECK(l.Allow(2, 4, 1000.0 + kSeverRepeatMs + 2));   // a new fighter on that entity
    CHECK(!l.Allow(3, 4, 1002.0));                       // other victims are untouched
}

// ---------------------------------------------------------------- the victim's replay decision
static void TestInitiateAccepted() {
    CHECK(InitiateAccepted(false, true, 4, 4));
    CHECK(!InitiateAccepted(true, true, 4, 4));     // a sever was already in process: this one did not start
    CHECK(!InitiateAccepted(false, false, 4, 4));   // the gate refused it
    CHECK(!InitiateAccepted(false, true, 3, 4));    // something else started
}

// ---------------------------------------------------------------- claims waiting for the owner's vitals
static SeverClaim ClaimFor(uint8_t victim, uint8_t part, uint32_t id) {
    SeverClaim c = SampleClaim();
    c.victim = victim; c.part = part; c.claimId = id;
    return c;
}

static void TestSeverPending() {
    SeverPending p;
    SeverClaim got;
    CHECK(!p.Take(2, 4, 0.0, got));
    p.Put(ClaimFor(2, 4, 1), 1000.0);
    p.Put(ClaimFor(2, 9, 2), 1000.0);
    p.Put(ClaimFor(3, 4, 3), 1000.0);
    CHECK(p.Size() == 3);
    CHECK(!p.Take(2, 5, 1100.0, got));
    CHECK(p.Take(2, 4, 1100.0, got) && got.claimId == 1);
    CHECK(!p.Take(2, 4, 1100.0, got));                // taken once
    CHECK(p.Size() == 2);

    // A newer claim on the same part replaces the older one and restarts its time.
    p.Put(ClaimFor(2, 9, 4), 2000.0);
    CHECK(p.Size() == 2);
    CHECK(p.Take(2, 9, 2000.0 + kSeverPendingMs - 1, got) && got.claimId == 4);

    // Expiry: a claim the owner's vitals never confirmed goes after kSeverPendingMs.
    p.Put(ClaimFor(5, 1, 5), 3000.0);
    CHECK(!p.Take(5, 1, 3000.0 + kSeverPendingMs, got));
    p.Put(ClaimFor(5, 1, 6), 3000.0);
    p.Expire(3000.0 + kSeverPendingMs - 1);
    CHECK(p.Size() == 1);                             // (3,4) from t=1000 expired, (5,1) is still fresh
    CHECK(p.Has(5, 1) && !p.Has(3, 4));
    p.Expire(3000.0 + kSeverPendingMs);
    CHECK(p.Size() == 0);

    // Forget drops every part of that victim only.
    p.Put(ClaimFor(6, 0, 7), 0.0); p.Put(ClaimFor(6, 14, 8), 0.0); p.Put(ClaimFor(7, 0, 9), 0.0);
    p.Forget(6);
    CHECK(p.Size() == 1 && p.Has(7, 0));
    p.Clear();
    CHECK(p.Size() == 0);
    // Out-of-range parts are never stored.
    SeverClaim odd = ClaimFor(7, 3, 10); odd.part = 15;
    p.Put(odd, 0.0);
    CHECK(p.Size() == 0);
}

// ---------------------------------------------------------------- StuckBlade
static StuckBladeMsg SampleStuck() {
    StuckBladeMsg m;
    m.attacker = 1; m.victim = 2; m.source = StrikeSource::WeaponLeft; m.stuck = true; m.seq = 513;
    m.bone = "spine_03"; m.weaponComponent = "Blade";
    m.weaponInBone.pos = {3, -4, 5};
    m.weaponInBone.rot = AxisAngleDeg(0, 1, 0, 10.0);
    return m;
}

static void TestStuckBlade() {
    StuckBladeMsg m = SampleStuck(), out;
    std::vector<uint8_t> b = Encode(m);
    CHECK(Decode(b.data(), b.size(), out));
    CHECK(out.attacker == 1 && out.victim == 2 && out.source == StrikeSource::WeaponLeft && out.stuck && out.seq == 513);
    CHECK(out.bone == "spine_03" && out.weaponComponent == "Blade" && EqXform(out.weaponInBone, m.weaponInBone));
    for (size_t n = 0; n < b.size(); ++n) CHECK(!Decode(b.data(), n, out));
    std::vector<uint8_t> longer = b; longer.push_back(0);
    CHECK(!Decode(longer.data(), longer.size(), out));
    std::vector<uint8_t> wrongType = b; wrongType[3] = uint8_t(MsgType::SeverClaim);
    CHECK(!Decode(wrongType.data(), wrongType.size(), out));

    // The release carries no bone.
    StuckBladeMsg off = m; off.stuck = false; off.bone = "";
    b = Encode(off);
    CHECK(Decode(b.data(), b.size(), out) && !out.stuck && out.seq == 513);

    StuckBladeMsg bad = m; bad.bone = "";            // a blade stuck in no bone
    b = Encode(bad); CHECK(!Decode(b.data(), b.size(), out));
    bad = m; bad.source = StrikeSource::Body;          // only a held weapon sticks
    b = Encode(bad); CHECK(!Decode(b.data(), b.size(), out));
    bad = m; bad.source = StrikeSource(3);
    b = Encode(bad); CHECK(!Decode(b.data(), b.size(), out));
    bad = m; bad.attacker = 2;
    b = Encode(bad); CHECK(!Decode(b.data(), b.size(), out));
    bad = m; bad.victim = 0;
    b = Encode(bad); CHECK(!Decode(b.data(), b.size(), out));
    bad = m; bad.attacker = uint8_t(kMaxWillies + 1);
    b = Encode(bad); CHECK(!Decode(b.data(), b.size(), out));
    bad = m; bad.weaponInBone.pos.y = kSeverMaxBoxOffset * 2;
    b = Encode(bad); CHECK(!Decode(b.data(), b.size(), out));
    bad = m; bad.weaponInBone.rot = {0, 0, 0, 0};
    b = Encode(bad); CHECK(!Decode(b.data(), b.size(), out));
    // stuck is a strict bool. Fixed offsets: header 4 -> attacker 4, victim 5, source 6, stuck 7.
    b = Encode(m); b[7] = 2; CHECK(!Decode(b.data(), b.size(), out));
    b = Encode(m); b[7] = 1; CHECK(Decode(b.data(), b.size(), out));
}

// ---------------------------------------------------------------- the victim's stuck-blade set
static void TestStuckBlades() {
    StuckBlades s;
    StuckBladeMsg m = SampleStuck();
    CHECK(s.On(m, 1000.0) == StuckBlades::Change::Attach);
    CHECK(s.Count() == 1);
    CHECK(s.On(m, 1001.0) == StuckBlades::Change::None);          // a repeat of the same seq
    StuckBladeMsg moved = m; moved.seq = 514; moved.bone = "head";
    CHECK(s.On(moved, 1002.0) == StuckBlades::Change::Reattach);  // the same hand stuck again elsewhere
    CHECK(s.Count() == 1);
    StuckBladeMsg stale = m; stale.stuck = false; stale.seq = 513;
    CHECK(s.On(stale, 1003.0) == StuckBlades::Change::None);      // releases an older sticking: ignored
    StuckBladeMsg off = moved; off.stuck = false;
    CHECK(s.On(off, 1004.0) == StuckBlades::Change::Release);
    CHECK(s.Count() == 0);
    CHECK(s.On(off, 1005.0) == StuckBlades::Change::None);
    // An older seq arriving after a newer one changes nothing (wrap-aware).
    StuckBladeMsg late = m; late.seq = 512;
    CHECK(s.On(late, 1006.0) == StuckBlades::Change::None);
    StuckBladeMsg wrapped = m; wrapped.seq = 3;                    // 3 is newer than 514 only after a wrap: not here
    CHECK(s.On(wrapped, 1007.0) == StuckBlades::Change::None);

    // The seq wraps: 2 follows 65535.
    StuckBlades w;
    StuckBladeMsg top = SampleStuck(); top.seq = 65535;
    CHECK(w.On(top, 0.0) == StuckBlades::Change::Attach);
    StuckBladeMsg after = SampleStuck(); after.seq = 2; after.bone = "pelvis";
    CHECK(w.On(after, 1.0) == StuckBlades::Change::Reattach);
    StuckBladeMsg afterOff = after; afterOff.stuck = false;
    CHECK(w.On(afterOff, 2.0) == StuckBlades::Change::Release);
    CHECK(w.On(top, 3.0) == StuckBlades::Change::None);           // 65535 is older than 2 now

    // Both hands of one attacker are independent; a timeout releases them.
    StuckBlades t;
    StuckBladeMsg r = SampleStuck(); r.source = StrikeSource::WeaponRight; r.seq = 1;
    StuckBladeMsg l = SampleStuck(); l.source = StrikeSource::WeaponLeft; l.seq = 1;
    CHECK(t.On(r, 0.0) == StuckBlades::Change::Attach);
    CHECK(t.On(l, 5000.0) == StuckBlades::Change::Attach);
    CHECK(t.Count() == 2);
    std::vector<StuckBlades::Key> gone = t.Expire(kStuckBladeMaxMs - 1);
    CHECK(gone.empty());
    gone = t.Expire(kStuckBladeMaxMs);
    CHECK(gone.size() == 1 && gone[0].attacker == 1 && gone[0].source == StrikeSource::WeaponRight);
    CHECK(t.Count() == 1);
    // Forget an entity (attacker or victim) releases everything it is part of.
    std::vector<StuckBlades::Key> f = t.Forget(2);
    CHECK(f.size() == 1 && t.Count() == 0);
    CHECK(t.On(r, 9000.0) == StuckBlades::Change::Attach);
    f = t.Forget(1);
    CHECK(f.size() == 1 && t.Count() == 0);
}

int main() {
    TestSpawnBoneTable();
    TestCaptureAndReplayFrame();
    TestCaptureRefusesAndTrims();
    TestSeverRoundTrip();
    TestSeverMalformed();
    TestSeverLimiter();
    TestInitiateAccepted();
    TestSeverPending();
    TestStuckBlade();
    TestStuckBlades();
    if (g_fail) { std::printf("%d failure(s)\n", g_fail); return 1; }
    std::printf("test_hsm_sever: all passed\n");
    return 0;
}
