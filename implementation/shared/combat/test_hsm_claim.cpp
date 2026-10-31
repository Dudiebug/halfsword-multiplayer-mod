// Unit tests for hsm_claim.h. Run via implementation/shared/run_tests.sh test_hsm_claim.
#include "combat/hsm_claim.h"
#include "hsm_protocol.h"
#include "session/hsm_lease.h"
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <random>
#include <set>
#include <string>
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
    pw.NoteClash(1, StrikeSource::WeaponRight, 1000);
    HitClaim c; c.attacker = 1; c.source = StrikeSource::WeaponRight;

    c.attackerTsMs = 1150; CHECK(pw.Cancels(c));    // clash 150ms before the claim: edge, cancels
    c.attackerTsMs = 1151; CHECK(!pw.Cancels(c));   // 151ms before: does not
    c.attackerTsMs = 983;  CHECK(pw.Cancels(c));    // clash 17ms after the claim: edge, cancels
    c.attackerTsMs = 982;  CHECK(!pw.Cancels(c));   // 18ms after: does not

    HitClaim other; other.attacker = 2; other.attackerTsMs = 1000; other.source = StrikeSource::WeaponRight;
    CHECK(!pw.Cancels(other));                      // a different attacker never cancels
}

// Test 4: a clash between my blade and the attacker's right weapon also cancelled that attacker's left-weapon blows,
// punches and kicks for 150 ms, so in a bind nothing else it did landed. A clash parries only the weapon that clashed.
static void TestParryWindowPerWeapon() {
    ParryWindow pw;
    pw.NoteClash(1, StrikeSource::WeaponRight, 1000);
    HitClaim right; right.attacker = 1; right.attackerTsMs = 1000; right.source = StrikeSource::WeaponRight;
    HitClaim left = right; left.source = StrikeSource::WeaponLeft;
    HitClaim body = right; body.source = StrikeSource::Body;
    CHECK(pw.Cancels(right));
    CHECK(!pw.Cancels(left));                       // the other weapon did not clash
    CHECK(!pw.Cancels(body));                       // a kick or a punch is never parried
    pw.NoteClash(1, StrikeSource::WeaponLeft, 1000);
    CHECK(pw.Cancels(left));
    CHECK(!pw.Cancels(body));
    pw.NoteClash(1, StrikeSource::Body, 1000);      // not a clash anything reports; still no parry of a body strike
    CHECK(!pw.Cancels(body));

    // An entity id is reused by the next fighter (respawn, rejoin): its old clashes go with Forget, and only its own.
    pw.NoteClash(2, StrikeSource::WeaponRight, 1000);
    pw.Forget(1);
    CHECK(!pw.Cancels(right));
    CHECK(!pw.Cancels(left));
    HitClaim two = right; two.attacker = 2;
    CHECK(pw.Cancels(two));
    pw.NoteClash(1, StrikeSource::WeaponRight, 1000);
    CHECK(pw.Cancels(right));                       // a clash after the Forget counts again
}

// Logs 3-9: one blow in seven was parried, and the log could not tell a blade sliding off in a bind (clash right before
// the blow) from a swing that glanced off and went on into the body (clash up to 150 ms before it). Cancels reports
// how far the nearest matching clash was from the blow (clash minus claim, in the attacker's ms) for the parried line.
static void TestParryWindowDelta() {
    ParryWindow pw;
    pw.NoteClash(1, StrikeSource::WeaponRight, 1000);
    pw.NoteClash(1, StrikeSource::WeaponLeft, 1100);   // the other weapon: never the one reported
    pw.NoteClash(1, StrikeSource::WeaponRight, 1060);
    HitClaim c; c.attacker = 1; c.source = StrikeSource::WeaponRight;

    int32_t delta = 12345;
    c.attackerTsMs = 1100; CHECK(pw.Cancels(c, &delta)); CHECK(delta == -40);   // nearest of 1000 and 1060
    delta = 12345;
    c.attackerTsMs = 1150; CHECK(pw.Cancels(c, &delta)); CHECK(delta == -90);   // 1000 is out of the window
    delta = 12345;
    c.attackerTsMs = 1050; CHECK(pw.Cancels(c, &delta)); CHECK(delta == 10);    // a clash just after the blow
    delta = 12345;
    c.attackerTsMs = 1030; CHECK(pw.Cancels(c, &delta)); CHECK(delta == -30);   // 1060 is nearer by distance but past kParryAfterMs
    delta = 12345;
    c.attackerTsMs = 1300; CHECK(!pw.Cancels(c, &delta)); CHECK(delta == 12345); // not parried: untouched
    c.attackerTsMs = 1100; CHECK(pw.Cancels(c));                                 // the out pointer is optional

    ParryWindow tie;
    tie.NoteClash(1, StrikeSource::WeaponRight, 1010);
    tie.NoteClash(1, StrikeSource::WeaponRight, 990);
    c.attackerTsMs = 1000; delta = 12345;
    CHECK(tie.Cancels(c, &delta)); CHECK(delta == -10);                          // a tie in distance goes to the earlier clash
}

// ---------------------------------------------------------------- body parts
static void TestBodyPartOfBone() {
    // One target per part: the two neck bones of test 4's storm, the head, any casing (test 4 logged "Head").
    CHECK(BodyPartOfBone("neck_01") == kBodyPartHead);
    CHECK(BodyPartOfBone("neck_02") == kBodyPartHead);
    CHECK(BodyPartOfBone("head") == kBodyPartHead);
    CHECK(BodyPartOfBone("Head") == kBodyPartHead);
    CHECK(BodyPartOfBone("NECK_01") == kBodyPartHead);
    for (const char* b : {"pelvis", "spine_01", "spine_03", "spine_05", "clavicle_l", "clavicle_r"})
        CHECK(BodyPartOfBone(b) == kBodyPartTorso);
    CHECK(BodyPartOfBone("hand_l") == kBodyPartHandL);
    CHECK(BodyPartOfBone("hand_r") == kBodyPartHandR);
    CHECK(BodyPartOfBone("hand_r") != BodyPartOfBone("hand_l"));
    CHECK(BodyPartOfBone("index_01_l") == kBodyPartHandL);         // fingers are the hand
    CHECK(BodyPartOfBone("thumb_03_r") == kBodyPartHandR);
    CHECK(BodyPartOfBone("upperarm_twist_01_l") == kBodyPartUpperArmL);
    CHECK(BodyPartOfBone("lowerarm_twist_01_r") == kBodyPartLowerArmR);
    CHECK(BodyPartOfBone("thigh_twist_01_r") == kBodyPartThighR);
    CHECK(BodyPartOfBone("ball_l") == kBodyPartFootL);             // toes are the foot
    CHECK(BodyPartOfBone("ball_r") == kBodyPartFootR);
    CHECK(BodyPartOfBone("Calf_R") == kBodyPartCalfR);
    for (const char* b : {"", "None", "root", "hand", "neck", "spine", "weapon_l", "ik_hand_l", "_l", "l"})
        CHECK(BodyPartOfBone(b) == kBodyPartOther);
    // Every limb part is its own: fourteen bones, fourteen parts, none Other.
    const char* limbs[] = {"head", "pelvis", "upperarm_l", "lowerarm_l", "hand_l", "upperarm_r", "lowerarm_r", "hand_r",
                           "thigh_l", "calf_l", "foot_l", "thigh_r", "calf_r", "foot_r"};
    std::set<uint8_t> parts;
    for (const char* b : limbs) { CHECK(BodyPartOfBone(b) != kBodyPartOther); parts.insert(BodyPartOfBone(b)); }
    CHECK(parts.size() == 14);
    // Every bone a pose carries is some body part.
    for (const char* b : hsm::sync::kPoseBoneNames) CHECK(BodyPartOfBone(b) != kBodyPartOther);
}

// ---------------------------------------------------------------- blow coalescer
using Verdict = BlowCoalescer::Verdict;
static BlowKey Key(const char* bone, const char* comp = "Blade", uint8_t attacker = 1, uint8_t victim = 2) {
    BlowKey k;
    k.attacker = attacker; k.victim = victim; k.strikeComponent = comp; k.bodyPart = BodyPartOfBone(bone);
    return k;
}

static void TestCoalescerNeckStorm() {
    // Test 4: twenty contacts 73 ms apart, alternating neck_01 / neck_02 (one blow). Strength is the stand-in's Damage
    // Out (rev 25): with damage, one send per 250 ms window: at 0, 292, 584, 876 and 1168 ms (contacts 0, 4, 8, 12,
    // 16), the rest the same contact again.
    BlowCoalescer bc;
    int sent = 0;
    for (int i = 0; i < 20; ++i) {
        const Verdict v = bc.Allow(Key(i % 2 ? "neck_02" : "neck_01"), 700.0, 1000.0 + 73.0 * i);
        const bool expectSend = i % 4 == 0;
        CHECK(v == (expectSend ? Verdict::Send : Verdict::Coalesced));
        sent += v == Verdict::Send;
    }
    CHECK(sent == 5);
    // Negative control: the same storm with no damage (a blade resting on the neck) sends nothing at all.
    BlowCoalescer rest;
    for (int i = 0; i < 20; ++i) CHECK(rest.Allow(Key(i % 2 ? "neck_02" : "neck_01"), 0.0, 1000.0 + 73.0 * i) == Verdict::ZeroImpulse);
}

static void TestCoalescerWindow() {
    BlowCoalescer bc;
    CHECK(bc.Allow(Key("head"), 5.0, 0) == Verdict::Send);
    CHECK(bc.Allow(Key("head"), 5.0, 249) == Verdict::Coalesced);     // still the same blow
    CHECK(bc.Allow(Key("head"), 5.0, 250) == Verdict::Send);          // a new window
    BlowCoalescer two;                                                // two blows 300 ms apart both land
    CHECK(two.Allow(Key("spine_02"), 400.0, 0) == Verdict::Send);
    CHECK(two.Allow(Key("spine_02"), 400.0, 300) == Verdict::Send);
}

static void TestCoalescerStronger() {
    BlowCoalescer bc;
    CHECK(bc.Allow(Key("thigh_l"), 100.0, 0) == Verdict::Send);
    CHECK(bc.Allow(Key("thigh_l"), 100.0, 10) == Verdict::Coalesced);   // not stronger
    CHECK(bc.Allow(Key("calf_l"), 90.0, 15) == Verdict::Send);          // another part is another target
    CHECK(bc.Allow(Key("thigh_l"), 150.0, 20) == Verdict::Send);        // stronger: the one more
    CHECK(bc.Allow(Key("thigh_l"), 500.0, 30) == Verdict::Coalesced);   // only one more per window
    CHECK(bc.Allow(Key("thigh_l"), 0.0, 40) == Verdict::ZeroImpulse);
    CHECK(bc.Allow(Key("thigh_l"), 100.0, 250) == Verdict::Send);       // the next window
    BlowCoalescer z;                                                    // a graze that did nothing, then a real hit
    CHECK(z.Allow(Key("hand_r"), 0.0, 0) == Verdict::ZeroImpulse);      // a zero-damage first contact is dropped and opens no window
    CHECK(z.Allow(Key("hand_r"), 0.0, 5) == Verdict::ZeroImpulse);
    CHECK(z.Allow(Key("hand_r"), 5.0, 10) == Verdict::Send);
    CHECK(z.Allow(Key("hand_r"), 9.0, 20) == Verdict::Send);            // the one more
    CHECK(z.Allow(Key("hand_r"), 12.0, 30) == Verdict::Coalesced);
    BlowCoalescer nan;                                                  // a non-number or negative counts as zero
    CHECK(nan.Allow(Key("head"), std::nan(""), 0) == Verdict::ZeroImpulse);
    CHECK(nan.Allow(Key("head"), -3.0, 1) == Verdict::ZeroImpulse);
    CHECK(nan.Allow(Key("head"), 1.0, 2) == Verdict::Send);
    // Zero-damage contacts spend nothing of the per-second cap.
    BlowCoalescer cap;
    for (int i = 0; i < 40; ++i) CHECK(cap.Allow(Key("head", "Blade", 1, 2), 0.0, i) == Verdict::ZeroImpulse);
    CHECK(cap.Allow(Key("head", "Blade", 1, 2), 3.0, 41) == Verdict::Send);
}

// Stuck-blade damage: one per 100 ms per (attacker, victim).
static void TestStuckDamageLimiter() {
    StuckDamageLimiter l;
    CHECK(l.Allow(1, 2, 0));                          // the first is always sent (even at t = 0)
    CHECK(!l.Allow(1, 2, 1));                         // a physics tick later: refused
    CHECK(!l.Allow(1, 2, 99.9));
    CHECK(l.Allow(1, 2, 100));                        // the interval has passed
    CHECK(!l.Allow(1, 2, 150));                       // a refusal does not restart the interval...
    CHECK(l.Allow(1, 2, 200));                        // ...so this is 100 ms after the last send
    CHECK(l.Allow(1, 3, 201));                        // another victim
    CHECK(l.Allow(4, 2, 201));                        // another attacker
    CHECK(l.Allow(2, 1, 201));                        // the reverse pair is its own
    l.Forget(2);                                      // entity 2 left: both directions start over
    CHECK(l.Allow(1, 2, 202));
    CHECK(l.Allow(4, 2, 202));
    CHECK(!l.Allow(1, 3, 202));                       // others untouched
    CHECK(l.Allow(1, 3, 100000));
    CHECK(l.Allow(1, 3, 5));                          // a clock that went back restarts
    // Negative control: 60 Hz ticks for a second send 10, not 60.
    StuckDamageLimiter hz;
    int sent = 0;
    for (int i = 0; i < 60; ++i) sent += hz.Allow(1, 2, 1000.0 + i * (1000.0 / 60.0));
    CHECK(sent == 10);
}

static void TestCoalescerKeys() {
    BlowCoalescer bc;
    CHECK(bc.Allow(Key("head"), 50.0, 0) == Verdict::Send);
    CHECK(bc.Allow(Key("head", "Pommel"), 50.0, 1) == Verdict::Send);          // another striking component
    CHECK(bc.Allow(Key("head", "Blade", 3), 50.0, 2) == Verdict::Send);        // another attacker
    CHECK(bc.Allow(Key("head", "Blade", 1, 4), 50.0, 3) == Verdict::Send);     // another victim
    CHECK(bc.Allow(Key("hand_l"), 50.0, 4) == Verdict::Send);                  // another part
    CHECK(bc.Allow(Key("head"), 50.0, 5) == Verdict::Coalesced);
}

static void TestCoalescerCap() {
    // Forty strengthening contacts in one second on eight parts: each would send five times (a window opens every
    // 400 ms, one stronger contact 200 ms later), the cap lets the first twelve through.
    const char* bones[] = {"head", "pelvis", "hand_l", "hand_r", "thigh_l", "thigh_r", "foot_l", "foot_r"};
    BlowCoalescer bc;
    int sent = 0, capped = 0;
    for (int i = 0; i < 40; ++i) {
        const Verdict v = bc.Allow(Key(bones[i % 8]), 100.0 + i, 25.0 * i);
        CHECK(v == (i < 12 ? Verdict::Send : Verdict::Capped));
        sent += v == Verdict::Send;
        capped += v == Verdict::Capped;
    }
    CHECK(sent == 12 && capped == 28);
    CHECK(bc.Allow(Key("head", "Blade", 1, 3), 10.0, 500) == Verdict::Send);    // per (attacker, victim)
    CHECK(bc.Allow(Key("head", "Blade", 5, 2), 10.0, 500) == Verdict::Send);
    CHECK(bc.Allow(Key("calf_l"), 10.0, 999) == Verdict::Capped);               // the first send is 999 ms old
    CHECK(bc.Allow(Key("calf_l"), 10.0, 1000) == Verdict::Send);                // 1000 ms: it has left the window
    CHECK(bc.Allow(Key("calf_r"), 10.0, 1000) == Verdict::Capped);              // and only it
}

static void TestCoalescerForget() {
    BlowCoalescer bc;
    for (int i = 0; i < 12; ++i) {                                            // twelve components fill the cap
        const std::string comp = "Part" + std::to_string(i);
        CHECK(bc.Allow(Key("head", comp.c_str(), 1, 2), 10.0, i) == Verdict::Send);
    }
    CHECK(bc.Allow(Key("head", "Blade", 1, 2), 10.0, 20) == Verdict::Capped);
    CHECK(bc.Allow(Key("head", "Blade", 3, 4), 10.0, 20) == Verdict::Send);
    bc.Forget(2);                                                             // the victim respawned
    CHECK(bc.Allow(Key("head", "Blade", 1, 2), 10.0, 21) == Verdict::Send);
    CHECK(bc.Allow(Key("head", "Blade", 3, 4), 10.0, 22) == Verdict::Coalesced);   // others untouched
    bc.Forget(3);                                                             // as attacker
    CHECK(bc.Allow(Key("head", "Blade", 3, 4), 10.0, 23) == Verdict::Send);
}

// ---------------------------------------------------------------- replay queue
static HitClaim QClaim(uint8_t victim, uint32_t id) {
    HitClaim c; c.victim = victim; c.attacker = 1; c.claimId = id;
    return c;
}

static void TestReplayQueue() {
    ReplayQueue q;
    for (uint32_t id = 1; id <= 20; ++id) CHECK(q.Push(QClaim(5, id)));
    CHECK(q.Size(5) == 20);
    uint32_t next = 1;
    for (size_t want : {8u, 8u, 4u, 0u}) {
        const std::vector<HitClaim> got = q.Drain();
        CHECK(got.size() == want);
        for (const HitClaim& c : got) CHECK(c.victim == 5 && c.claimId == next++);   // arrival order
    }
    CHECK(q.Size(5) == 0);

    ReplayQueue two;                                       // per victim
    for (uint32_t id = 1; id <= 10; ++id) two.Push(QClaim(5, id));
    for (uint32_t id = 1; id <= 3; ++id) two.Push(QClaim(6, id));
    std::vector<HitClaim> got = two.Drain();
    CHECK(got.size() == 11);
    CHECK(two.Size(5) == 2 && two.Size(6) == 0);
    two.Push(QClaim(6, 4));
    two.Forget(5);
    got = two.Drain();
    CHECK(got.size() == 1 && got[0].victim == 6 && got[0].claimId == 4);
    CHECK(two.Drain(3).empty());

    ReplayQueue cap;                                       // a flood is bounded
    for (uint32_t id = 1; id <= kMaxQueuedReplays; ++id) CHECK(cap.Push(QClaim(7, id)));
    CHECK(!cap.Push(QClaim(7, 1000)));
    CHECK(cap.Size(7) == kMaxQueuedReplays);
    CHECK(cap.Push(QClaim(8, 1)));                         // another victim still has room
    CHECK(cap.Drain(2).size() == 3);
    CHECK(cap.Push(QClaim(7, 1001)));                      // room again after a drain
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
    if (rng() % 2) {   // struck by a prop: a level prop or a dropped weapon, never from a hand
        c.striker = StrikerKind::Prop;
        c.propId = rng() % 2 ? hsm::MakeLevelPropId(uint16_t(rng())) : hsm::MakeDropPropId(uint8_t(1 + rng() % 16), uint8_t(rng() % 2), uint8_t(rng()), uint32_t(rng()));
        c.propVelocity = RandV3(rng);
        c.source = StrikeSource::Body;
    }
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
           EqV3(a.hitBoxExtent, b.hitBoxExtent) && a.striker == b.striker && a.propId == b.propId &&
           EqV3(a.propVelocity, b.propVelocity) && a.rawDamage == b.rawDamage;
}

static void TestSerializationRoundTrip() {
    std::mt19937 rng(12345);
    int props = 0;
    for (int i = 0; i < 1000; ++i) {
        HitClaim c = RandClaim(rng);
        hsm::Writer w;
        PutClaim(w, c);
        hsm::Reader r(w.buf.data(), w.buf.size());
        HitClaim back;
        CHECK(GetClaim(r, back));
        CHECK(r.done());
        CHECK(ClaimEq(c, back));
        props += c.striker == StrikerKind::Prop;
    }
    CHECK(props > 300 && props < 700);   // both kinds were round-tripped
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
    // + 1 (hasHitBox) + 80 (hitBoxLocal) + 24 (hitBoxExtent) + 1 (striker) + 8 (propId) + 24 (propVelocity) + 1 (victimLife, rev 19) + 8 (rawDamage, rev 25) = 306.
    CHECK(w0.buf.size() == 306);

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
    // Bad enum: striker byte (offset 264) set to an undefined StrikerKind value.
    {
        std::vector<uint8_t> buf = w0.buf;
        buf[264] = 3;
        hsm::Reader r(buf.data(), buf.size());
        HitClaim tmp;
        CHECK(!GetClaim(r, tmp));
    }
    // A fighter's claim names no prop: a prop id (offset 265) or a prop velocity (offset 273) on one is garbage.
    {
        std::vector<uint8_t> buf = w0.buf;
        buf[265] = 1;
        hsm::Reader r(buf.data(), buf.size());
        HitClaim tmp;
        CHECK(!GetClaim(r, tmp));
        buf = w0.buf;
        const double one = 1.0;
        std::memcpy(buf.data() + 273 + 16, &one, 8);   // propVelocity.z
        hsm::Reader r2(buf.data(), buf.size());
        CHECK(!GetClaim(r2, tmp));
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

// ---------------------------------------------------------------- props (test 5: rocks did no damage)
static HitClaim PropRef() {
    HitClaim c;
    c.claimId = 9; c.attacker = 1; c.victim = 2; c.attackerTsMs = 500;
    c.hitBone = "head"; c.hitImpulse = {0, 0, 900}; c.strikeComponent = "StaticMeshComponent0";
    Xform bone;
    bone.pos = {100, 0, 160};
    bone.rot = AxisAngleDeg(0, 0, 1, 90.0);
    StruckByProp(c, hsm::MakeLevelPropId(14), {300, 0, -50}, bone);
    return c;
}

// The prop's velocity at the contact travels in the hit bone's frame like every other direction (rotation only), and
// the claim is a body blow: no hand holds a thrown rock, and no clash parries it.
static void TestPropStriker() {
    Xform bone;
    bone.pos = {100, 0, 160};
    bone.rot = AxisAngleDeg(0, 0, 1, 90.0);   // +90 deg about Z: (1,0,0) -> (0,1,0)
    bone.scale = {0.9, 0.9, 0.9};
    HitClaim c;
    c.source = StrikeSource::WeaponRight;
    c.propVelocity = {7, 7, 7};
    StruckByProp(c, hsm::MakeLevelPropId(14), {300, 0, -50}, bone);
    CHECK(c.striker == StrikerKind::Prop);
    CHECK(c.propId == hsm::MakeLevelPropId(14));
    CHECK(c.source == StrikeSource::Body);
    CHECK(NearV3(c.propVelocity, {0, -300, -50}));                    // unrotated by -90 deg about Z, scale ignored
    CHECK(NearV3(PropVelocityWorld(c, bone), {300, 0, -50}));
    Xform victim = bone;
    victim.rot = AxisAngleDeg(0, 0, 1, 180.0);                          // the victim's bone faces the other way
    CHECK(NearV3(PropVelocityWorld(c, victim), {0, 300, -50}));
    CHECK(Near(Len(PropVelocityWorld(c, victim)), Len(V3{300, 0, -50})));
    HitClaim fighter;
    CHECK(fighter.striker == StrikerKind::Fighter && fighter.propId == 0);   // a claim is a fighter's unless said
    ParryWindow pw;
    pw.NoteClash(1, StrikeSource::WeaponRight, 500);
    pw.NoteClash(1, StrikeSource::WeaponLeft, 500);
    HitClaim p = PropRef();
    CHECK(!pw.Cancels(p));
}

static void TestPropClaimDecode() {
    const HitClaim ref = PropRef();
    auto roundTrips = [](const HitClaim& c, HitClaim& back) {
        hsm::Writer w;
        PutClaim(w, c);
        hsm::Reader r(w.buf.data(), w.buf.size());
        return GetClaim(r, back);
    };
    HitClaim back;
    CHECK(roundTrips(ref, back) && ClaimEq(ref, back));
    HitClaim m = ref;
    m.propId = hsm::MakeDropPropId(3, 1, 5, 77);   // a thrown weapon
    CHECK(roundTrips(m, back) && back.propId == m.propId);
    m = ref; m.propId = 0;                          CHECK(!roundTrips(m, back));   // a prop claim names its prop
    m = ref; m.propId = 0x0005;                     CHECK(!roundTrips(m, back));   // a weapon id with no owner entity
    m = ref; m.propId = uint64_t(1) << 50 | 0x0105; CHECK(!roundTrips(m, back));   // bits above a weapon id
    m = ref; m.source = StrikeSource::WeaponRight;  CHECK(!roundTrips(m, back));   // no hand holds a thrown prop
    m = ref; m.source = StrikeSource::WeaponLeft;   CHECK(!roundTrips(m, back));
    m = ref; m.propVelocity = {0, 0, 0};            CHECK(roundTrips(m, back));    // a prop at rest still strikes
    m = ref; m.striker = StrikerKind::Fighter;      CHECK(!roundTrips(m, back));   // a fighter's claim names no prop
    m.propId = 0;                                   CHECK(!roundTrips(m, back));   // ... nor a prop velocity
    m.propVelocity = {0, 0, 0};                     CHECK(roundTrips(m, back));
    hsm::Writer w;
    PutClaim(w, ref);
    for (size_t n = 0; n < w.buf.size(); ++n) {   // every truncation fails
        hsm::Reader r(w.buf.data(), n);
        CHECK(!GetClaim(r, back));
    }
    std::vector<uint8_t> buf = w.buf;
    const uint64_t nanBits = 0x7FF8000000000000ull;
    std::memcpy(buf.data() + buf.size() - 33, &nanBits, 8);   // propVelocity.x (victimLife and rawDamage follow)
    hsm::Reader r(buf.data(), buf.size());
    CHECK(!GetClaim(r, back));
}

// ---------------------------------------------------------------- stuck-blade damage (rev 25, StrikerKind::Stuck)
static HitClaim StuckRef() {
    HitClaim c;
    c.claimId = 11; c.attacker = 1; c.victim = 2; c.attackerTsMs = 900; c.victimLife = 3;
    c.hitBone = "spine_03"; c.location = {1, 2, 3}; c.normal = {0, 0, 1};
    c.cuttingPower = 140; c.lowerThreshold = true;
    c.source = StrikeSource::WeaponRight; c.strikeComponent = "Blade";
    c.striker = StrikerKind::Stuck; c.rawDamage = 4200.5;
    return c;
}

static void TestStuckClaimDecode() {
    const HitClaim ref = StuckRef();
    auto roundTrips = [](const HitClaim& c, HitClaim& back) {
        hsm::Writer w;
        PutClaim(w, c);
        hsm::Reader r(w.buf.data(), w.buf.size());
        return GetClaim(r, back);
    };
    HitClaim back;
    CHECK(roundTrips(ref, back) && ClaimEq(ref, back) && back.rawDamage == 4200.5 && back.striker == StrikerKind::Stuck);
    HitClaim m = ref; m.source = StrikeSource::WeaponLeft;   CHECK(roundTrips(m, back) && back.source == StrikeSource::WeaponLeft);
    m = ref; m.rawDamage = kMaxStuckRawDamage;               CHECK(roundTrips(m, back));              // the edge of the range
    m = ref; m.cuttingPower = 0;                             CHECK(roundTrips(m, back));
    m = ref; m.cuttingPower = kMaxStuckCuttingPower;         CHECK(roundTrips(m, back));
    m = ref; m.hasHitBox = true; m.hitBoxExtent = {5, 5, 5}; m.hitBoxLocal.rot = {0, 0, 0, 1}; CHECK(roundTrips(m, back));   // Box, when stuck
    // Out of range or ill-formed: all rejected.
    m = ref; m.rawDamage = 0;                                CHECK(!roundTrips(m, back));             // nothing to apply
    m = ref; m.rawDamage = -1;                               CHECK(!roundTrips(m, back));
    m = ref; m.rawDamage = kMaxStuckRawDamage * 1.001;       CHECK(!roundTrips(m, back));
    m = ref; m.rawDamage = std::numeric_limits<double>::infinity();  CHECK(!roundTrips(m, back));
    m = ref; m.rawDamage = std::nan("");                     CHECK(!roundTrips(m, back));
    m = ref; m.cuttingPower = -0.5;                          CHECK(!roundTrips(m, back));
    m = ref; m.cuttingPower = kMaxStuckCuttingPower * 1.001; CHECK(!roundTrips(m, back));
    m = ref; m.source = StrikeSource::Body;                  CHECK(!roundTrips(m, back));             // a blade is in a hand
    m = ref; m.hitBone = "";                                 CHECK(!roundTrips(m, back));             // it names a bone
    m = ref; m.propId = hsm::MakeLevelPropId(14);            CHECK(!roundTrips(m, back));             // and no prop
    m = ref; m.propVelocity = {0, 0, 1};                     CHECK(!roundTrips(m, back));
    // Negative controls: a damage on any other kind of claim is garbage; so is a stuck kind with the byte flipped back.
    m = ref; m.striker = StrikerKind::Fighter;               CHECK(!roundTrips(m, back));
    m.rawDamage = 0;                                         CHECK(roundTrips(m, back));              // the plain claim it was
    m = PropRef(); m.rawDamage = 5;                          CHECK(!roundTrips(m, back));
    // Truncation and trailing bytes.
    hsm::Writer w;
    PutClaim(w, ref);
    for (size_t n = 0; n < w.buf.size(); ++n) {
        hsm::Reader r(w.buf.data(), n);
        CHECK(!GetClaim(r, back));
    }
    std::vector<uint8_t> buf = w.buf;
    const uint64_t nanBits = 0x7FF8000000000000ull;
    std::memcpy(buf.data() + buf.size() - 8, &nanBits, 8);   // rawDamage is the last field
    hsm::Reader r(buf.data(), buf.size());
    CHECK(!GetClaim(r, back));
    buf = w.buf;
    buf.push_back(0);
    hsm::Reader r2(buf.data(), buf.size());
    CHECK(!GetClaim(r2, back));
}

// Who decides a contact between a fighter and a prop: the one machine that streams the prop. A stand-in struck by a
// prop streamed here is a claim; one struck by a copy streamed elsewhere is that streamer's to claim. My fighter struck
// by a prop streamed here takes it natively; by a copy driven from elsewhere, not (the streamer's claim brings it).
static void TestDecidePropContact() {
    CHECK(DecidePropContact(false, true) == PropContact::Claim);
    CHECK(DecidePropContact(false, false) == PropContact::Block);
    CHECK(DecidePropContact(true, true) == PropContact::Native);
    CHECK(DecidePropContact(true, false) == PropContact::Block);
}

// A bite's (6,6,6) marker crosses unturned, so the victim's Get Damage still skips the consciousness loss (audit item 21).
static void TestBiteNormal() {
    Xform standIn; standIn.pos = {100, 200, 300}; standIn.rot = AxisAngleDeg(0, 0, 1, 37.0);
    Xform victimBone; victimBone.pos = {-40, 10, 90}; victimBone.rot = AxisAngleDeg(1, 0, 0, 80.0);
    DcdInputs in; in.hitBone = "neck_01"; in.location = {110, 205, 330}; in.normal = {6, 6, 6};
    HitClaim c = MakeClaim(in, standIn, 1, 10, 2, 1000, StrikeSource::Body, "", "");
    CHECK(IsBiteNormal(c.normal));
    CHECK(IsBiteNormal(ReplayInputs(c, victimBone).normal));
    in.normal = {0, 0, 1};
    CHECK(!IsBiteNormal(ReplayInputs(MakeClaim(in, standIn, 2, 10, 2, 1000, StrikeSource::Body, "", ""), victimBone).normal));
}

int main() {
    TestBoneFrameRoundTrip();
    TestBiteNormal();
    TestScaledBoneFrame();
    TestGateTruthTable();
    TestLimiter();
    TestDedupe();
    TestDedupeForget();
    TestClaimFresh();
    TestReplayBoneOk();
    TestParryWindow();
    TestParryWindowPerWeapon();
    TestParryWindowDelta();
    TestBodyPartOfBone();
    TestCoalescerNeckStorm();
    TestCoalescerWindow();
    TestCoalescerStronger();
    TestCoalescerKeys();
    TestCoalescerCap();
    TestCoalescerForget();
    TestStuckDamageLimiter();
    TestReplayQueue();
    TestSerializationRoundTrip();
    TestHostileDecode();
    TestPropStriker();
    TestPropClaimDecode();
    TestStuckClaimDecode();
    TestDecidePropContact();
    if (g_fail) { std::printf("test_hsm_claim: %d failure(s)\n", g_fail); return 1; }
    std::printf("test_hsm_claim: all passed\n");
    return 0;
}
