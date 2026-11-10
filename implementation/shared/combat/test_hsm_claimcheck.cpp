// Rev 42 lag-compensated claim check, the new claim wire fields, the adaptive parry window and Deflect.
// Run via implementation/shared/run_tests.sh test_hsm_claimcheck.
#include "combat/hsm_claimcheck.h"
#include "sync/hsm_remote.h"
#include <cmath>
#include <cstdio>

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++g_fail; } } while (0)
using namespace hsm;
using namespace hsm::combat;

// The victim walked along +x at 1 uu/ms: at its stream time t its head was at x = t - 1000.
static SelfPoseHistory Walk() {
    SelfPoseHistory h;
    for (uint32_t t = 1000; t <= 1600; t += 17) {
        sync::PoseFrame f; f.tsMs = t;
        for (int i = 0; i < sync::kPoseBones; ++i) f.bones[size_t(i)].pos = V3{double(t) - 1000.0, 0, 100.0 + i};
        h.Push(f);
    }
    return h;
}
static HitClaim Blow(uint32_t viewTs, double contactX) {
    HitClaim c; c.attacker = 2; c.victim = 1; c.hitBone = "head"; c.location = V3{0, 0, 5};
    c.victimViewTsMs = viewTs; c.worldLocation = V3{contactX, 0, 100.0 + sync::kBoneHead + 5};
    return c;
}

static void TestBoneNames() {
    CHECK(PoseBoneOf("pelvis") == 0 && PoseBoneOf("head") == sync::kBoneHead && PoseBoneOf("hand_r") == sync::kBoneHandR);
    CHECK(PoseBoneOf("index_02_l") == sync::kBoneHandL && PoseBoneOf("lowerarm_twist_01_r") == 15 && PoseBoneOf("ball_l") == sync::kBoneFootL);
    CHECK(PoseBoneOf("thigh_twist_01_r") == 20 && PoseBoneOf("weapon") == -1 && PoseBoneOf("") == -1);
}

static void TestHistory() {
    SelfPoseHistory h = Walk();
    Xform x;
    CHECK(h.BoneAt(1100, sync::kBoneHead, x) && std::fabs(x.pos.x - 100.0) < 1e-9);   // interpolated between frames
    CHECK(h.BoneAt(10, 0, x) && x.pos.x == 0);                                          // clamped to the oldest
    CHECK(!h.BoneAt(1100, 99, x));
    sync::PoseFrame old; old.tsMs = 900;
    h.Push(old);                                                                        // the clock never goes back
    CHECK(h.NewestTs() != 900);
}

static void TestCheck() {
    SelfPoseHistory h = Walk();
    const uint32_t now = h.NewestTs();   // 1595
    // The attacker saw the victim as it was at 1500 (x = 500) and struck it there: fair.
    ClaimCheckResult r = CheckClaim(h, Blow(1500, 500), now);
    CHECK(r.verdict == ClaimVerdict::Ok && r.distUu < 1e-6 && !r.capped && std::fabs(r.rewindMs - 95) < 1e-9);
    // Struck a stand-in 60 uu off where the victim really was: far.
    r = CheckClaim(h, Blow(1500, 560), now);
    CHECK(r.verdict == ClaimVerdict::Far && std::fabs(r.distUu - 60) < 1e-6);
    // A view 400 ms old is checked 200 ms back (x = 395): the old contact (x = 195) is far, so no hit after cover.
    r = CheckClaim(h, Blow(1195, 195), now);
    CHECK(r.capped && r.verdict == ClaimVerdict::Far && std::fabs(r.rewindMs - kClaimRewindCapMs) < 1e-9);
    // Nothing to check against.
    CHECK(CheckClaim(h, Blow(0, 0), now).verdict == ClaimVerdict::Unchecked);
    { HitClaim c = Blow(1500, 500); c.hitBone = "weapon_socket"; CHECK(CheckClaim(h, c, now).verdict == ClaimVerdict::Unchecked); }
    { HitClaim c = Blow(1500, 500); c.striker = StrikerKind::Stuck; CHECK(CheckClaim(h, c, now).verdict == ClaimVerdict::Unchecked); }
    CHECK(CheckClaim(SelfPoseHistory{}, Blow(1500, 500), now).verdict == ClaimVerdict::Unchecked);
    ClaimCheckMode m;
    CHECK(ParseClaimCheckMode("log", m) && m == ClaimCheckMode::Log && ParseClaimCheckMode("on", m) && m == ClaimCheckMode::On);
    CHECK(ParseClaimCheckMode("0", m) && m == ClaimCheckMode::Off && !ParseClaimCheckMode("maybe", m));
}

static void TestWire() {
    HitClaim c = Blow(1234, 77); c.claimId = 9; c.attackerTsMs = 5000; c.source = StrikeSource::WeaponRight;
    Writer w; PutClaim(w, c);
    Reader r(w.buf.data(), w.buf.size());
    HitClaim o;
    CHECK(GetClaim(r, o) && o.victimViewTsMs == 1234 && o.worldLocation.x == 77 && o.worldLocation.z == c.worldLocation.z);
}

static void TestParryWindowFor() {
    ParryWindowMs w = ParryWindowFor(30, 30, 4);   // short link: a short window
    CHECK(w.before == 80 && w.after == 19);
    w = ParryWindowFor(60, 150, 10);
    CHECK(w.before == 230 && w.after == 22);
    CHECK(ParryWindowFor(0, 0, 0).before == kParryBeforeMinMs && ParryWindowFor(500, 500, 0).before == kParryBeforeMaxMs);
    CHECK(ParryWindowFor(std::nan(""), 0, 0).before == kParryBeforeMs);
    // The window in force decides.
    ParryWindow pw;
    pw.NoteClash(2, StrikeSource::WeaponRight, 1000);
    HitClaim c; c.attacker = 2; c.source = StrikeSource::WeaponRight; c.attackerTsMs = 1120;
    CHECK(pw.Cancels(c));                          // the fixed 150 ms
    pw.SetWindow(2, ParryWindowFor(30, 30, 4));    // 80 ms
    CHECK(!pw.Cancels(c));
    c.attackerTsMs = 1070; CHECK(pw.Cancels(c));
    pw.Forget(2);
    CHECK(pw.WindowOf(2).before == kParryBeforeMs);
}

static void TestDeflect() {
    using namespace hsm::sync;
    DeflectGate g;
    RemoteWriteMsg m;
    CHECK(!g.Make(1, 2, 0, V3{100, 0, 0}, 1000, m));                     // too weak
    CHECK(g.Make(1, 2, 0, V3{3000, 0, 0}, 1000, m) && m.kind == RemoteKind::Deflect && m.bone == 0 && m.vec.x == 3000);
    CHECK(!g.Make(1, 2, 0, V3{3000, 0, 0}, 1050, m));                    // one knock per clash
    CHECK(g.Make(1, 2, 1, V3{3000, 0, 0}, 1050, m));                     // the other weapon
    CHECK(g.Make(1, 2, 0, V3{0, 0, 1e6}, 1200, m) && std::fabs(m.vec.z - kDeflectMaxImpulse) < 1e-6);   // capped
    CHECK(!g.Make(1, 1, 0, V3{3000, 0, 0}, 2000, m) && !g.Make(1, 2, 2, V3{3000, 0, 0}, 2000, m));
    RemoteWriteMsg o;
    auto b = Encode(m);
    CHECK(Decode(b.data(), b.size(), o) && o.kind == RemoteKind::Deflect);
    m.bone = kPoseWeapons; b = Encode(m); CHECK(!Decode(b.data(), b.size(), o));
    m.bone = 0; m.vec = V3{kDeflectMaxImpulse * 2, 0, 0}; b = Encode(m); CHECK(!Decode(b.data(), b.size(), o));
}

static void TestCopyFilter() {
    ClaimCopyFilter f;
    CHECK(f.First(2, 10, 1000) && !f.First(2, 10, 1050));   // the unreliable copy right behind the reliable one
    CHECK(f.First(3, 10, 1050));                             // another attacker's id 10 is its own
    CHECK(f.First(2, 11, 1100));
    CHECK(f.First(2, 10, 1000 + kCopyWindowMs + 1));         // long after: a new body's id 10
    f.Forget(2);
    CHECK(f.First(2, 11, 1200));
    for (uint32_t i = 0; i < kCopyKeep + 10; ++i) CHECK(f.First(4, 100 + i, 5000));   // bounded, never refuses new ids
}

int main() {
    TestCopyFilter();
    TestBoneNames();
    TestHistory();
    TestCheck();
    TestWire();
    TestParryWindowFor();
    TestDeflect();
    if (g_fail == 0) std::printf("test_hsm_claimcheck: all passed\n");
    return g_fail == 0 ? 0 : 1;
}
