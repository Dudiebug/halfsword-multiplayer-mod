// Deal Complex Damage (DCD) claims: the bone-local capture of an attacker's "Deal Complex Damage"
// call, its wire encoding, and the gating/dedupe/parry/age rules that decide which calls become
// claims and which claims are ever replayed on the victim. Pure types, no engine headers; World
// space is Unreal units (uu, cm) and degrees, matching sync/hsm_pose.h.
//
// Claim fields and the bone-frame rule follow Cyrex0/HalfSword-MP
// docs/development/subsystems/combat.md, Copyright (c) 2026 Cyrex0 and the HalfSword-MP
// contributors, MIT (see THIRD_PARTY_NOTICES.md).
#pragma once
#include "hsm_protocol.h"
#include "session/hsm_lease.h"
#include "sync/hsm_pose.h"
#include "util/hsm_names.h"
#include <bitset>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <deque>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace hsm::combat {

using hsm::sync::Q4;
using hsm::sync::V3;

constexpr size_t kMaxClaimString = 64;          // hitBone / strikeComponent / strikeBone
constexpr int kClaimLimiterMaxPerBoneTick = 4;  // BF.MAX_PER_BONE_TICK
constexpr int64_t kClaimMaxAgeMs = 1000;        // claims older than this (receiver estimate) are dropped
constexpr int32_t kParryBeforeMs = 150;         // clash window: this far before the claim's attacker ts
constexpr int32_t kParryAfterMs = 17;           // clash window: this far after the claim's attacker ts
constexpr size_t kClaimDedupeWindow = 512;      // per-attacker anti-replay bitmap width

// ---------------------------------------------------------------- vector / quaternion helpers
// Q4 is a unit quaternion (FQuat convention, x,y,z,w); callers are expected to pass normalised
// rotations, as the rest of the sync layer does. These are free functions (not operators) to keep
// V3/Q4 plain aggregates, matching hsm_pose.h.
inline V3 Add(const V3& a, const V3& b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
inline V3 Sub(const V3& a, const V3& b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
inline V3 Cross(const V3& a, const V3& b) {
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}
inline Q4 Conjugate(const Q4& q) { return {-q.x, -q.y, -q.z, q.w}; }
// Hamilton product a*b: rotating by (a*b) equals rotating by b, then by a.
inline Q4 QuatMul(const Q4& a, const Q4& b) {
    return {
        a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
        a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
        a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w,
        a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z,
    };
}
// v + 2w(u x v) + 2u x (u x v), the standard q*v*q^-1 sandwich for a unit quaternion q=(w,u).
// Rotation only: preserves |v| exactly (up to floating-point error) for any input length.
inline V3 RotateVector(const Q4& q, const V3& v) {
    V3 u{q.x, q.y, q.z};
    V3 uv = Cross(u, v);
    V3 t{uv.x * 2.0, uv.y * 2.0, uv.z * 2.0};
    V3 uut = Cross(u, t);
    return {v.x + q.w * t.x + uut.x, v.y + q.w * t.y + uut.y, v.z + q.w * t.z + uut.z};
}
// 1/s, or 0 for a near-zero scale component (mirrors UE FTransform's safe scale reciprocal so a
// degenerate bone scale never produces inf/NaN).
inline double SafeRecip(double s) { return std::fabs(s) > 1e-8 ? 1.0 / s : 0.0; }

// World <-> bone-local transform, matching UE FTransform semantics: ToLocal/FromLocal are the
// full point transform (translate + rotate + scale, scale divided out going to local, exactly
// InverseTransformPosition/TransformPosition); ToLocalDir/FromLocalDir are rotation-only (no
// translation, no scale) for normals/velocities/impulses, so their length is unchanged. This is
// the bone-frame rule HSMP uses for Deal Complex Damage claims: "rotation removed, offset divided
// by the bone scale" for the point, rotation-only for directions.
struct Xform {
    V3 pos;
    Q4 rot;
    V3 scale{1, 1, 1};

    V3 ToLocal(const V3& point) const {
        V3 rel = Sub(point, pos);
        V3 unrotated = RotateVector(Conjugate(rot), rel);
        return {unrotated.x * SafeRecip(scale.x), unrotated.y * SafeRecip(scale.y), unrotated.z * SafeRecip(scale.z)};
    }
    V3 FromLocal(const V3& point) const {
        V3 scaled{point.x * scale.x, point.y * scale.y, point.z * scale.z};
        return Add(RotateVector(rot, scaled), pos);
    }
    V3 ToLocalDir(const V3& dir) const { return RotateVector(Conjugate(rot), dir); }
    V3 FromLocalDir(const V3& dir) const { return RotateVector(rot, dir); }
};

// What struck: HSMP flag bit 7 (BF.WEAPON) plus the hand that owns the history/replay component.
enum class StrikeSource : uint8_t { Body = 0, WeaponRight = 1, WeaponLeft = 2 };
// Whose: the attacker fighter's weapon or body, or a prop (a rock, a thrown weapon, debris) streamed by the claiming
// machine, whose contact it decided for the attacker (DecidePropContact).
enum class StrikerKind : uint8_t { Fighter = 0, Prop = 1 };

// A captured "Deal Complex Damage" call, in the hit bone's local frame (see Xform above).
struct HitClaim {
    uint32_t claimId = 0;
    uint8_t attacker = 0, victim = 0;
    uint32_t attackerTsMs = 0;          // attacker's clock, the pose-clock timestamp of the call
    std::string hitBone;                // FName of the Hit Component's bone
    V3 location, normal, hitVelocity, hitImpulse;   // all in the hit bone's local frame
    double cuttingPower = 0, stabRate = 0, rigidity = 0, kickPower = 0, drawCut = 0;
    int32_t bluntDestruction = 0;
    bool lowerThreshold = false, damageParentBone = false, extraHighVelocity = false;
    StrikeSource source = StrikeSource::Body;
    std::string strikeComponent;        // component name on the attacker's weapon or body
    std::string strikeBone;             // for body strikes: the attacker bone; may be empty
    bool hasHitBox = false;
    Xform hitBoxLocal;                  // Hit Box's world transform, expressed in the hit bone frame
    V3 hitBoxExtent;                    // Hit Box half-extents; only meaningful when hasHitBox
    StrikerKind striker = StrikerKind::Fighter;
    uint64_t propId = 0;                // Prop: the prop (session/hsm_lease.h ids); 0 for a fighter
    V3 propVelocity;                    // Prop: its velocity at the contact, in the hit bone's frame; zero for a fighter
};

// The world-space "Deal Complex Damage" call inputs, same fields as the decompiled signature
// minus the two component pointers (Hit Component, Collided Component), which never cross the
// wire: the victim replays against its own pawn/bone, and "what struck" travels as HitClaim's
// source/strikeComponent/strikeBone instead of a pointer.
struct DcdInputs {
    std::string hitBone;
    V3 location, normal, hitVelocity, hitImpulse;   // world space
    double cuttingPower = 0, stabRate = 0, rigidity = 0;
    int32_t bluntDestruction = 0;
    bool lowerThreshold = false, damageParentBone = false;
    double kickPower = 0;
    bool hasHitBox = false;
    Xform hitBoxWorld;                  // Hit Box's world transform, if present
    V3 hitBoxExtent;
    bool extraHighVelocity = false;
    double drawCut = 0;
};

// Captures a DCD call made on hitBoneWorld (the stand-in's bone pose on the attacker's screen) as
// a claim. ids/meta (claimId, attacker, victim, attackerTsMs, source, strikeComponent, strikeBone)
// are not part of the DCD call itself (they come from the claim pipeline / the component pointers
// DcdInputs omits), so they are supplied separately.
inline HitClaim MakeClaim(const DcdInputs& in, const Xform& hitBoneWorld, uint32_t claimId, uint8_t attacker,
                           uint8_t victim, uint32_t attackerTsMs, StrikeSource source,
                           const std::string& strikeComponent, const std::string& strikeBone) {
    HitClaim c;
    c.claimId = claimId;
    c.attacker = attacker;
    c.victim = victim;
    c.attackerTsMs = attackerTsMs;
    c.hitBone = in.hitBone;
    c.location = hitBoneWorld.ToLocal(in.location);
    c.normal = hitBoneWorld.ToLocalDir(in.normal);
    c.hitVelocity = hitBoneWorld.ToLocalDir(in.hitVelocity);
    c.hitImpulse = hitBoneWorld.ToLocalDir(in.hitImpulse);
    c.cuttingPower = in.cuttingPower;
    c.stabRate = in.stabRate;
    c.rigidity = in.rigidity;
    c.kickPower = in.kickPower;
    c.drawCut = in.drawCut;
    c.bluntDestruction = in.bluntDestruction;
    c.lowerThreshold = in.lowerThreshold;
    c.damageParentBone = in.damageParentBone;
    c.extraHighVelocity = in.extraHighVelocity;
    c.source = source;
    c.strikeComponent = strikeComponent;
    c.strikeBone = strikeBone;
    c.hasHitBox = in.hasHitBox;
    if (in.hasHitBox) {
        c.hitBoxLocal.pos = hitBoneWorld.ToLocal(in.hitBoxWorld.pos);
        c.hitBoxLocal.rot = combat::QuatMul(Conjugate(hitBoneWorld.rot), in.hitBoxWorld.rot);   // qualified: ADL also finds sync::QuatMul
        c.hitBoxLocal.scale = in.hitBoxWorld.scale;   // scale does not compose through rotation; carried as-is
        c.hitBoxExtent = in.hitBoxExtent;
    }
    return c;
}

// Turns a claim back into world-space DCD call inputs against victimBoneWorld (the victim's own
// bone pose). This is the world<->bone-local round trip: MakeClaim then ReplayInputs with the
// same Xform recovers the original world inputs; with a different Xform, the bone-local point
// lands on the same spot of the (possibly turned/moved/leaning/rescaled) victim bone instead.
inline DcdInputs ReplayInputs(const HitClaim& c, const Xform& victimBoneWorld) {
    DcdInputs out;
    out.hitBone = c.hitBone;
    out.location = victimBoneWorld.FromLocal(c.location);
    out.normal = victimBoneWorld.FromLocalDir(c.normal);
    out.hitVelocity = victimBoneWorld.FromLocalDir(c.hitVelocity);
    out.hitImpulse = victimBoneWorld.FromLocalDir(c.hitImpulse);
    out.cuttingPower = c.cuttingPower;
    out.stabRate = c.stabRate;
    out.rigidity = c.rigidity;
    out.kickPower = c.kickPower;
    out.drawCut = c.drawCut;
    out.bluntDestruction = c.bluntDestruction;
    out.lowerThreshold = c.lowerThreshold;
    out.damageParentBone = c.damageParentBone;
    out.extraHighVelocity = c.extraHighVelocity;
    out.hasHitBox = c.hasHitBox;
    if (c.hasHitBox) {
        out.hitBoxWorld.pos = victimBoneWorld.FromLocal(c.hitBoxLocal.pos);
        out.hitBoxWorld.rot = combat::QuatMul(victimBoneWorld.rot, c.hitBoxLocal.rot);
        out.hitBoxWorld.scale = c.hitBoxLocal.scale;
        out.hitBoxExtent = c.hitBoxExtent;
    }
    return out;
}

// ---------------------------------------------------------------- props (test 5: rocks did no damage)
// A contact between a fighter and a prop is decided on the one machine that streams the prop (its lessee, or the host
// while it is FREE): there the prop is simulated and the contact carries a real impulse; everywhere else the prop is a
// copy teleported to that stream, whose contact does not. A stand-in struck by a prop streamed here becomes a claim; a
// stand-in struck by a copy driven from elsewhere is that streamer's to claim; my fighter struck by a prop streamed
// here takes it natively, and struck by a driven copy not at all (the streamer's claim against its stand-in of my
// fighter brings the blow).
enum class PropContact : uint8_t { Native, Claim, Block };
inline PropContact DecidePropContact(bool victimOwnedHere, bool propStreamedHere) {
    if (victimOwnedHere) return propStreamedHere ? PropContact::Native : PropContact::Block;
    return propStreamedHere ? PropContact::Claim : PropContact::Block;
}

// Marks a claim (MakeClaim's) as struck by a prop: its id and its world velocity at the contact, taken into the hit
// bone's frame like every other direction (rotation only). A body blow: no hand holds it and no clash parries it.
inline void StruckByProp(HitClaim& c, uint64_t propId, const V3& velocityWorld, const Xform& hitBoneWorld) {
    c.striker = StrikerKind::Prop;
    c.propId = propId;
    c.propVelocity = hitBoneWorld.ToLocalDir(velocityWorld);
    c.source = StrikeSource::Body;
}
// The prop's velocity at the contact against victimBoneWorld (the victim's own bone), as ReplayInputs turns the rest.
inline V3 PropVelocityWorld(const HitClaim& c, const Xform& victimBoneWorld) { return victimBoneWorld.FromLocalDir(c.propVelocity); }

// ---------------------------------------------------------------- the DCD contact gate
// Willie_BP.cpp:2994 (decompiled):
//   if (!(((VSize(Hit Impulse) * (Cutting Power + 1)) >= Last Complex Damage Impulse)
//         || (Last Complex Damage Bone != Hit Bone))) return;
// i.e. the call is gated through (a claim) iff the scaled impulse is at least the last one
// recorded on this bone, or this is a different bone than the last recorded one.
inline bool DcdGatePasses(double impulseLen, double cuttingPower, double lastImpulse, bool sameBoneAsLast) {
    return (impulseLen * (cuttingPower + 1.0)) >= lastImpulse || !sameBoneAsLast;
}
// The Last Complex Damage Impulse to store after a pass (the bone is simply set to Hit Bone).
inline double DcdGateNewImpulse(double impulseLen, double cuttingPower) { return impulseLen * (cuttingPower + 1.0); }

// At most kClaimLimiterMaxPerBoneTick claims per (victim, bone) per tick.
class ClaimLimiter {
public:
    bool Allow(uint8_t victim, const std::string& bone) {
        int& n = counts_[Key{victim, bone}];
        if (n >= kClaimLimiterMaxPerBoneTick) return false;
        ++n;
        return true;
    }
    void NewTick() { counts_.clear(); }

private:
    struct Key {
        uint8_t victim;
        std::string bone;
        bool operator<(const Key& o) const { return victim != o.victim ? victim < o.victim : bone < o.bone; }
    };
    std::map<Key, int> counts_;
};

// Per-attacker anti-replay: a high-water claimId plus a fixed-width bitmap of the last
// kClaimDedupeWindow ids relative to it (bit 0 = the high-water id itself). Memory is bounded by
// the number of distinct attackers (uint8_t, so at most 256 entries) regardless of how many claim
// ids are ever seen.
class ClaimDedupe {
public:
    // Returns true iff (attacker, claimId) has not been seen before (and should be processed).
    bool Accept(uint8_t attacker, uint32_t claimId) {
        State& s = state_[attacker];
        if (!s.have) {
            s.have = true;
            s.highest = claimId;
            s.bits.reset();
            s.bits.set(0);
            return true;
        }
        if (claimId == s.highest) return false;           // duplicate of the newest
        int32_t delta = int32_t(claimId - s.highest);      // wrap-aware, as hsm::SeqNewer
        if (delta > 0) {
            size_t shift = size_t(delta);
            if (shift >= kClaimDedupeWindow) s.bits.reset();
            else s.bits <<= shift;                          // ages existing bits; overflowed bits are dropped
            s.highest = claimId;
            s.bits.set(0);
            return true;
        }
        size_t back = size_t(-delta);
        if (back >= kClaimDedupeWindow) return false;       // too far in the past to tell; refuse
        if (s.bits.test(back)) return false;                // duplicate
        s.bits.set(back);
        return true;
    }

    // The attacker left or rejoined: a restarted machine numbers its claims from 1 again, which must not read as repeats.
    void Forget(uint8_t attacker) { state_.erase(attacker); }

private:
    struct State {
        bool have = false;
        uint32_t highest = 0;
        std::bitset<kClaimDedupeWindow> bits;
    };
    std::map<uint8_t, State> state_;
};

// Bone rule, on the victim: can a claim naming bone (with boneIndex = GetBoneIndex on my fighter's mesh) be replayed?
// "None" (NAME_None as text) always can: the game calls Deal Complex Damage with no bone itself (a hit result without
// one, Willie_BP.cpp:7970), its body runs on with it, and GetSocketTransform(None) is the mesh's own frame on both
// machines. Any other name must be a bone of my fighter. An empty name is never one the game produced.
inline bool ReplayBoneOk(const std::string& bone, int32_t boneIndex) {
    return bone == "None" || (!bone.empty() && boneIndex >= 0);
}

// Age rule: claims older than kClaimMaxAgeMs (receiver-estimated) are dropped.
inline bool ClaimFresh(int64_t ageMs) { return ageMs < kClaimMaxAgeMs; }

// Parry rule, decided on the victim: a clash between my weapon and one weapon of that attacker's stand-in cancels a
// claim struck with that same weapon if the clash's attacker-clock timestamp falls within
// [claim.attackerTsMs - kParryBeforeMs, claim.attackerTsMs + kParryAfterMs]. The attacker's other weapon and its body
// (a kick, a punch) are not parried by it (test 4: in a bind, nothing else the attacker did landed).
class ParryWindow {
public:
    void NoteClash(uint8_t attacker, StrikeSource weapon, uint32_t attackerTsMsEstimate) {
        auto& q = clashes_[attacker];
        q.push_back(Clash{attackerTsMsEstimate, weapon});
        if (q.size() > kMaxClashHistory) q.pop_front();
    }
    bool Cancels(const HitClaim& c) const {
        if (c.source == StrikeSource::Body) return false;   // a body strike never met a blade
        auto it = clashes_.find(c.attacker);
        if (it == clashes_.end()) return false;
        for (const Clash& k : it->second) {
            if (k.weapon != c.source) continue;
            int32_t delta = int32_t(k.tsMs - c.attackerTsMs);   // clash ts minus claim ts, wrap-aware
            if (delta >= -kParryBeforeMs && delta <= kParryAfterMs) return true;
        }
        return false;
    }
    void Forget(uint8_t attacker) { clashes_.erase(attacker); }   // its id goes to the next fighter

private:
    struct Clash { uint32_t tsMs; StrikeSource weapon; };
    static constexpr size_t kMaxClashHistory = 64;   // bounds memory; clashes are reported at most every 100ms per weapon
    std::map<uint8_t, std::deque<Clash>> clashes_;
};

// ---------------------------------------------------------------- one blow, many contacts (attacker side)
// A blade resting on a body, or one swing sliding over neighbouring bones, makes the game call Deal Complex Damage on
// every contact. Test 4 logged twenty zero-impulse claims, 73 ms apart, alternating neck_01 and neck_02 (one blow).
// Contacts are grouped by body part, so two bones of one part are one target.
enum BodyPart : uint8_t {
    kBodyPartOther = 0,
    kBodyPartHead,          // head, neck_*
    kBodyPartTorso,         // pelvis, spine_*, clavicle_*
    kBodyPartUpperArmL, kBodyPartLowerArmL, kBodyPartHandL,
    kBodyPartUpperArmR, kBodyPartLowerArmR, kBodyPartHandR,
    kBodyPartThighL, kBodyPartCalfL, kBodyPartFootL,
    kBodyPartThighR, kBodyPartCalfR, kBodyPartFootR,
};

// The body part of a skeleton bone, ignoring ASCII case; kBodyPartOther for anything else ("None", empty, unknown).
inline uint8_t BodyPartOfBone(std::string_view bone) {
    if (SameNameAscii(bone, "head") || StartsWithNameAscii(bone, "neck_")) return kBodyPartHead;
    if (SameNameAscii(bone, "pelvis") || StartsWithNameAscii(bone, "spine_") || StartsWithNameAscii(bone, "clavicle_"))
        return kBodyPartTorso;
    if (bone.size() <= 2) return kBodyPartOther;
    const std::string_view side = bone.substr(bone.size() - 2);
    const bool left = SameNameAscii(side, "_l");
    if (!left && !SameNameAscii(side, "_r")) return kBodyPartOther;
    struct Limb { const char* prefix; uint8_t left, right; };
    static constexpr Limb kLimbs[] = {
        {"upperarm_", kBodyPartUpperArmL, kBodyPartUpperArmR}, {"lowerarm_", kBodyPartLowerArmL, kBodyPartLowerArmR},
        {"hand_", kBodyPartHandL, kBodyPartHandR},             {"thumb_", kBodyPartHandL, kBodyPartHandR},
        {"index_", kBodyPartHandL, kBodyPartHandR},            {"middle_", kBodyPartHandL, kBodyPartHandR},
        {"ring_", kBodyPartHandL, kBodyPartHandR},             {"pinky_", kBodyPartHandL, kBodyPartHandR},
        {"thigh_", kBodyPartThighL, kBodyPartThighR},          {"calf_", kBodyPartCalfL, kBodyPartCalfR},
        {"foot_", kBodyPartFootL, kBodyPartFootR},             {"ball_", kBodyPartFootL, kBodyPartFootR},
    };
    for (const Limb& l : kLimbs)
        if (StartsWithNameAscii(bone, l.prefix)) return left ? l.left : l.right;
    return kBodyPartOther;
}

constexpr double kBlowWindowMs = 250;         // contacts of one key this close to its first one are one blow
constexpr int kClaimsPerSecond = 12;          // per (attacker, victim), sliding
constexpr double kClaimCapWindowMs = 1000;

struct BlowKey {
    uint8_t attacker = 0, victim = 0;
    std::string strikeComponent;
    uint8_t bodyPart = kBodyPartOther;
    bool operator<(const BlowKey& o) const {
        if (attacker != o.attacker) return attacker < o.attacker;
        if (victim != o.victim) return victim < o.victim;
        if (bodyPart != o.bodyPart) return bodyPart < o.bodyPart;
        return strikeComponent < o.strikeComponent;
    }
};

// Decides, per contact, whether it becomes a claim. strength is the DCD gate's scaled impulse (DcdGateNewImpulse).
//   - The first contact of a key opens a kBlowWindowMs window and is sent.
//   - Inside the window one more contact is sent, only when strictly stronger than every one sent in it (Coalesced
//     otherwise); a contact of strength 0 (a blunt part sliding) is never that one (ZeroImpulse).
//   - At most kClaimsPerSecond are sent per (attacker, victim) in any kClaimCapWindowMs (Capped); a capped contact
//     sends nothing and opens nothing.
class BlowCoalescer {
public:
    enum class Verdict : uint8_t { Send, Coalesced, ZeroImpulse, Capped };
    Verdict Allow(const BlowKey& key, double strength, double nowMs) {
        if (!(strength > 0)) strength = 0;                       // NaN and negatives count as no impulse
        Window& w = windows_[key];
        const bool open = w.open && nowMs - w.startMs < kBlowWindowMs;
        if (open) {
            if (strength == 0) return Verdict::ZeroImpulse;
            if (w.extraSent || !(strength > w.strongest)) return Verdict::Coalesced;
        }
        std::deque<double>& sent = sent_[Pair{key.attacker, key.victim}];
        while (!sent.empty() && nowMs - sent.front() >= kClaimCapWindowMs) sent.pop_front();
        if (sent.size() >= size_t(kClaimsPerSecond)) return Verdict::Capped;
        sent.push_back(nowMs);
        if (open) w.extraSent = true;
        else w = Window{true, nowMs, 0, false};
        w.strongest = strength;
        if (windows_.size() > kPruneAbove) Prune(nowMs);
        return Verdict::Send;
    }
    // An entity left, respawned or rejoined: its contacts, as attacker or victim, start over.
    void Forget(uint8_t entity) {
        for (auto it = windows_.begin(); it != windows_.end();)
            it = it->first.attacker == entity || it->first.victim == entity ? windows_.erase(it) : std::next(it);
        for (auto it = sent_.begin(); it != sent_.end();)
            it = it->first.attacker == entity || it->first.victim == entity ? sent_.erase(it) : std::next(it);
    }

private:
    struct Window {
        bool open = false;
        double startMs = 0;
        double strongest = 0;       // the strongest contact sent in this window
        bool extraSent = false;     // the one more has been sent
    };
    struct Pair {
        uint8_t attacker, victim;
        bool operator<(const Pair& o) const { return attacker != o.attacker ? attacker < o.attacker : victim < o.victim; }
    };
    static constexpr size_t kPruneAbove = 256;   // keys whose window closed are dropped past this many
    void Prune(double nowMs) {
        for (auto it = windows_.begin(); it != windows_.end();)
            it = nowMs - it->second.startMs >= kBlowWindowMs ? windows_.erase(it) : std::next(it);
    }
    std::map<BlowKey, Window> windows_;
    std::map<Pair, std::deque<double>> sent_;    // send times in the last kClaimCapWindowMs, at most kClaimsPerSecond
};

// ---------------------------------------------------------------- replays spread over frames (victim side)
constexpr size_t kReplaysPerTick = 8;         // per victim
constexpr size_t kMaxQueuedReplays = 64;      // per victim; more is a flood, dropped

// Claims to replay, per victim in arrival order.
class ReplayQueue {
public:
    bool Push(const HitClaim& c) {                       // false when the victim's queue is full (the claim is dropped)
        std::deque<HitClaim>& q = queues_[c.victim];
        if (q.size() >= kMaxQueuedReplays) return false;
        q.push_back(c);
        return true;
    }
    // Up to maxPerVictim claims of every victim, each victim's in arrival order.
    std::vector<HitClaim> Drain(size_t maxPerVictim = kReplaysPerTick) {
        std::vector<HitClaim> out;
        for (auto it = queues_.begin(); it != queues_.end();) {
            std::deque<HitClaim>& q = it->second;
            for (size_t n = 0; n < maxPerVictim && !q.empty(); ++n) {
                out.push_back(std::move(q.front()));
                q.pop_front();
            }
            it = q.empty() ? queues_.erase(it) : std::next(it);
        }
        return out;
    }
    size_t Size(uint8_t victim) const {
        auto it = queues_.find(victim);
        return it == queues_.end() ? 0 : it->second.size();
    }
    void Forget(uint8_t victim) { queues_.erase(victim); }

private:
    std::map<uint8_t, std::deque<HitClaim>> queues_;
};

// ---------------------------------------------------------------- serialisation
// Follows hsm_protocol.h's Writer/Reader: little-endian, bounded strings, decoders validate
// finiteness, ranges and enums and return false (never crash) on malformed input. hsm_protocol.h
// has no f64 primitive, so doubles are added here as raw little-endian IEEE-754 binary64 via the
// existing u64()/buf, exactly as f32() reinterprets u32().
inline void PutF64(hsm::Writer& w, double v) {
    uint64_t u;
    std::memcpy(&u, &v, sizeof(u));
    w.u64(u);
}
inline bool GetF64(hsm::Reader& r, double& out) {
    uint64_t u = r.u64();
    double v;
    std::memcpy(&v, &u, sizeof(v));
    if (!std::isfinite(v)) return false;
    out = v;
    return true;
}
inline void PutV3(hsm::Writer& w, const V3& v) {
    PutF64(w, v.x);
    PutF64(w, v.y);
    PutF64(w, v.z);
}
inline bool GetV3(hsm::Reader& r, V3& v) {
    bool ok = true;
    ok &= GetF64(r, v.x);
    ok &= GetF64(r, v.y);
    ok &= GetF64(r, v.z);
    return ok;
}
inline void PutQ4(hsm::Writer& w, const Q4& q) {
    PutF64(w, q.x);
    PutF64(w, q.y);
    PutF64(w, q.z);
    PutF64(w, q.w);
}
inline bool GetQ4(hsm::Reader& r, Q4& q) {
    bool ok = true;
    ok &= GetF64(r, q.x);
    ok &= GetF64(r, q.y);
    ok &= GetF64(r, q.z);
    ok &= GetF64(r, q.w);
    return ok;
}
inline void PutXform(hsm::Writer& w, const Xform& x) {
    PutV3(w, x.pos);
    PutQ4(w, x.rot);
    PutV3(w, x.scale);
}
inline bool GetXform(hsm::Reader& r, Xform& x) {
    bool ok = true;
    ok &= GetV3(r, x.pos);
    ok &= GetQ4(r, x.rot);
    ok &= GetV3(r, x.scale);
    return ok;
}

inline void PutClaim(hsm::Writer& w, const HitClaim& m) {
    w.u32(m.claimId);
    w.u8(m.attacker);
    w.u8(m.victim);
    w.u32(m.attackerTsMs);
    w.str(m.hitBone, kMaxClaimString);
    PutV3(w, m.location);
    PutV3(w, m.normal);
    PutV3(w, m.hitVelocity);
    PutV3(w, m.hitImpulse);
    PutF64(w, m.cuttingPower);
    PutF64(w, m.stabRate);
    PutF64(w, m.rigidity);
    PutF64(w, m.kickPower);
    PutF64(w, m.drawCut);
    w.u32(uint32_t(m.bluntDestruction));
    w.u8(m.lowerThreshold ? 1 : 0);
    w.u8(m.damageParentBone ? 1 : 0);
    w.u8(m.extraHighVelocity ? 1 : 0);
    w.u8(uint8_t(m.source));
    w.str(m.strikeComponent, kMaxClaimString);
    w.str(m.strikeBone, kMaxClaimString);
    w.u8(m.hasHitBox ? 1 : 0);
    PutXform(w, m.hitBoxLocal);
    PutV3(w, m.hitBoxExtent);
    w.u8(uint8_t(m.striker));
    w.u64(m.propId);
    PutV3(w, m.propVelocity);
}

inline bool GetClaim(hsm::Reader& r, HitClaim& m) {
    m.claimId = r.u32();
    m.attacker = r.u8();
    m.victim = r.u8();
    m.attackerTsMs = r.u32();
    m.hitBone = r.str(kMaxClaimString);

    bool numOk = true;
    numOk &= GetV3(r, m.location);
    numOk &= GetV3(r, m.normal);
    numOk &= GetV3(r, m.hitVelocity);
    numOk &= GetV3(r, m.hitImpulse);
    numOk &= GetF64(r, m.cuttingPower);
    numOk &= GetF64(r, m.stabRate);
    numOk &= GetF64(r, m.rigidity);
    numOk &= GetF64(r, m.kickPower);
    numOk &= GetF64(r, m.drawCut);

    m.bluntDestruction = int32_t(r.u32());
    uint8_t lt = r.u8(), dpb = r.u8(), ehv = r.u8();
    uint8_t srcRaw = r.u8();
    m.strikeComponent = r.str(kMaxClaimString);
    m.strikeBone = r.str(kMaxClaimString);
    uint8_t hasBox = r.u8();
    numOk &= GetXform(r, m.hitBoxLocal);
    numOk &= GetV3(r, m.hitBoxExtent);
    const uint8_t strikerRaw = r.u8();
    m.propId = r.u64();
    numOk &= GetV3(r, m.propVelocity);

    if (!r.done() || !numOk) return false;
    if (lt > 1 || dpb > 1 || ehv > 1 || hasBox > 1) return false;
    if (srcRaw > uint8_t(StrikeSource::WeaponLeft)) return false;
    if (strikerRaw > uint8_t(StrikerKind::Prop)) return false;
    if (strikerRaw == uint8_t(StrikerKind::Prop)) {
        if (!ValidPropId(m.propId) || srcRaw != uint8_t(StrikeSource::Body)) return false;   // a prop, never from a hand
    } else if (m.propId != 0 || m.propVelocity.x != 0 || m.propVelocity.y != 0 || m.propVelocity.z != 0) {
        return false;                                                                        // a fighter's names no prop
    }

    m.lowerThreshold = lt == 1;
    m.damageParentBone = dpb == 1;
    m.extraHighVelocity = ehv == 1;
    m.source = StrikeSource(srcRaw);
    m.hasHitBox = hasBox == 1;
    m.striker = StrikerKind(strikerRaw);
    return true;
}

}  // namespace hsm::combat
