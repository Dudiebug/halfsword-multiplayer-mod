#include "gameplay/owner_sync.h"
#include "log.h"
#include "session/hsm_authority.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>

namespace hsm::game {
using namespace hsm::ue;
using sync::V3;

namespace {
constexpr double kPoseEveryMs = 1000.0 / 60.0;      // players: HSMP's 60 Hz pose stream
constexpr double kBotPoseEveryMs = 1000.0 / 30.0;   // bots: up to 10 of them, half rate (the buffer adds ~17 ms delay)
constexpr double kVitalsEveryMs = 1000.0 / 15.0;
constexpr double kBindGraceMs = 3000.0;             // a fresh stand-in's mesh registers within a few frames
constexpr double kBindRetryMs = 500.0;              // after the grace: retry this often (a bind resolves names)
constexpr double kSnapClearUu = 90.0;               // never snap a stand-in's pelvis onto a local one (main.lua:2432)

double Dist(const V3& a, const V3& b) { return std::sqrt((a.x - b.x) * (a.x - b.x) + (a.y - b.y) * (a.y - b.y) + (a.z - b.z) * (a.z - b.z)); }
}  // namespace

double SteadyNowMs() {
    using namespace std::chrono;
    return duration<double, std::milli>(steady_clock::now().time_since_epoch()).count();
}

// ---- OwnerStreams ------------------------------------------------------------------------------------------------------

void OwnerStreams::Start(double nowMs) {
    Clear();
    startMs_ = nowMs;
}

void OwnerStreams::Clear() { *this = OwnerStreams{}; }

uint32_t OwnerStreams::ClockMs(double nowMs) const {
    return static_cast<uint32_t>(std::llround(std::max(0.0, nowMs - startMs_))) + 1000u;   // as the mirror bench
}

void OwnerStreams::Tick(Adapter& adapter, double nowMs, float dt, const std::vector<std::pair<uint8_t, UObject*>>& owned, const Send& send) {
    if (!Started()) return;
    const double stepS = prevDtS_ > 0 ? prevDtS_ : dt;   // the step that produced the state read now
    prevDtS_ = dt;
    if (slowFrames_.Push(double(dt) * 1000.0, nowMs))
        HSM_LOG("owner: slow sender (frame): %u of %u frames past %.0f ms in %.0f s, worst %.1f ms; a pose still goes every frame",
                slowFrames_.Slow(), slowFrames_.Frames(), sync::kSlowGapMs, sync::kSlowWindowMs / 1000.0, slowFrames_.WorstMs());
    const uint16_t stepUs = static_cast<uint16_t>(std::clamp(stepS * 1e6, 0.0, 65535.0));
    std::array<bool, kMaxWillies + 1> live{};
    for (const auto& [e, w] : owned)
        if (ValidEntity(e) && w) live[e] = true;
    for (size_t e = 1; e <= kMaxWillies; ++e)
        if (!live[e]) owned_[e].reset();
    const uint32_t ts = ClockMs(nowMs);
    for (const auto& [e, w] : owned) {
        if (!ValidEntity(e) || !w) continue;
        std::unique_ptr<Owned>& slot = owned_[e];
        if (!slot) slot = std::make_unique<Owned>();
        Owned& o = *slot;
        if (o.willie != w) { o.willie = w; o.havePelvis = false; o.failLogged = false; }
        if (nowMs >= o.nextPoseMs) {
            o.nextPoseMs = std::max(o.nextPoseMs + (e > kMaxFighters ? kBotPoseEveryMs : kPoseEveryMs), nowMs);
            sync::PoseFrame f;
            if (o.rig.Bind(w) && o.rig.Read(ts, stepUs, &f, nullptr)) {
                const FVector at = adapter.ActorLocation(w);
                PoseMsg m;
                m.entity = e;
                m.rootX = float(at.x); m.rootY = float(at.y); m.rootZ = float(at.z);
                m.yawDeg = float(adapter.ActorYaw(w));
                m.pose = sync::EncodePose(f);
                send(Encode(m));
                ++posesSent_;
                o.pelvis = f.bones[sync::kBonePelvis].pos;
                o.havePelvis = true;
            } else {
                ++readFailed_;
                o.havePelvis = false;
                if (!o.failLogged) HSM_LOG("owner: entity %u (%s) cannot be read for streaming", e, ObjectName(w).c_str());
                o.failLogged = true;
            }
        }
        if (nowMs >= o.nextVitalsMs) {
            o.nextVitalsMs = std::max(o.nextVitalsMs + kVitalsEveryMs, nowMs);
            VitalsMsg v;
            if (adapter.ReadVitals(w, e, v)) { send(Encode(v)); ++vitalsSent_; }
        }
    }
}

bool OwnerStreams::PelvisOf(uint8_t entity, V3& out) const {
    if (!ValidEntity(entity) || !owned_[entity] || !owned_[entity]->havePelvis) return false;
    out = owned_[entity]->pelvis;
    return true;
}

// ---- StandIns ----------------------------------------------------------------------------------------------------------

void StandIns::OnPose(const PoseMsg& m, const sync::PoseFrame& frame, double nowMs) {
    if (!ValidEntity(m.entity)) return;
    Slot& s = slots_[m.entity];
    if (!s.feed) s.feed = std::make_unique<Feed>();
    if (!s.feed->player.Push(frame, std::llround(nowMs))) { ++s.feed->rejected; return; }
    s.feed->root.Push(frame.tsMs, V3{m.rootX, m.rootY, m.rootZ}, m.yawDeg);
    ++s.feed->poses;
    if (!s.feed->slowSet) { s.feed->slow = sync::SlowSenderMeter(m.entity > kMaxFighters ? kBotPoseEveryMs : kPoseEveryMs); s.feed->slowSet = true; }
    if (s.feed->slow.Push(frame.tsMs, nowMs)) {
        const sync::SlowReport& r = s.feed->slow.Last();
        HSM_LOG("standins: slow sender (%s): entity %u, %u of %u pose gaps past %.0f ms (frame %u, network %u), worst owner gap %.0f ms, "
                "worst arrival gap %.0f ms", sync::SlowCauseName(r.cause), m.entity, r.frame + r.network, r.gaps, s.feed->slow.SlowGapMs(),
                r.frame, r.network, r.worstTsGapMs, r.worstArrivalGapMs);
    }
    if (!s.feed->haveHands || SeqNewer(frame.tsMs, s.feed->handTs)) {
        for (int h = 0; h < sync::kPoseWeapons; ++h) {
            uint32_t cls = 0;
            V3 pos{};
            for (const sync::WeaponSample& w : frame.weapons)
                if (w.present && w.hand == h) { cls = w.classHash ? w.classHash : 1; pos = w.pos; }
            if (!s.feed->haveHands || cls != s.feed->handClass[h]) {
                s.feed->handPrevClass[h] = s.feed->haveHands ? s.feed->handClass[h] : 0;
                s.feed->handSinceMs[h] = nowMs;
                s.feed->handFirstPos[h] = pos;
            }
            s.feed->handClass[h] = cls;
            s.feed->handPos[h] = pos;
        }
        s.feed->handTs = frame.tsMs;
        s.feed->haveHands = true;
    }
}

void StandIns::OnVitals(const VitalsMsg& v) {
    if (!ValidEntity(v.entity)) return;
    Slot& s = slots_[v.entity];
    if (!s.feed) s.feed = std::make_unique<Feed>();
    s.feed->vitals = v;
    s.feed->haveVitals = true;
}

void StandIns::Attach(uint8_t entity, UObject* willie) {
    if (!ValidEntity(entity) || !willie) return;
    Slot& s = slots_[entity];
    s.actor = std::make_unique<Actor>();
    s.actor->willie = willie;
    s.actor->index = ObjectIndex(willie);
    s.actor->bindDeadlineMs = SteadyNowMs() + kBindGraceMs;
    s.window = Window{};
    HSM_LOG("standins: entity %u -> %s", entity, ObjectName(willie).c_str());
}

void StandIns::Detach(uint8_t entity) {
    if (ValidEntity(entity)) slots_[entity].actor.reset();
}

void StandIns::Forget(uint8_t entity) {
    if (ValidEntity(entity)) slots_[entity] = Slot{};
}

void StandIns::Clear() {
    for (Slot& s : slots_) s = Slot{};
    costSumMs_ = costMaxMs_ = 0;
    ticks_ = 0;
}

bool StandIns::IsStandIn(const UObject* o) const { return o && EntityOf(o) != 0; }

// Both check the object-array slot, so a stand-in the game destroyed since the last Tick (and its reused address) is
// never taken for the stand-in.
uint8_t StandIns::EntityOf(const UObject* willie) const {
    if (!willie) return 0;
    for (size_t e = 1; e <= kMaxWillies; ++e)
        if (const Actor* a = slots_[e].actor.get(); a && a->willie == willie && ObjectAt(a->index) == willie) return uint8_t(e);
    return 0;
}

UObject* StandIns::ActorOf(uint8_t entity) const {
    const Actor* a = ValidEntity(entity) ? slots_[entity].actor.get() : nullptr;
    return a && ObjectAt(a->index) == a->willie ? a->willie : nullptr;
}

const VitalsMsg* StandIns::Vitals(uint8_t entity) const {
    if (!ValidEntity(entity) || !slots_[entity].feed || !slots_[entity].feed->haveVitals) return nullptr;
    return &slots_[entity].feed->vitals;
}

bool StandIns::PoseHand(uint8_t entity, int hand, uint32_t& classHash) const {
    if (!ValidEntity(entity) || hand < 0 || hand >= sync::kPoseWeapons) return false;
    const Feed* f = slots_[entity].feed.get();
    if (!f || !f->haveHands || f->handClass[hand] == 1) return false;
    classHash = f->handClass[hand];
    return true;
}

bool StandIns::PoseHandInfo(uint8_t entity, int hand, PoseHandView& out) const {
    if (!ValidEntity(entity) || hand < 0 || hand >= sync::kPoseWeapons) return false;
    const Feed* f = slots_[entity].feed.get();
    if (!f || !f->haveHands) return false;
    out.classHash = f->handClass[size_t(hand)];
    out.prevClassHash = f->handPrevClass[size_t(hand)];
    out.sinceMs = f->handSinceMs[size_t(hand)];
    out.pos = f->handPos[size_t(hand)];
    out.firstPos = f->handFirstPos[size_t(hand)];
    return true;
}

bool StandIns::OwnerClockNow(uint8_t entity, double nowMs, int64_t& out) const {
    if (!ValidEntity(entity) || !slots_[entity].feed || !slots_[entity].feed->player.hasOffset()) return false;
    out = std::llround(nowMs - slots_[entity].feed->player.offsetMs());
    return true;
}

bool StandIns::ShownOwnerTs(uint8_t entity, uint32_t& out) const {
    if (!ValidEntity(entity) || !slots_[entity].actor || !slots_[entity].actor->haveShown) return false;
    out = slots_[entity].actor->shownTs;
    return true;
}

int StandIns::Count() const {
    int n = 0;
    for (const Slot& s : slots_) n += s.actor ? 1 : 0;
    return n;
}

void StandIns::Tick(Adapter& adapter, double nowMs, float dt, const std::vector<V3>& avoid) {
    bool any = false;
    const auto t0 = std::chrono::steady_clock::now();
    for (size_t e = 1; e <= kMaxWillies; ++e) {
        if (!slots_[e].actor) continue;
        any = true;
        TickOne(adapter, uint8_t(e), slots_[e], nowMs, dt, avoid);
    }
    if (!any) return;
    const double costMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    ++ticks_;
    costSumMs_ += costMs;
    costMaxMs_ = std::max(costMaxMs_, costMs);
}

void StandIns::TickOne(Adapter& adapter, uint8_t entity, Slot& s, double nowMs, float dt, const std::vector<V3>& avoid) {
    Actor& a = *s.actor;
    if (!adapter.StillValid(a.willie, a.index)) {
        HSM_LOG("standins: entity %u's stand-in is gone", entity);
        s.actor.reset();
        return;
    }
    if (!a.rig.Bound()) {
        if (nowMs < a.nextBindMs) return;
        if (!a.rig.Bind(a.willie)) {
            if (nowMs > a.bindDeadlineMs) {
                if (!a.bindLogged) HSM_LOG("standins: entity %u's stand-in does not bind; retrying every %.0f ms", entity, kBindRetryMs);
                a.bindLogged = true;
                a.nextBindMs = nowMs + kBindRetryMs;
            }
            return;
        }
        a.driver.Reset();
        a.haveTarget = false;
    }
    if (!a.rig.StillValid()) { a.rig.Unbind(); return; }
    a.rig.Neutralise(nowMs);
    if (!s.feed) return;   // nothing streamed yet: the gravity-free bodies hold where they spawned
    // The owner's death, shown on the stand-in. The game's lock-on and AI skip a DED fighter; without it a player cut in
    // half stayed a live target on the other machine, and the lock-on kept aiming at where they had stood. The
    // stand-in's own tick is blocked, so setting DED runs none of the game's Death logic.
    if (s.feed->haveVitals) {
        const VitalsMsg& v = s.feed->vitals;
        const bool dead = (v.flags & 1u) != 0;
        if (dead != a.deadShown && SetBool(a.willie, "DED", dead)) {
            a.deadShown = dead;
            if (double* hp = Field<double>(a.willie, "Health"); hp && dead) *hp = 0.0;
            HSM_LOG("standins: entity %u's stand-in marked %s", entity, dead ? "dead" : "alive");
            // The lock-on never reads DED: Willie_BP's 0.1 s pulse (decomp Willie_BP.cpp:15778) sphere-traces object type 13,
            // the enemy's "Lock On Detector Beacon", and aims at the first hit; Lock On Found is rewritten each pulse and
            // cleared at :16545. The game's own release is the beacon going away: Death() destroys it (:8259) and the tick
            // turns its collision off while Fallen (:16565), neither of which runs on a stand-in. So do the same here.
            UObject** bp = Field<UObject*>(a.willie, "Lock On Detector Beacon");
            UObject* beacon = bp ? *bp : nullptr;
            if (beacon && ObjectAt(ObjectIndex(beacon)) == beacon) {
                Call(beacon, "SetCollisionEnabled").set<uint8_t>("NewType", dead ? 0 : 1).invoke();   // NoCollision / QueryOnly
                HSM_LOG("standins: entity %u's lock-on beacon %s", entity, dead ? "off; a lock-on on it releases" : "back on");
            }
        }
        // Down, not dead yet (test 2026-10-25: cut in half, the owner lay fallen and knocked out at 0 health for 23 s
        // before the game called it a death, and the other player's lock-on kept aiming at the standing spot its capsule
        // was left at). The capsule follows the body, and the stand-in's health is the owner's 0, while down.
        const bool down = dead || (v.flags & 6u) != 0 || v.health <= 0.f;
        if (down != a.downShown) {
            a.downShown = down;
            HSM_LOG("standins: entity %u is %s (flags %u, health %.0f)", entity, down ? "down; the capsule follows the body" : "up again",
                    unsigned(v.flags), double(v.health));
        }
        if (!dead && v.health <= 0.f)
            if (double* hp = Field<double>(a.willie, "Health"); hp && *hp > 0.0) *hp = 0.0;
    }

    sync::FighterNow now;
    if (!a.rig.Read(0, 0, nullptr, &now)) return;
    if (const uint32_t dead = deadBones_ ? deadBones_(entity) : 0u)   // hidden or cut off: no body to drive or measure
        for (int i = 0; i < sync::kPoseBones; ++i) if ((dead >> i) & 1u) { now.valid[i] = false; now.velValid[i] = false; }
    a.rig.SetPinnedWeapons(heldWeapons_ ? heldWeapons_(entity) : 0u);
    bool snapBlocked = false;
    if (a.haveTarget)
        for (const V3& p : avoid) snapBlocked = snapBlocked || Dist(a.lastTargetPelvis, p) < kSnapClearUu;
    sync::PoseFrame target;
    const sync::DriverStep st = a.driver.Step(s.feed->player, nowMs, now, dt, snapBlocked, target);
    const uint32_t held = heldBones_ ? heldBones_(entity) : 0u;   // bones my fighter's grab holds: left to the grab
    if (st.have) {
        // The capsule goes where the owner's capsule was at the shown time, facing its way; first, since a teleporting
        // capsule may carry the Mesh, then Shift (HSMP main.lua:4773 drive_root, then :4791 snap_mesh).
        V3 root;
        double yaw = 0.0;
        // A dead owner's capsule stays where they stood (or fell from); the stand-in's goes with the body instead, so
        // nothing that aims at the actor (lock-on, AI) aims at an empty spot.
        const bool haveRoot = !a.deadShown && !a.downShown && s.feed->root.At(st.info.playTsMs, root, yaw);
        a.rig.PlaceCapsule(haveRoot ? root : target.bones[sync::kBonePelvis].pos, haveRoot ? &yaw : nullptr);
        a.lastTargetPelvis = target.bones[sync::kBonePelvis].pos;
        a.haveTarget = true;
        a.shownTs = st.info.playTsMs;
        a.haveShown = true;
    }
    if (st.place) a.rig.Shift(st.shift);
    if (st.have && st.pelvisErrUu >= 0.0) {
        if (!held) a.rig.Drive(st.cmd);
        else {
            sync::FighterCommand cmd = st.cmd;
            for (int i = 0; i < sync::kPoseBones; ++i) if ((held >> i) & 1u) cmd.drive[i] = false;
            a.rig.Drive(cmd);
        }
    }
    if (st.have) a.rig.ApplyGrip(target.grip);   // after Neutralise and Drive: the owner's grips on this stand-in's hands
    if (st.have) a.rig.ApplyHeld(target, now, nowMs);   // and the owner's seat of the weapon in the right hand
    Record(s, st, now, target);
    if (stepObserver_) stepObserver_(entity, st, now, target, a.rig);
}

// As the mirror bench: the stand-in's body against the target it was servoed to; placement frames counted apart.
void StandIns::Record(Slot& s, const sync::DriverStep& st, const sync::FighterNow& now, const sync::PoseFrame& target) {
    Window& w = s.window;
    ++w.frames;
    w.struck += st.struck;
    if (!st.have) return;
    ++w.targeted;
    w.extrapolated += st.info.extrapolated;
    w.stale += st.info.stale;
    w.cuts += st.info.cut;
    w.delaySum += st.info.delayMs;
    w.j95Max = std::max(w.j95Max, st.info.j95Ms);
    if (st.place) { ++w.placed; w.snaps += st.snap; return; }
    if (st.pelvisErrUu >= 0.0) w.pelvis.Add(st.pelvisErrUu);
    for (int b : {sync::kBoneHandL, sync::kBoneHandR}) {
        const double e = sync::BoneError(now, target, b);
        if (e >= 0.0) w.hands.Add(e);
    }
}

std::string StandIns::HealthLine() {
    std::string out;
    char buf[512];
    for (size_t e = 1; e <= kMaxWillies; ++e) {
        Slot& s = slots_[e];
        if (!s.actor && !s.feed) continue;
        const Window& w = s.window;
        const double nt = w.targeted ? double(w.targeted) : 1.0;
        const unsigned long long poses = s.feed ? static_cast<unsigned long long>(s.feed->poses) : 0ull;
        const unsigned long long rejected = s.feed ? static_cast<unsigned long long>(s.feed->rejected) : 0ull;
        std::snprintf(buf, sizeof buf,
                      "%s e%zu%s: pelvis avg %.1f p99 %.1f max %.1f | hands avg %.1f max %.1f uu | delay %.0f ms j95 %.0f | extrap %.1f%% "
                      "stale %.1f%% | cuts %llu snaps %llu placed %llu struck %llu | poses %llu rejected %llu",
                      out.empty() ? "" : " ||", e, s.actor ? "" : " (no actor)", w.pelvis.Mean(), w.pelvis.Percentile(0.99), w.pelvis.Max(),
                      w.hands.Mean(), w.hands.Max(), w.delaySum / nt, w.j95Max, 100.0 * double(w.extrapolated) / nt,
                      100.0 * double(w.stale) / nt, static_cast<unsigned long long>(w.cuts), static_cast<unsigned long long>(w.snaps),
                      static_cast<unsigned long long>(w.placed), static_cast<unsigned long long>(w.struck), poses, rejected);
        out += buf;
        s.window = Window{};
    }
    if (out.empty()) return out;
    std::snprintf(buf, sizeof buf, " || stand-in cost avg %.3f max %.3f ms/frame", ticks_ ? costSumMs_ / double(ticks_) : 0.0, costMaxMs_);
    out += buf;
    costSumMs_ = costMaxMs_ = 0;
    ticks_ = 0;
    return out;
}

}  // namespace hsm::game
