// Unit tests for hsm_fx.h. Run via implementation/shared/run_tests.sh test_hsm_fx.
//
// Wire layout is written out independently below (Wire()), in the order hsm_fx.h documents:
//   header(PaintFx) entity:u8 kind:u8 seq:u16 bone:str bodyPart:u8 local:Xform extent:V3 rate:f64 damageRate:f64
//   loopCount:u8 extraVelocity:u8 velocity:V3 power:V3 bleeding:f64 gateLastRate:f64 gateSpRate:f64 gatePart:u8
#include "combat/hsm_fx.h"
#include <cmath>
#include <cstdio>
#include <limits>
#include <string>

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++g_fail; } } while (0)

using namespace hsm::combat;
using hsm::MsgType;

namespace {

bool Near(double a, double b, double eps = 1e-9) { return std::fabs(a - b) <= eps; }
bool NearV(const V3& a, const V3& b, double eps = 1e-9) { return Near(a.x, b.x, eps) && Near(a.y, b.y, eps) && Near(a.z, b.z, eps); }

PaintFxMsg Blood() {
    PaintFxMsg m;
    m.entity = 3; m.kind = FxKind::Blood; m.seq = 777; m.bone = "lowerarm_r"; m.bodyPart = 2;
    m.local.pos = {4, -2, 7.5};
    m.local.rot = {0, 0, std::sin(0.3), std::cos(0.3)};
    m.rate = 12.5; m.damageRate = 1; m.loopCount = 4; m.extraVelocity = true;
    m.velocity = {-30, 12, 400}; m.power = {9000, -100, 25}; m.bleeding = 37;
    m.gateLastRate = 4.25; m.gateSpRate = 2.5; m.gatePart = 6;
    return m;
}
PaintFxMsg Wound() {
    PaintFxMsg m;
    m.entity = 16; m.kind = FxKind::Wound; m.seq = 65535; m.bone = "head"; m.bodyPart = 0;
    m.local.pos = {1, 2, 3};
    m.extent = {3, 3, 3}; m.damageRate = 1.5; m.loopCount = 1;
    return m;
}

// The documented layout, written by hand: kindByte and extraByte override the enum and bool so raw bad values fit.
std::vector<uint8_t> Wire(const PaintFxMsg& m, uint8_t kindByte, uint8_t extraByte, MsgType type = MsgType::PaintFx) {
    hsm::Writer w;
    w.header(type);
    w.u8(m.entity); w.u8(kindByte); w.u16(m.seq); w.str(m.bone, 255); w.u8(m.bodyPart);
    PutXform(w, m.local); PutV3(w, m.extent); PutF64(w, m.rate); PutF64(w, m.damageRate);
    w.u8(m.loopCount); w.u8(extraByte); PutV3(w, m.velocity); PutV3(w, m.power); PutF64(w, m.bleeding);
    PutF64(w, m.gateLastRate); PutF64(w, m.gateSpRate); w.u8(m.gatePart);
    return w.buf;
}
std::vector<uint8_t> Wire(const PaintFxMsg& m) { return Wire(m, uint8_t(m.kind), m.extraVelocity ? 1 : 0); }

bool Dec(const std::vector<uint8_t>& b, PaintFxMsg& out) { return Decode(b.data(), b.size(), out); }
bool Dec(const std::vector<uint8_t>& b) { PaintFxMsg o; return Dec(b, o); }

void TestRoundTrip() {
    for (const PaintFxMsg& m : {Blood(), Wound()}) {
        std::vector<uint8_t> b = Encode(m);
        CHECK(b == Wire(m));   // Encode writes the documented layout
        PaintFxMsg o;
        CHECK(Dec(b, o));
        CHECK(o.entity == m.entity && o.kind == m.kind && o.seq == m.seq && o.bone == m.bone && o.bodyPart == m.bodyPart);
        CHECK(NearV(o.local.pos, m.local.pos) && Near(o.local.rot.z, m.local.rot.z) && Near(o.local.rot.w, m.local.rot.w));
        CHECK(NearV(o.local.scale, m.local.scale) && NearV(o.extent, m.extent));
        CHECK(o.rate == m.rate && o.damageRate == m.damageRate && o.loopCount == m.loopCount);
        CHECK(o.extraVelocity == m.extraVelocity && NearV(o.velocity, m.velocity) && NearV(o.power, m.power));
        CHECK(o.bleeding == m.bleeding);
        CHECK(o.gateLastRate == m.gateLastRate && o.gateSpRate == m.gateSpRate && o.gatePart == m.gatePart);
    }
}

void TestFraming() {
    std::vector<uint8_t> b = Encode(Blood());
    CHECK(!Dec(Wire(Blood(), 1, 1, MsgType::StuckBlade)));   // same body, another type: negative control
    std::vector<uint8_t> shortB(b.begin(), b.end() - 1);
    CHECK(!Dec(shortB));
    std::vector<uint8_t> longB = b; longB.push_back(0);
    CHECK(!Dec(longB));
    PaintFxMsg o;
    CHECK(!Decode(b.data(), 0, o));
}

void TestRanges() {
    { PaintFxMsg m = Blood(); m.entity = 0; CHECK(!Dec(Wire(m))); }
    { PaintFxMsg m = Blood(); m.entity = 17; CHECK(!Dec(Wire(m))); }
    { PaintFxMsg m = Blood(); m.entity = 1; CHECK(Dec(Wire(m))); }
    CHECK(!Dec(Wire(Blood(), 0, 1)));
    CHECK(!Dec(Wire(Blood(), 7, 1)));                          // rev 15: kinds 1..6 (3 Voice, 4 HitSound, 5 Armor, 6 ArmorOn)
    CHECK(!Dec(Wire(Blood(), 1, 2)));                          // extraVelocity is 0 or 1
    { PaintFxMsg m = Blood(); m.bone = ""; CHECK(!Dec(Wire(m))); }
    { PaintFxMsg m = Blood(); m.bone = std::string(64, 'b'); CHECK(Dec(Wire(m))); }
    { PaintFxMsg m = Blood(); m.bone = std::string(65, 'b'); CHECK(!Dec(Wire(m))); }
    { PaintFxMsg m = Blood(); m.bodyPart = 7; CHECK(Dec(Wire(m))); }
    { PaintFxMsg m = Blood(); m.bodyPart = 8; CHECK(!Dec(Wire(m))); }
    { PaintFxMsg m = Blood(); m.local.rot = {0, 0, 0, 2}; CHECK(!Dec(Wire(m))); }
    { PaintFxMsg m = Blood(); m.local.scale = {1, 0, 1}; CHECK(!Dec(Wire(m))); }
    { PaintFxMsg m = Blood(); m.local.pos = {kFxMaxOffset + 1, 0, 0}; CHECK(!Dec(Wire(m))); }
    { PaintFxMsg m = Blood(); m.local.pos = {kFxMaxOffset, 0, 0}; CHECK(Dec(Wire(m))); }
    { PaintFxMsg m = Blood(); m.extent = {-1, 0, 0}; CHECK(!Dec(Wire(m))); }
    { PaintFxMsg m = Wound(); m.extent = {kFxMaxExtent, kFxMaxExtent, kFxMaxExtent}; CHECK(Dec(Wire(m))); }
    { PaintFxMsg m = Wound(); m.extent = {3, kFxMaxExtent + 1, 3}; CHECK(!Dec(Wire(m))); }
    { PaintFxMsg m = Wound(); m.extent = {3, 3, 0}; CHECK(!Dec(Wire(m))); }   // a wound box has a volume
    { PaintFxMsg m = Blood(); m.rate = -0.1; CHECK(!Dec(Wire(m))); }
    { PaintFxMsg m = Blood(); m.rate = kFxMaxRate + 1; CHECK(!Dec(Wire(m))); }
    { PaintFxMsg m = Blood(); m.rate = kFxMaxRate; CHECK(Dec(Wire(m))); }
    { PaintFxMsg m = Wound(); m.damageRate = -1; CHECK(!Dec(Wire(m))); }
    { PaintFxMsg m = Wound(); m.damageRate = kFxMaxRate + 1; CHECK(!Dec(Wire(m))); }
    { PaintFxMsg m = Blood(); m.loopCount = 0; CHECK(!Dec(Wire(m))); }
    { PaintFxMsg m = Blood(); m.loopCount = kFxMaxLoops; CHECK(Dec(Wire(m))); }
    { PaintFxMsg m = Blood(); m.loopCount = kFxMaxLoops + 1; CHECK(!Dec(Wire(m))); }
    { PaintFxMsg m = Blood(); m.velocity = {0, kFxMaxVector + 1, 0}; CHECK(!Dec(Wire(m))); }
    { PaintFxMsg m = Blood(); m.power = {0, 0, -kFxMaxVector - 1}; CHECK(!Dec(Wire(m))); }
    { PaintFxMsg m = Blood(); m.power = {0, 0, -kFxMaxVector}; CHECK(Dec(Wire(m))); }
    { PaintFxMsg m = Blood(); m.velocity.x = std::numeric_limits<double>::quiet_NaN(); CHECK(!Dec(Wire(m))); }
    { PaintFxMsg m = Blood(); m.bleeding = -1; CHECK(!Dec(Wire(m))); }
    { PaintFxMsg m = Blood(); m.bleeding = kFxMaxBleeding + 1; CHECK(!Dec(Wire(m))); }
    { PaintFxMsg m = Blood(); m.gateLastRate = -1; CHECK(!Dec(Wire(m))); }
    { PaintFxMsg m = Blood(); m.gateLastRate = kFxMaxRate + 1; CHECK(!Dec(Wire(m))); }
    { PaintFxMsg m = Blood(); m.gateSpRate = -1; CHECK(!Dec(Wire(m))); }
    { PaintFxMsg m = Blood(); m.gateSpRate = kFxMaxRate + 1; CHECK(!Dec(Wire(m))); }
    { PaintFxMsg m = Blood(); m.gatePart = 7; CHECK(Dec(Wire(m))); }
    { PaintFxMsg m = Blood(); m.gatePart = 8; CHECK(!Dec(Wire(m))); }
    // A rejected decode leaves the output untouched.
    PaintFxMsg out = Wound();
    PaintFxMsg bad = Blood(); bad.bodyPart = 9;
    CHECK(!Dec(Wire(bad), out));
    CHECK(out.kind == FxKind::Wound && out.bone == "head");
}

void TestBoneFrame() {
    Xform bone;
    bone.pos = {100, -50, 30};
    bone.rot = {0, std::sin(0.4), 0, std::cos(0.4)};
    Xform world;
    world.pos = {120, -40, 25};
    world.rot = {std::sin(0.2), 0, 0, std::cos(0.2)};
    V3 dir{0, 300, -20};
    Xform local = FxToBone(bone, world);
    Xform back = FxFromBone(bone, local);
    CHECK(NearV(back.pos, world.pos, 1e-9));
    CHECK(Near(back.rot.x, world.rot.x) && Near(back.rot.y, world.rot.y) && Near(back.rot.z, world.rot.z) && Near(back.rot.w, world.rot.w));
    CHECK(NearV(bone.FromLocalDir(bone.ToLocalDir(dir)), dir));
    // On a moved and turned bone, the local spot follows the bone.
    Xform moved = bone;
    moved.pos = {0, 0, 0};
    moved.rot = {0, 0, std::sin(0.8), std::cos(0.8)};
    Xform there = FxFromBone(moved, local);
    CHECK(NearV(there.pos, moved.FromLocal(local.pos)));
    CHECK(!NearV(there.pos, world.pos, 1.0));
}

void TestWoundRadius() {
    CHECK(Near(WoundRadius(0, 0), 3));
    CHECK(Near(WoundRadius(100, 100), 3));       // (200)*0.005 = 1 -> 3
    CHECK(Near(WoundRadius(1000, 0), 5));
    CHECK(Near(WoundRadius(600, 400), 5));
    CHECK(Near(WoundRadius(5000, 0), 10));       // 25 -> 10
    CHECK(Near(WoundRadius(1900, 100), 10));
}

// Get Damage passing a heavy blow on to the parent bone calls itself with Location (0,0,0) and no Hit Box: no wound.
void TestPassedOnDamage() {
    CHECK(IsPassedOnDamage(V3{0, 0, 0}, false));
    CHECK(!IsPassedOnDamage(V3{0, 0, 0}, true));            // a Hit Box paints the box, wherever Location is
    CHECK(!IsPassedOnDamage(V3{0, 0, 1e-3}, false));        // a real hit near the world origin is still a wound
    CHECK(!IsPassedOnDamage(V3{-120, 40, 95}, false));
    CHECK(!IsPassedOnDamage(V3{std::numeric_limits<double>::quiet_NaN(), 0, 0}, false));
}

void TestLimiter() {
    FxLimiter l;
    int allowed = 0;
    for (int i = 0; i < kFxMaxPerSecond + 5; ++i) allowed += l.Allow(3, FxKind::Blood, 1000.0 + i) ? 1 : 0;
    CHECK(allowed == kFxMaxPerSecond);
    CHECK(l.Allow(3, FxKind::Wound, 1010));          // kinds are counted apart
    CHECK(l.Allow(4, FxKind::Blood, 1010));          // and entities
    CHECK(!l.Allow(3, FxKind::Blood, 1999));
    CHECK(l.Allow(3, FxKind::Blood, 2000));           // a new one-second window
    l.Forget(3);
    for (int i = 0; i < kFxMaxPerSecond - 1; ++i) CHECK(l.Allow(3, FxKind::Blood, 2001));
    CHECK(l.Allow(3, FxKind::Blood, 2001));
    CHECK(!l.Allow(3, FxKind::Blood, 2001));
    FxLimiter m;
    for (int i = 0; i < kFxMaxPerSecond; ++i) m.Allow(5, FxKind::Wound, 0);
    m.Forget(6);
    CHECK(!m.Allow(5, FxKind::Wound, 1));            // Forget of another entity keeps 5's count
    CHECK(!m.Allow(0, FxKind::Blood, 0));            // entity 0 is never a fighter
}

PaintFxMsg Voice() {
    PaintFxMsg m;
    m.entity = 2; m.kind = FxKind::Voice; m.seq = 9; m.bone = "Pain_Big_Cue"; m.rate = 0.75;   // bone: the cue
    return m;
}
PaintFxMsg HitSound() {
    PaintFxMsg m;
    m.entity = 2; m.kind = FxKind::HitSound; m.seq = 10; m.bone = "spine_03"; m.local.pos = {3, 1, -2};
    m.extraVelocity = true; m.velocity = {1200, 2500, 0};   // |Impulse|, Damage Raw Scaled; extraVelocity: Lower Threshold
    return m;
}
PaintFxMsg Armor() {
    PaintFxMsg m;
    m.entity = 5; m.kind = FxKind::Armor; m.seq = 11; m.bone = "pelvis"; m.local.pos = {150, 20, 0}; m.rate = 7;
    return m;
}

PaintFxMsg ArmorOn() {
    PaintFxMsg m;
    m.entity = 5; m.kind = FxKind::ArmorOn; m.seq = 12; m.bone = "BP_Armor_Helmet_C"; m.local.pos = {40, -10, 60};   // bone: the class
    return m;
}
PaintFxMsg StripRequest() {
    PaintFxMsg m = Armor();
    m.extraVelocity = true;   // sent by the one who stripped the fallen fighter, to its owner
    return m;
}

// Rev 15: a fighter's voice lines, its blunt hit and bone sounds, and an armour piece it took off travel as PaintFx too.
void TestSoundAndArmorKinds() {
    for (const PaintFxMsg& m : {Voice(), HitSound(), Armor(), ArmorOn(), StripRequest()}) {
        PaintFxMsg o;
        CHECK(Dec(Encode(m), o));
        CHECK(o.kind == m.kind && o.entity == m.entity && o.bone == m.bone && Near(o.rate, m.rate));
        CHECK(NearV(o.velocity, m.velocity) && o.extraVelocity == m.extraVelocity && NearV(o.local.pos, m.local.pos));
        CHECK(Dec(Wire(m)));
    }
    CHECK(hsm::kProtocolRevision >= 15);                                        // the kinds above came with rev 15
    { PaintFxMsg m = Voice(); m.rate = kFxMaxRate + 1; CHECK(!Dec(Wire(m))); }
    { PaintFxMsg m = Armor(); m.rate = kFxArmorSlots; CHECK(!Dec(Wire(m))); }      // a slot is a byte enum below 64
    { PaintFxMsg m = Armor(); m.rate = kFxArmorSlots - 1; CHECK(Dec(Wire(m))); }
    { PaintFxMsg m = Armor(); m.rate = 2.5; CHECK(!Dec(Wire(m))); }                // and whole
    { PaintFxMsg m = Armor(); m.rate = -1; CHECK(!Dec(Wire(m))); }
    CHECK(IsStripRequest(StripRequest()) && !IsStripRequest(Armor()) && !IsStripRequest(HitSound()));
    { PaintFxMsg m = HitSound(); m.velocity.x = -1; CHECK(!Dec(Wire(m))); }        // a length and a damage: not negative
    { PaintFxMsg m = HitSound(); m.velocity.y = -1; CHECK(!Dec(Wire(m))); }
}

// Get Damage's sounds (Willie_BP.cpp:4071, 4379-4385), none when the blow went Inside:
//   blunt  volume clamp(|Impulse| * 0.0005 * (Lower Threshold ? 3 : 1), 0, 1), played when > 0.1; the cue by Damage
//          Raw Scaled: < 1000 light, < 3000 wet hard, else blunt hard;
//   bone   volume clamp(Damage Raw Scaled * 0.0008 * (Lower Threshold ? 3 : 1), 0, 1), only from 1000 damage up.
// (Its slash cue is played at |Impulse| * 0 * Cutting Power: never above the 0.1 gate, so it is not mirrored.)
void TestHitSoundPlan() {
    HitSoundPlan p = PlanHitSound(1000, 500, false, false);
    CHECK(p.blunt == BluntCue::Light && Near(p.bluntVolume, 0.5) && Near(p.boneVolume, 0));
    p = PlanHitSound(1000, 500, true, false);
    CHECK(p.blunt == BluntCue::Light && Near(p.bluntVolume, 1.0));
    p = PlanHitSound(300, 1000, true, false);
    CHECK(Near(p.bluntVolume, 0.45) && Near(p.boneVolume, 1.0));   // Lower Threshold triples both (the blunt one below its clamp here)
    p = PlanHitSound(300, 400, true, false);
    CHECK(Near(p.boneVolume, 0));
    p = PlanHitSound(200, 500, false, false);
    CHECK(p.blunt == BluntCue::None && Near(p.bluntVolume, 0));     // 0.1 is not above the gate
    p = PlanHitSound(201, 500, false, false);
    CHECK(p.blunt == BluntCue::Light);
    p = PlanHitSound(1000, 1000, false, false);
    CHECK(p.blunt == BluntCue::WetHard && Near(p.boneVolume, 0.8));
    p = PlanHitSound(1000, 999.9, false, false);
    CHECK(p.blunt == BluntCue::Light && Near(p.boneVolume, 0));
    p = PlanHitSound(1000, 3000, false, false);
    CHECK(p.blunt == BluntCue::BluntHard && Near(p.boneVolume, 1.0));
    p = PlanHitSound(1000, 1000, true, false);
    CHECK(Near(p.boneVolume, 1.0));
    p = PlanHitSound(5000, 5000, false, true);
    CHECK(p.blunt == BluntCue::None && Near(p.bluntVolume, 0) && Near(p.boneVolume, 0));   // Inside: no sound
    p = PlanHitSound(std::numeric_limits<double>::quiet_NaN(), 5000, false, false);
    CHECK(p.blunt == BluntCue::None);
}

// M1/M2: only the stock naming of a cue or an armour class may travel; a lookup by name is a scan of every loaded object.
void TestNameAllowlists() {
    for (const char* ok : {"Pain_Cue", "Pain_Big_Cue", "ScreamLongPain_Cue", "Englishman001_De_Cue", "Cue", "Heavy_exertion_cue", "Pain_Cue_1", "Pain_Cue_23"})
        CHECK(ValidVoiceCueName(ok));
    for (const char* bad : {"", "Pain", "Cue1", "Pain_Cue_", "Pain_Cue_x", "Pain-Cue", "Pain Cue", "Pain.Cue", "Pain_Cue\n", "Pa\xC3\xAFn_Cue", "/Game/Sounds/Pain_Cue",
                            "BP_Armor_Head_C", "pelvis"})
        CHECK(!ValidVoiceCueName(bad));
    CHECK(ValidVoiceCueName(std::string(61, 'a') + "Cue") && !ValidVoiceCueName(std::string(62, 'a') + "Cue"));   // 64 bytes at most
    CHECK(!ValidVoiceCueName(std::string("Pain_Cue\0x", 10)));                                                    // an embedded NUL
    for (const char* ok : {"BP_Armor_Head_Armet_001_C", "BP_Armor_Body_Doublet_C", "BP_Armor_Modular_Core_Arms_Harness_001_C", "BP_ArmorActor_C"})
        CHECK(ValidArmorClassName(ok));
    for (const char* bad : {"", "BP_Armor", "BP_Armor_C", "BP_Armor_Head", "Armor_Head_C", "BP_Weapon_C", "ModularWeaponBP_C", "bp_armor_head_c", "BP_Armor_Head-C",
                            "BP_Armor_Head C", "BP_Armor_\xC3\xA9_C", "Actor", "BP_Armor_Head_C\n"})
        CHECK(!ValidArmorClassName(bad));
    CHECK(ValidArmorClassName("BP_Armor_" + std::string(53, 'a') + "_C") && !ValidArmorClassName("BP_Armor_" + std::string(54, 'a') + "_C"));
    // Decode refuses them, so the host neither applies nor relays one.
    { PaintFxMsg m = Voice(); m.bone = "Pain"; CHECK(!Dec(Wire(m))); }
    { PaintFxMsg m = Voice(); m.bone = "SomeAsset"; CHECK(!Dec(Wire(m))); }
    { PaintFxMsg m = Voice(); CHECK(Dec(Wire(m))); }
    { PaintFxMsg m = ArmorOn(); m.bone = "ModularWeaponBP_C"; CHECK(!Dec(Wire(m))); }
    { PaintFxMsg m = ArmorOn(); CHECK(Dec(Wire(m))); }
    { PaintFxMsg m = Armor(); m.bone = "not a cue"; CHECK(Dec(Wire(m))); }   // other kinds name a bone, checked at the MakeName call
}

// M1: failures are remembered (no second scan), the OLDEST entry goes at capacity, a miss expires, a hit does not.
void TestNameCache() {
    NameCache<int> c(3, 1000.0);
    CHECK(c.Find("a", 0) == nullptr);
    c.PutMiss("zzz_Cue", 0);
    const auto* e = c.Find("zzz_Cue", 500);
    CHECK(e && !e->found);                                   // remembered as unresolvable
    CHECK(c.Find("zzz_Cue", 999) != nullptr);
    CHECK(c.Find("zzz_Cue", 1000) == nullptr && c.size() == 0);   // expired: looked up again, once
    c.Put("hit_Cue", 7, true, 0);
    e = c.Find("hit_Cue", 1e9);
    CHECK(e && e->found && e->value == 7);                   // a hit never expires
    // Eviction order is insertion order, not alphabetical: "zzz" is oldest, "aaa" sorts first.
    NameCache<int> d(3);
    d.Put("zzz", 1, true, 0); d.Put("mmm", 2, true, 0); d.Put("aaa", 3, true, 0);
    d.Put("bbb", 4, true, 0);
    CHECK(d.size() == 3 && d.Find("zzz", 0) == nullptr);
    CHECK(d.Find("aaa", 0) && d.Find("mmm", 0) && d.Find("bbb", 0));
    // Re-putting a key makes it the newest.
    d.Put("mmm", 9, true, 0);
    d.Put("ccc", 5, true, 0);
    CHECK(d.Find("mmm", 0) && d.Find("mmm", 0)->value == 9 && d.Find("aaa", 0) == nullptr && d.size() == 3);
    // A flood of distinct names never grows past the cap.
    NameCache<int> f(256);
    for (int i = 0; i < 100000; ++i) f.PutMiss("n" + std::to_string(i) + "_Cue", double(i));
    CHECK(f.size() == 256 && f.Find("n99999_Cue", 99999) && f.Find("n0_Cue", 99999) == nullptr);
    f.Clear();
    CHECK(f.size() == 0);
}

// M1: the receive-side limiter takes a per-second count, per (fighter, kind).
void TestRxLimiter() {
    FxLimiter rx(kFxMaxRxPerSecond);
    int ok = 0;
    for (int i = 0; i < 1000; ++i) ok += rx.Allow(2, FxKind::Voice, 100.0 + i * 0.1);
    CHECK(ok == kFxMaxRxPerSecond);
    CHECK(rx.Allow(2, FxKind::Blood, 100.0) && rx.Allow(3, FxKind::Voice, 100.0));      // other kinds and fighters have their own
    CHECK(rx.Allow(2, FxKind::Voice, 1100.0));                                          // the next window
    FxLimiter tx;                                                                        // the sender's default is unchanged
    ok = 0;
    for (int i = 0; i < 100; ++i) ok += tx.Allow(1, FxKind::Blood, 0.0);
    CHECK(ok == kFxMaxPerSecond && kFxMaxRxPerSecond >= 2 * kFxMaxPerSecond);
}

}  // namespace

int main() {
    TestSoundAndArmorKinds();
    TestNameAllowlists();
    TestNameCache();
    TestRxLimiter();
    TestHitSoundPlan();
    TestRoundTrip();
    TestFraming();
    TestRanges();
    TestBoneFrame();
    TestWoundRadius();
    TestPassedOnDamage();
    TestLimiter();
    if (g_fail) { std::printf("test_hsm_fx: %d failure(s)\n", g_fail); return 1; }
    std::printf("test_hsm_fx: ok\n");
    return 0;
}
