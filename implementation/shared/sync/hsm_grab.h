// Protocol revision 9: grabs across machines. A grab is native on the grabber's machine (Willie_BP ReceiveTick binds
// the held body to its hand with "Grab R/L Constraint"), so a stand-in held that way moves only on the grabber's
// screen. The grabber's owner therefore streams what each hand holds (whose fighter, which bone, and the grip: its
// hand in that bone's frame), and the held fighter's owner pulls that bone toward the grabber's hand as its stand-in
// shows it, through a soft, force-limited handle: the victim can struggle, and past kGrabBreakUu for kGrabBreakMs
// breaks free. A hold ends when the grabber lets go, its stream is silent for kGrabLeaseMs, or the grabber dies.
//   GrabState  grabber's owner -> host -> others: reliable on a change, unreliable on the kGrabRefreshMs refresh
//
// Design and constants from Cyrex0/HalfSword-MP mods/HSMPInteract/Scripts/main.lua (cfg, drive_held), Copyright (c)
// 2026 Cyrex0 and the HalfSword-MP contributors, MIT (see THIRD_PARTY_NOTICES.md). HSMP moves a PhysicsHandle's
// target to grip + clamp(hand - grip, lead); here the same spring-damper is a per-step velocity change on the bone
// (no component spawned), integrated implicitly like the handle's acceleration drive.
#pragma once
#include "hsm_protocol.h"
#include "sync/hsm_pose.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace hsm {

constexpr size_t kGrabHands = 2;        // 0 right (hand_r), 1 left (hand_l)
constexpr size_t kMaxGrabBone = 64;     // bone names are short ("lowerarm_l"); longer is garbage
constexpr double kMaxGripUu = 60.0;     // cm from the bone's origin: no hand holds farther out (HSMP MAX_POINT)

struct GrabHand {
    bool held = false;
    uint8_t target = 0;                 // the held fighter
    uint32_t grabId = 0;                // nonzero, the grabber's; a new one per new hold
    std::string bone;                   // the held body
    sync::V3 grip;                      // the grabber's hand in that bone's frame, cm
};

struct GrabStateMsg {
    uint8_t grabber = 0;
    uint32_t seq = 0;                   // +1 per message from this grabber; receivers keep the newest
    std::array<GrabHand, kGrabHands> hands{};
};

namespace grabdetail {
using sync::V3;
inline V3 Add(const V3& a, const V3& b) { return V3{ a.x + b.x, a.y + b.y, a.z + b.z }; }
inline V3 Sub(const V3& a, const V3& b) { return V3{ a.x - b.x, a.y - b.y, a.z - b.z }; }
inline V3 Scale(const V3& a, double s) { return V3{ a.x * s, a.y * s, a.z * s }; }
inline double Len(const V3& a) { return std::sqrt(a.x * a.x + a.y * a.y + a.z * a.z); }
inline bool Finite(const V3& a) { return std::isfinite(a.x) && std::isfinite(a.y) && std::isfinite(a.z); }
inline V3 ClampLen(const V3& a, double lim) { const double l = Len(a); return l > lim ? Scale(a, lim / l) : a; }
}  // namespace grabdetail

// A bone name: letters, digits and underscores ("lowerarm_l", "spine_03"), at most kMaxGrabBone. The receiver interns it
// as an FName, so nothing else may travel.
inline bool ValidBoneName(const std::string& s) {
    if (s.empty() || s.size() > kMaxGrabBone) return false;
    for (const char ch : s) {
        const unsigned char c = static_cast<unsigned char>(ch);
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_')) return false;
    }
    return true;
}


// A hold that may travel: a fighter other than the grabber, a grab id, a named bone, a finite grip within reach.
inline bool ValidHold(uint8_t grabber, const GrabHand& h) {
    return h.held && h.target != 0 && h.target <= kMaxWillies && h.target != grabber && h.grabId != 0
        && ValidBoneName(h.bone) && grabdetail::Finite(h.grip)
        && grabdetail::Len(h.grip) <= kMaxGripUu;
}

// Per hand: u8 held, and if held: u8 target, u32 grab id, bone, f32 grip x y z.
inline std::vector<uint8_t> Encode(const GrabStateMsg& m) {
    Writer w; w.header(MsgType::GrabState);
    w.u8(m.grabber); w.u32(m.seq);
    for (const GrabHand& h : m.hands) {
        w.u8(h.held ? 1 : 0);
        if (!h.held) continue;
        w.u8(h.target); w.u32(h.grabId); w.str(h.bone, kMaxGrabBone);
        w.f32(float(h.grip.x)); w.f32(float(h.grip.y)); w.f32(float(h.grip.z));
    }
    return w.buf;
}
inline bool Decode(const uint8_t* d, size_t n, GrabStateMsg& m) {
    if (n > kMaxPacket) return false;
    Reader r(d, n); MsgType t; if (!r.header(t) || t != MsgType::GrabState) return false;
    m.grabber = r.u8(); m.seq = r.u32();
    if (!r.ok() || m.grabber == 0 || m.grabber > kMaxWillies) return false;
    for (GrabHand& h : m.hands) {
        h = GrabHand{};
        const uint8_t held = r.u8();
        if (!r.ok() || held > 1) return false;
        if (!held) continue;
        h.held = true; h.target = r.u8(); h.grabId = r.u32(); h.bone = r.str(kMaxGrabBone);
        h.grip.x = r.f32(); h.grip.y = r.f32(); h.grip.z = r.f32();
        if (!r.ok() || !ValidHold(m.grabber, h)) return false;
    }
    return r.done();
}

}  // namespace hsm

namespace hsm::sync {

constexpr double kGrabRefreshMs = 100.0;   // a hold is resent this often: the lease refresh (HSMP grab_update_ms)
constexpr double kGrabLeaseMs = 1500.0;    // nothing newer heard for this long: released (grab_lease_ms)
constexpr double kGripMaxUu = 55.0;        // the sender clamps the grip to this, inside kMaxGripUu (grip_max)
constexpr double kGrabBreakUu = 130.0;     // the grip farther than this from the hand ... (grab_break)
constexpr double kGrabBreakMs = 400.0;     // ... for this long: broke free until a new grab (grab_break_ms)
// A body that cannot struggle (dead, fallen, knocked out) breaks free only on a glitch: farther and longer than that.
constexpr double kGrabDownBreakUu = 250.0, kGrabDownBreakMs = 1000.0;
// The grabber's stand-in shows its hand a playout delay late; the pull aims this far ahead along the hand's velocity.
constexpr double kGrabAnchorLeadS = 0.08, kGrabAnchorLeadCapUu = 40.0;

// The same holds (the same grabs of the same bones); grips and seq may differ.
inline bool SameHolds(const GrabStateMsg& a, const GrabStateMsg& b) {
    if (a.grabber != b.grabber) return false;
    for (size_t i = 0; i < kGrabHands; ++i) {
        const GrabHand& x = a.hands[i]; const GrabHand& y = b.hands[i];
        if (x.held != y.held || x.target != y.target || x.grabId != y.grabId || x.bone != y.bone) return false;
    }
    return true;
}

enum class GrabSend { None, Change, Refresh };   // Change goes reliable, Refresh unreliable

// The grabber's side. Update every tick with what the owned fighter's hands hold (grab ids are assigned here: a new
// one when a hand takes another fighter or bone); it fills `out` and says when to send it. An invalid hold counts as
// a free hand, and nothing is sent while nothing is or was held. Reset when the owned fighter is replaced.
class GrabSender {
public:
    GrabSend Update(uint8_t grabber, const std::array<GrabHand, kGrabHands>& reads, double nowMs, GrabStateMsg& out) {
        GrabStateMsg next; next.grabber = grabber;
        bool any = false;
        for (size_t i = 0; i < kGrabHands; ++i) {
            GrabHand h = reads[i];
            const GrabHand& was = last_.hands[i];
            const bool same = last_.grabber == grabber && was.held && was.target == h.target && was.bone == h.bone;
            h.grabId = same ? was.grabId : (nextId_ + 1 == 0 ? 1 : nextId_ + 1);
            if (grabdetail::Finite(h.grip)) h.grip = grabdetail::ClampLen(h.grip, kGripMaxUu);
            if (!ValidHold(grabber, h)) continue;
            if (!same) nextId_ = h.grabId;
            next.hands[i] = h; any = true;
        }
        bool wasAny = false;
        for (const GrabHand& h : last_.hands) wasAny = wasAny || h.held;
        GrabSend send = GrabSend::None;
        if (!SameHolds(next, last_) && (any || wasAny)) send = GrabSend::Change;
        else if (any && nowMs - sentMs_ >= kGrabRefreshMs) send = GrabSend::Refresh;
        if (send == GrabSend::None) { if (!any && !wasAny) last_.grabber = grabber; return send; }
        next.seq = ++seq_;
        last_ = next; sentMs_ = nowMs; out = next;
        return send;
    }
    void Reset() { last_ = GrabStateMsg{}; }   // seq and grab ids keep counting, so receivers take what follows

private:
    GrabStateMsg last_;                        // the last sent
    double sentMs_ = 0;
    uint32_t seq_ = 0, nextId_ = 0;
};

struct GrabPull {                              // one hold on a fighter of mine, to pull
    uint8_t grabber = 0, hand = 0, target = 0;
    uint32_t grabId = 0;
    std::string bone;
    V3 grip;                                   // in the bone's frame, cm
};

// The held side: the newest state of every grabber, and which holds broke free.
class GrabInbox {
public:
    // True if m is its grabber's newest state. An older one is never taken back, however long the stream was silent:
    // silence is the normal state after a let-go, so a delayed refresh must not revive the hold. A grabber whose owner
    // restarted its counter has left first, and Forget lets its stream start afresh.
    bool Accept(const GrabStateMsg& m, double nowMs) {
        if (m.grabber == 0 || m.grabber > kMaxWillies) return false;
        Slot& s = slots_[m.grabber - 1];
        if (s.heard && !SeqNewer(m.seq, s.state.seq)) return false;
        s.state = m; s.heard = true; s.heardMs = nowMs;
        for (const GrabHand& h : m.hands) if (h.held) s.topId = std::max(s.topId, h.grabId);
        return true;
    }
    // The holds to pull now: fresh, valid, on a fighter of mine, by a living grabber, not broken free.
    std::vector<GrabPull> Pulls(double nowMs, const std::function<bool(uint8_t)>& mine,
                                const std::function<bool(uint8_t)>& grabberDead) const {
        std::vector<GrabPull> out;
        for (const Slot& s : slots_) {
            if (!s.heard || nowMs - s.heardMs > kGrabLeaseMs || grabberDead(s.state.grabber)) continue;
            for (size_t i = 0; i < kGrabHands; ++i) {
                const GrabHand& h = s.state.hands[i];
                if (!ValidHold(s.state.grabber, h) || !mine(h.target)) continue;
                if (s.brk[i].broken && s.brk[i].grabId == h.grabId) continue;
                if (h.grabId <= s.retiredUpTo && nowMs < s.retiredUntilMs) continue;
                out.push_back(GrabPull{ s.state.grabber, uint8_t(i), h.target, h.grabId, h.bone, h.grip });
            }
        }
        return out;
    }
    // This tick's grip-to-hand distance of a pulled hold; true once it broke free (it stays broken for that grab): farther
    // than breakUu for breakMs (kGrabDownBreakUu/Ms for a body that cannot struggle).
    bool NoteDistance(const GrabPull& p, double distUu, double nowMs, double breakUu = kGrabBreakUu, double breakMs = kGrabBreakMs) {
        if (p.grabber == 0 || p.grabber > kMaxWillies || p.hand >= kGrabHands) return false;
        Break& b = slots_[p.grabber - 1].brk[p.hand];
        if (b.grabId != p.grabId) b = Break{ p.grabId };
        if (b.broken) return true;
        if (distUu > breakUu) {
            if (!b.far) { b.far = true; b.farSinceMs = nowMs; }
            b.broken = nowMs - b.farSinceMs >= breakMs;
        } else {
            b.far = false;
        }
        return b.broken;
    }
    // A fighter of mine was replaced (it respawned, or appeared): every hold on it so far was on an earlier body, and the
    // grabber's machine only learns of that a round trip later. They count as broken free, so the refreshes of those
    // grabs do not bring them back; a new grab pulls the new body.
    void DropTarget(uint8_t target) {
        for (Slot& s : slots_)
            for (size_t i = 0; i < kGrabHands; ++i)
                if (s.state.hands[i].held && s.state.hands[i].target == target) s.brk[i] = Break{ s.state.hands[i].grabId, false, true, 0 };
    }
    // The grabber left, restarted or respawned. Its stream starts afresh (a restarted counter is taken), but every grab it
    // made so far -- any id up to the highest heard, held now or let go of -- is retired for kGrabLeaseMs: refreshes its
    // old body sent before its machine saw the respawn are still on the way, and a reordered one may predate its let-go.
    // A new grab (a higher id: the sender keeps counting across a respawn) pulls at once; a restarted machine reusing
    // low ids waits out the retirement at most, and a rejoin takes longer than that anyway. A retirement still running
    // is carried over: a second Forget within it keeps the old grabs retired and adds the ones heard since.
    void Forget(uint8_t grabber, double nowMs) {
        if (grabber == 0 || grabber > kMaxWillies) return;
        Slot& s = slots_[grabber - 1];
        Slot fresh;
        fresh.retiredUpTo = nowMs < s.retiredUntilMs ? std::max(s.topId, s.retiredUpTo) : s.topId;
        fresh.retiredUntilMs = nowMs + kGrabLeaseMs;
        s = fresh;
    }
    // Every hold is dropped, but what was retired stays retired and the highest ids heard are remembered: a refresh of a
    // grab made before still on its way (or a stream replayed after a pause) must not bring it back.
    void Clear() {
        for (Slot& s : slots_) {
            Slot fresh;
            fresh.topId = s.topId;
            fresh.retiredUpTo = s.retiredUpTo;
            fresh.retiredUntilMs = s.retiredUntilMs;
            s = fresh;
        }
    }

private:
    struct Break { uint32_t grabId = 0; bool far = false, broken = false; double farSinceMs = 0; };
    struct Slot {
        bool heard = false; double heardMs = 0; GrabStateMsg state; std::array<Break, kGrabHands> brk{};
        uint32_t topId = 0;                                        // the highest grab id heard since the last Forget
        uint32_t retiredUpTo = 0; double retiredUntilMs = 0;       // grabs up to this id are dead until then
    };
    std::array<Slot, kMaxWillies> slots_{};
};

struct GrabPullParams {
    double stiffness = 1500.0;   // 1/s^2: HSMP grab_k, a soft handle the victim can struggle against
    double damping = 150.0;      // 1/s: grab_d; zeta = 150 / (2 sqrt 1500) ~ 1.9, overdamped: never past the hand
    double leadUu = 40.0;        // the spring's stretch is capped here: pull <= stiffness x lead (grab_lead)
    double maxDvUu = 800.0;      // cm/s per step at most (HSMP apply_max_dv): a glitch never launches a body
    double minDtS = 0.004, maxDtS = 0.080;   // the stand-in servo's frame-time clamp
};

// A held body that cannot struggle (dead, fallen, knocked out): carried the way the game carries a held NPC, not
// dragged on a soft spring (test 2026-10-25: grabbing another player's body "worked" but did not feel like the game's).
// Three times as stiff, twice the lead, still overdamped (zeta = 200 / (2 sqrt 4500) ~ 1.5), and the same per-step cap
// raised so a body keeps up with a walking grabber.
inline GrabPullParams DownPullParams() {
    GrabPullParams p;
    p.stiffness = 4500.0; p.damping = 200.0; p.leadUu = 80.0; p.maxDvUu = 1500.0;
    return p;
}
// Share of a down body's pull also given to its pelvis and chest, so the whole body comes along, not just the held limb.
constexpr double kGrabDownSpread = 0.5;

// Where the pull aims: the grabber's stand-in hand, advanced along its velocity by leadS (at most capUu), since the
// stand-in shows the grabber a playout delay late. Non-finite velocity: the hand itself.
inline V3 GrabAnchor(const V3& hand, const V3& handVel, double leadS = kGrabAnchorLeadS, double capUu = kGrabAnchorLeadCapUu) {
    using namespace grabdetail;
    if (!Finite(hand) || !Finite(handVel) || !std::isfinite(leadS)) return hand;
    return Add(hand, ClampLen(Scale(handVel, std::max(leadS, 0.0)), capUu));
}

// The pose bone index of a held bone name, or -1.
inline int PoseBoneIndex(const std::string& name) {
    for (int i = 0; i < kPoseBones; ++i) if (name == kPoseBoneNames[i]) return i;
    return -1;
}
// The pose bones (bit per index) the held fighter's stand-in leaves to the grabber's native hold on the grabber's screen,
// so the servo and the grab constraint do not pull the bone two ways: the held bone and its parent, or, for a body that
// cannot struggle, the held limb up to the torso (the owner's own pull brings the rest after the hand). Never the whole
// body: a stand-in's bodies have no gravity and ignore the floor, so an undriven torso would float.
inline uint32_t YieldBones(int bone, bool down) {
    if (bone < 0 || bone >= kPoseBones) return 0;
    if (!down) return (1u << bone) | (1u << kPoseParent[size_t(bone)]);
    uint32_t m = 0;
    for (int b = bone; b != kBonePelvis && !(b >= 2 && b <= 5); b = kPoseParent[size_t(b)]) m |= 1u << b;   // up to pelvis / spine
    return m ? m : (1u << bone);
}

// The velocity change for the held bone this step (SetPhysicsLinearVelocity, added): backward Euler of the handle's
// spring-damper toward the grabber's hand, the hand advanced by its own velocity over the step and the damping
// relative to it, so a bone moving with the hand needs nothing and a moving hand is followed without lag. Stable at
// any step. Non-finite input: no change.
inline V3 GrabPullDv(const V3& grip, const V3& gripVel, const V3& hand, const V3& handVel, double dtS,
                     const GrabPullParams& p = {}) {
    using namespace grabdetail;
    if (!Finite(grip) || !Finite(gripVel) || !Finite(hand) || !Finite(handVel) || !std::isfinite(dtS)) return {};
    const double dt = std::clamp(dtS, p.minDtS, p.maxDtS);
    const V3 stretch = ClampLen(Sub(Add(hand, Scale(handVel, dt)), grip), p.leadUu);
    const V3 drive = Add(Scale(stretch, p.stiffness), Scale(handVel, p.damping));
    const V3 v = Scale(Add(gripVel, Scale(drive, dt)), 1.0 / (1.0 + p.damping * dt + p.stiffness * dt * dt));
    const V3 dv = ClampLen(Sub(v, gripVel), p.maxDvUu);
    return Finite(dv) ? dv : V3{};
}

}  // namespace hsm::sync
