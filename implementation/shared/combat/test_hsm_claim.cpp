// Unit tests for hsm_claim.h. Run via implementation/shared/run_tests.sh test_hsm_claim.
#include "combat/hsm_claim.h"
#include "hsm_protocol.h"
#include <cmath>
#include <cstdio>
#include <cstring>
#include <random>
#include <vector>

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++g_fail; } } while (0)

using namespace hsm::combat;

static bool Near(double a, double b, double tol = 1e-6) { return std::fabs(a - b) <= tol; }
static double Len(const V3& v) { return std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z); }
static bool NearV3(const V3& a, const V3& b, double tol = 1e-6) {
    return Near(a.x, b.x, tol) && Near(a.y, b.y, tol) && Near(a.z, b.z, tol);
}
static bool EqV3(const V3& a, const V3& b) { return a.x == b.x && a.y == b.y && a.z == b.z; }
static bool EqQ4(const Q4& a, const Q4& b) { return a.x == b.x && a.y == b.y && a.z == b.z && a.w == b.w; }
static bool EqXform(const Xform& a, const Xform& b) { return EqV3(a.pos, b.pos) && EqQ4(a.rot, b.rot) && EqV3(a.scale, b.scale); }

// Test-local quaternion helpers (independent of the library's internals) for building fixtures.
static Q4 TQMul(const Q4& a, const Q4& b) {
    return {
        a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
        a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
        a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w,
        a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z,
    };
}
static Q4 AxisAngleDeg(double ax, double ay, double az, double degrees) {
    double half = degrees * M_PI / 180.0 / 2.0;
    double s = std::sin(half), c = std::cos(half);
    return {ax * s, ay * s, az * s, c};
}

// ---------------------------------------------------------------- scaled bone frame
// Stand-ins are spawned at the owner's height scale (0.875..1), so the bone transform a claim is
// taken in is scaled: the point's offset is divided by the scale (UE InverseTransformPosition),
// worked out by hand here, and a claim taken on a scaled stand-in lands on the same scaled spot.
static void TestScaledBoneFrame() {
    Xform bone;
    bone.pos = {10, 20, 30};
    bone.rot = AxisAngleDeg(0, 0, 1, 90.0);   // +90 deg about Z: (1,0,0) -> (0,1,0)
    bone.scale = {2, 4, 0.5};
    // point - pos = (2, 6, 1); unrotated by -90 deg about Z = (6, -2, 1); / scale = (3, -0.5, 2).
    CHECK(NearV3(bone.ToLocal({12, 26, 31}), {3, -0.5, 2}));
    CHECK(NearV3(bone.FromLocal({3, -0.5, 2}), {12, 26, 31}));

    Xform standIn;
    standIn.pos = {-40, 75, 110};
    standIn.rot = AxisAngleDeg(0, 1, 0, 25.0);
    standIn.scale = {0.9, 0.9, 0.9};
    DcdInputs in;
    in.hitBone = "upperarm_r";
    in.location = standIn.FromLocal({8, -2, 5});
    in.hasHitBox = false;
    HitClaim c = MakeClaim(in, standIn, 1, 1, 2, 1000, StrikeSource::Body, "", "");
    CHECK(NearV3(c.location, {8, -2, 5}));
    Xform victim = standIn;
    victim.pos = {300, -20, 95};
    victim.rot = AxisAngleDeg(0, 0, 1, -120.0);
    CHECK(NearV3(victim.ToLocal(victim.FromLocal(c.location)), {8, -2, 5}));
}

// ---------------------------------------------------------------- bone-frame round trip
static void TestBoneFrameRoundTrip() {
    Xform standIn;
    standIn.pos = {100, 200, 50};
    standIn.rot = AxisAngleDeg(0, 0, 1, 30.0);
    standIn.scale = {1, 1, 1};

    DcdInputs in;
    in.hitBone = "head";
    const V3 localPoint{5, -3, 10};
    in.location = standIn.FromLocal(localPoint);
    in.normal = {0.3, -0.1, 0.9};
    in.hitVelocity = {120, 0, -40};
    in.hitImpulse = {10, 20, 30};
    in.cuttingPower = 1.5; in.stabRate = 0.4; in.rigidity = 2.0; in.kickPower = 0; in.drawCut = 0;
    in.bluntDestruction = 0; in.lowerThreshold = false; in.damageParentBone = false; in.extraHighVelocity = false;
    in.hasHitBox = false;

    HitClaim c = MakeClaim(in, standIn, 1, 1, 2, 1000, StrikeSource::WeaponRight, "Sword_Blade", "");
    CHECK(NearV3(c.location, localPoint));
    CHECK(Near(Len(c.normal), Len(in.normal)));
    CHECK(Near(Len(c.hitVelocity), Len(in.hitVelocity)));
    CHECK(Near(Len(c.hitImpulse), Len(in.hitImpulse)));

    // A DIFFERENT victim bone pose: turned 90 degrees, moved, and leaning, with non-unit scale.
    Xform victimBone;
    victimBone.pos = {500, -250, 80};
    victimBone.rot = TQMul(AxisAngleDeg(0, 0, 1, 90.0), AxisAngleDeg(1, 0, 0, 15.0));
    victimBone.scale = {1.3, 0.8, 1.1};

    DcdInputs out = ReplayInputs(c, victimBone);
    CHECK(NearV3(out.location, victimBone.FromLocal(localPoint)));
    CHECK(Near(Len(out.normal), Len(in.normal)));
    CHECK(Near(Len(out.hitVelocity), Len(in.hitVelocity)));
    CHECK(Near(Len(out.hitImpulse), Len(in.hitImpulse)));

    // Round trip through the SAME bone pose recovers the original world inputs exactly.
    DcdInputs same = ReplayInputs(c, standIn);
    CHECK(NearV3(same.location, in.location));
    CHECK(NearV3(same.normal, in.normal));
    CHECK(NearV3(same.hitVelocity, in.hitVelocity));
    CHECK(NearV3(same.hitImpulse, in.hitImpulse));

    // Hit box carried through the bone frame too.
    DcdInputs inBox = in;
    inBox.hasHitBox = true;
    inBox.hitBoxWorld.pos = {110, 190, 55};
    inBox.hitBoxWorld.rot = AxisAngleDeg(0, 1, 0, 10.0);
    inBox.hitBoxWorld.scale = {1, 1, 1};
    inBox.hitBoxExtent = {4, 6, 8};
    HitClaim cBox = MakeClaim(inBox, standIn, 2, 1, 2, 1000, StrikeSource::Body, "", "upperarm_l");
    CHECK(cBox.hasHitBox);
    DcdInputs outBox = ReplayInputs(cBox, standIn);
    CHECK(outBox.hasHitBox);
    CHECK(NearV3(outBox.hitBoxWorld.pos, inBox.hitBoxWorld.pos));
    CHECK(NearV3(outBox.hitBoxExtent, inBox.hitBoxExtent));
}

// ---------------------------------------------------------------- DCD contact gate
static void TestGateTruthTable() {
    // scaled = impulseLen * (cuttingPower + 1)
    CHECK(DcdGatePasses(10.0, 1.0, 20.0, true));      // scaled == last, same bone: >= passes
    CHECK(!DcdGatePasses(10.0, 1.0, 20.01, true));    // scaled < last, same bone: fails
    CHECK(DcdGatePasses(10.0, 1.0, 20.01, false));    // scaled < last, different bone: passes (OR)
    CHECK(DcdGatePasses(0.0, 0.0, 0.0, true));        // first hit: zero impulse, lastImpulse 0 -> 0>=0 passes
    CHECK(!DcdGatePasses(0.0, 0.0, 5.0, true));       // zero impulse, nonzero last, same bone: fails
    CHECK(DcdGatePasses(0.0, 0.0, 5.0, false));       // zero impulse, different bone: passes
    CHECK(DcdGatePasses(5.0, 0.0, 5.0, true));        // exact equality edge: passes
    CHECK(Near(DcdGateNewImpulse(10.0, 1.0), 20.0));
    CHECK(Near(DcdGateNewImpulse(0.0, 0.0), 0.0));
}

// ---------------------------------------------------------------- limiter
static void TestLimiter() {
    ClaimLimiter lim;
    for (int i = 0; i < 4; ++i) CHECK(lim.Allow(7, "head"));
    CHECK(!lim.Allow(7, "head"));          // 5th on the same (victim, bone) this tick: refused
    CHECK(lim.Allow(7, "arm_l"));          // a different bone is independent
    CHECK(lim.Allow(9, "head"));           // a different victim is independent
    lim.NewTick();
    CHECK(lim.Allow(7, "head"));           // new tick resets the counters
    for (int i = 0; i < 3; ++i) CHECK(lim.Allow(7, "head"));
    CHECK(!lim.Allow(7, "head"));
}

// ---------------------------------------------------------------- dedupe
static void TestDedupe() {
    ClaimDedupe dd;
    CHECK(dd.Accept(1, 100));
    CHECK(!dd.Accept(1, 100));             // duplicate of the newest
    CHECK(dd.Accept(2, 100));              // a different (interleaved) attacker is independent
    CHECK(dd.Accept(1, 101));
    CHECK(!dd.Accept(1, 101));
    CHECK(dd.Accept(1, 99));               // an older id, still inside the window: new
    CHECK(!dd.Accept(1, 99));              // ...and now a duplicate

    // Advance well past the window: ids beyond it are refused (can no longer tell).
    for (uint32_t i = 102; i < 102 + 600; ++i) CHECK(dd.Accept(1, i));
    CHECK(!dd.Accept(1, 100));             // far outside the window now

    // Bounded memory under a million distinct ids for one attacker.
    ClaimDedupe big;
    for (uint32_t i = 0; i < 1000000; ++i) CHECK(big.Accept(3, i));
    CHECK(!big.Accept(3, 999999));         // duplicate of the newest
    CHECK(big.Accept(3, 1000000));         // still accepts new ids after a million
}

// A rejoined (restarted) attacker numbers its claims from 1 again: forgetting it accepts them, and only it.
static void TestDedupeForget() {
    ClaimDedupe dd;
    for (uint32_t i = 1; i <= 12; ++i) CHECK(dd.Accept(1, i));
    CHECK(dd.Accept(2, 5));
    CHECK(!dd.Accept(1, 1));               // before: the restarted numbering reads as a duplicate
    dd.Forget(1);
    CHECK(dd.Accept(1, 1));                // after: a fresh attacker
    CHECK(!dd.Accept(1, 1));               // whose new numbering is deduplicated as before
    CHECK(dd.Accept(1, 2));
    CHECK(!dd.Accept(2, 5));               // another attacker's state is untouched
    dd.Forget(9);                          // an attacker never seen: nothing happens
    CHECK(!dd.Accept(2, 5));
    CHECK(!dd.Accept(1, 2));
}

// ---------------------------------------------------------------- replay gate
static void TestReplayGate() {
    ReplayGate rg;
    HitClaim c1; c1.victim = 5; c1.attacker = 9; c1.hitBone = "head"; c1.attackerTsMs = 1000;

    ReplayGate::Decision d0 = rg.Before(c1);
    CHECK(d0.clearComplexImpulse);
    CHECK(!d0.keepLastDamage);             // nothing applied yet
    rg.Applied(c1);

    HitClaim c2 = c1; c2.attackerTsMs = 1000 + 199;
    ReplayGate::Decision d1 = rg.Before(c2);
    CHECK(d1.clearComplexImpulse && d1.keepLastDamage);    // same attacker+bone, 199ms: keep

    HitClaim c3 = c1; c3.attackerTsMs = 1000 + 200;
    ReplayGate::Decision d2 = rg.Before(c3);
    CHECK(d2.clearComplexImpulse && !d2.keepLastDamage);   // 200ms: clears

    HitClaim c4 = c1; c4.attacker = 10; c4.attackerTsMs = 1000 + 50;
    CHECK(!rg.Before(c4).keepLastDamage);  // different attacker: clears

    HitClaim c5 = c1; c5.hitBone = "torso"; c5.attackerTsMs = 1000 + 50;
    CHECK(!rg.Before(c5).keepLastDamage);  // different bone: clears

    // Complex impulse is always cleared, even on the keep path.
    CHECK(rg.Before(c2).clearComplexImpulse);
}

// ---------------------------------------------------------------- age rule
static void TestClaimFresh() {
    CHECK(ClaimFresh(0));
    CHECK(ClaimFresh(999));
    CHECK(!ClaimFresh(1000));
    CHECK(!ClaimFresh(5000));
    CHECK(ClaimFresh(-5));                 // early relative to the receiver's estimate: still fresh
}

// ---------------------------------------------------------------- bone rule
static void TestReplayBoneOk() {
    CHECK(ReplayBoneOk("None", -1));       // the game's own boneless hits (test 2: two claims refused as "bone 'None'")
    CHECK(ReplayBoneOk("None", 0));
    CHECK(ReplayBoneOk("hand_l", 0));
    CHECK(ReplayBoneOk("spine_02", 7));
    CHECK(!ReplayBoneOk("hand_l", -1));    // a bone my fighter lacks
    CHECK(!ReplayBoneOk("", 0));
    CHECK(!ReplayBoneOk("", -1));
    CHECK(!ReplayBoneOk("none", -1));      // only the engine's exact spelling of NAME_None
}

// ---------------------------------------------------------------- parry window
static void TestParryWindow() {
    ParryWindow pw;
    pw.NoteClash(1, 1000);
    HitClaim c; c.attacker = 1;

    c.attackerTsMs = 1150; CHECK(pw.Cancels(c));    // clash 150ms before the claim: edge, cancels
    c.attackerTsMs = 1151; CHECK(!pw.Cancels(c));   // 151ms before: does not
    c.attackerTsMs = 983;  CHECK(pw.Cancels(c));    // clash 17ms after the claim: edge, cancels
    c.attackerTsMs = 982;  CHECK(!pw.Cancels(c));   // 18ms after: does not

    HitClaim other; other.attacker = 2; other.attackerTsMs = 1000;
    CHECK(!pw.Cancels(other));                      // a different attacker never cancels
}

// ---------------------------------------------------------------- serialisation round trip
static std::string RandStr(std::mt19937& rng, size_t maxLen) {
    std::uniform_int_distribution<size_t> lenDist(0, maxLen);
    std::uniform_int_distribution<int> chDist('a', 'z');
    std::string s(lenDist(rng), ' ');
    for (char& ch : s) ch = char(chDist(rng));
    return s;
}
static double RandFinite(std::mt19937& rng) {
    std::uniform_real_distribution<double> d(-1e6, 1e6);
    return d(rng);
}
static V3 RandV3(std::mt19937& rng) { return {RandFinite(rng), RandFinite(rng), RandFinite(rng)}; }
static Q4 RandQ4(std::mt19937& rng) { return {RandFinite(rng), RandFinite(rng), RandFinite(rng), RandFinite(rng)}; }

static HitClaim RandClaim(std::mt19937& rng) {
    HitClaim c;
    c.claimId = uint32_t(rng());
    c.attacker = uint8_t(rng() % 256);
    c.victim = uint8_t(rng() % 256);
    c.attackerTsMs = uint32_t(rng());
    c.hitBone = RandStr(rng, kMaxClaimString);
    c.location = RandV3(rng); c.normal = RandV3(rng); c.hitVelocity = RandV3(rng); c.hitImpulse = RandV3(rng);
    c.cuttingPower = RandFinite(rng); c.stabRate = RandFinite(rng); c.rigidity = RandFinite(rng);
    c.kickPower = RandFinite(rng); c.drawCut = RandFinite(rng);
    c.bluntDestruction = int32_t(rng());
    c.lowerThreshold = (rng() % 2) == 0;
    c.damageParentBone = (rng() % 2) == 0;
    c.extraHighVelocity = (rng() % 2) == 0;
    c.source = StrikeSource(rng() % 3);
    c.strikeComponent = RandStr(rng, kMaxClaimString);
    c.strikeBone = RandStr(rng, kMaxClaimString);
    c.hasHitBox = (rng() % 2) == 0;
    c.hitBoxLocal.pos = RandV3(rng); c.hitBoxLocal.rot = RandQ4(rng); c.hitBoxLocal.scale = RandV3(rng);
    c.hitBoxExtent = RandV3(rng);
    return c;
}
static bool ClaimEq(const HitClaim& a, const HitClaim& b) {
    return a.claimId == b.claimId && a.attacker == b.attacker && a.victim == b.victim &&
           a.attackerTsMs == b.attackerTsMs && a.hitBone == b.hitBone &&
           EqV3(a.location, b.location) && EqV3(a.normal, b.normal) &&
           EqV3(a.hitVelocity, b.hitVelocity) && EqV3(a.hitImpulse, b.hitImpulse) &&
           a.cuttingPower == b.cuttingPower && a.stabRate == b.stabRate && a.rigidity == b.rigidity &&
           a.kickPower == b.kickPower && a.drawCut == b.drawCut &&
           a.bluntDestruction == b.bluntDestruction &&
           a.lowerThreshold == b.lowerThreshold && a.damageParentBone == b.damageParentBone &&
           a.extraHighVelocity == b.extraHighVelocity && a.source == b.source &&
           a.strikeComponent == b.strikeComponent && a.strikeBone == b.strikeBone &&
           a.hasHitBox == b.hasHitBox && EqXform(a.hitBoxLocal, b.hitBoxLocal) &&
           EqV3(a.hitBoxExtent, b.hitBoxExtent);
}

static void TestSerializationRoundTrip() {
    std::mt19937 rng(12345);
    for (int i = 0; i < 1000; ++i) {
        HitClaim c = RandClaim(rng);
        hsm::Writer w;
        PutClaim(w, c);
        hsm::Reader r(w.buf.data(), w.buf.size());
        HitClaim back;
        CHECK(GetClaim(r, back));
        CHECK(r.done());
        CHECK(ClaimEq(c, back));
    }
}

// ---------------------------------------------------------------- hostile decode
static void TestHostileDecode() {
    // A reference claim with known, short string lengths so byte offsets below are exact:
    // hitBone="h" (len 1), strikeComponent="w" (len 1), strikeBone="" (len 0).
    HitClaim ref;
    ref.claimId = 1; ref.attacker = 2; ref.victim = 3; ref.attackerTsMs = 4;
    ref.hitBone = "h";
    ref.location = {1, 2, 3}; ref.normal = {0, 0, 1}; ref.hitVelocity = {4, 5, 6}; ref.hitImpulse = {7, 8, 9};
    ref.cuttingPower = 1.0; ref.stabRate = 0.5; ref.rigidity = 2.0; ref.kickPower = 0.0; ref.drawCut = 0.0;
    ref.bluntDestruction = -7;
    ref.lowerThreshold = false; ref.damageParentBone = true; ref.extraHighVelocity = false;
    ref.source = StrikeSource::WeaponRight;
    ref.strikeComponent = "w"; ref.strikeBone = "";
    ref.hasHitBox = false; ref.hitBoxLocal = Xform{}; ref.hitBoxExtent = {0, 0, 0};

    hsm::Writer w0;
    PutClaim(w0, ref);
    // Fixed wire layout for these string lengths: 10 (header) + 2 (hitBone) + 4*24 (vectors) +
    // 5*8 (doubles) + 4 (bluntDestruction) + 4 (bools+source) + 2 (strikeComponent) + 1 (strikeBone)
    // + 1 (hasHitBox) + 80 (hitBoxLocal) + 24 (hitBoxExtent) = 264.
    CHECK(w0.buf.size() == 264);

    {
        HitClaim back;
        hsm::Reader r(w0.buf.data(), w0.buf.size());
        CHECK(GetClaim(r, back));
        CHECK(ClaimEq(ref, back));
    }

    // Every truncation of the buffer must fail, never crash.
    for (size_t n = 0; n < w0.buf.size(); ++n) {
        hsm::Reader r(w0.buf.data(), n);
        HitClaim tmp;
        CHECK(!GetClaim(r, tmp));
    }
    // Trailing garbage also fails (exact-size framing: r.done() requires p_ == n_).
    {
        std::vector<uint8_t> buf = w0.buf;
        buf.push_back(0);
        hsm::Reader r(buf.data(), buf.size());
        HitClaim tmp;
        CHECK(!GetClaim(r, tmp));
    }
    // NaN injection into cuttingPower (offset 108, 8 bytes).
    {
        std::vector<uint8_t> buf = w0.buf;
        uint64_t nanBits = 0x7FF8000000000000ull;
        std::memcpy(buf.data() + 108, &nanBits, 8);
        hsm::Reader r(buf.data(), buf.size());
        HitClaim tmp;
        CHECK(!GetClaim(r, tmp));
    }
    // Infinity injection into the same field.
    {
        std::vector<uint8_t> buf = w0.buf;
        uint64_t infBits = 0x7FF0000000000000ull;
        std::memcpy(buf.data() + 108, &infBits, 8);
        hsm::Reader r(buf.data(), buf.size());
        HitClaim tmp;
        CHECK(!GetClaim(r, tmp));
    }
    // NaN injection into a hit-box quaternion component (hitBoxLocal.rot starts at offset 184).
    {
        std::vector<uint8_t> buf = w0.buf;
        uint64_t nanBits = 0x7FF8000000000000ull;
        std::memcpy(buf.data() + 184, &nanBits, 8);
        hsm::Reader r(buf.data(), buf.size());
        HitClaim tmp;
        CHECK(!GetClaim(r, tmp));
    }
    // Overlong string: hitBone's length byte (offset 10) set beyond kMaxClaimString.
    {
        std::vector<uint8_t> buf = w0.buf;
        buf[10] = 200;
        hsm::Reader r(buf.data(), buf.size());
        HitClaim tmp;
        CHECK(!GetClaim(r, tmp));
    }
    // Overlong string: strikeComponent's length byte (offset 156).
    {
        std::vector<uint8_t> buf = w0.buf;
        buf[156] = 255;
        hsm::Reader r(buf.data(), buf.size());
        HitClaim tmp;
        CHECK(!GetClaim(r, tmp));
    }
    // Bad enum: source byte (offset 155) set to an undefined StrikeSource value.
    {
        std::vector<uint8_t> buf = w0.buf;
        buf[155] = 3;
        hsm::Reader r(buf.data(), buf.size());
        HitClaim tmp;
        CHECK(!GetClaim(r, tmp));
    }
    // Bad bool: lowerThreshold byte (offset 152) set to something other than 0/1.
    {
        std::vector<uint8_t> buf = w0.buf;
        buf[152] = 2;
        hsm::Reader r(buf.data(), buf.size());
        HitClaim tmp;
        CHECK(!GetClaim(r, tmp));
    }
    // Bad bool: hasHitBox byte (offset 159).
    {
        std::vector<uint8_t> buf = w0.buf;
        buf[159] = 42;
        hsm::Reader r(buf.data(), buf.size());
        HitClaim tmp;
        CHECK(!GetClaim(r, tmp));
    }

    // Purely random bytes, random lengths: must never crash (ASan/UBSan aborts the test on a real
    // bug). The result is otherwise unconstrained -- a random buffer could coincidentally decode.
    std::mt19937 rng(777);
    std::uniform_int_distribution<size_t> lenDist(0, 600);
    std::uniform_int_distribution<int> byteDist(0, 255);
    for (int i = 0; i < 2000; ++i) {
        std::vector<uint8_t> buf(lenDist(rng));
        for (uint8_t& b : buf) b = uint8_t(byteDist(rng));
        hsm::Reader r(buf.data(), buf.size());
        HitClaim tmp;
        (void)GetClaim(r, tmp);
    }
}

int main() {
    TestBoneFrameRoundTrip();
    TestScaledBoneFrame();
    TestGateTruthTable();
    TestLimiter();
    TestDedupe();
    TestDedupeForget();
    TestReplayGate();
    TestClaimFresh();
    TestReplayBoneOk();
    TestParryWindow();
    TestSerializationRoundTrip();
    TestHostileDecode();
    if (g_fail) { std::printf("test_hsm_claim: %d failure(s)\n", g_fail); return 1; }
    std::printf("test_hsm_claim: all passed\n");
    return 0;
}
