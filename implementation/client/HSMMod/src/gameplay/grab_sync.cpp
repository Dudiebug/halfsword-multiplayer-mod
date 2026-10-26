#include "gameplay/grab_sync.h"
#include "combat/hsm_claim.h"
#include "gameplay/owner_sync.h"
#include "log.h"
#include <cmath>
#include <cstdio>

namespace hsm::game {

using namespace hsm::ue;
using combat::Xform;
using sync::V3;

namespace {

constexpr uint64_t kLogFirst = 10;                  // the first grabs, holds and releases are logged in full (test evidence)
constexpr size_t kMaxNames = 128;                   // bone names cached: the 22 bodies seeded, then what peers name (identifiers only)
constexpr const char* kHandBone[kGrabHands] = { "hand_r", "hand_l" };   // the grip is measured from, and pulled to, these
constexpr const char* kHandTag[kGrabHands] = { "R", "L" };

V3 ToV(const FVector& v) { return V3{ v.x, v.y, v.z }; }
FVector ToF(const V3& v) { return FVector{ v.x, v.y, v.z }; }
double Dist(const V3& a, const V3& b) { return std::sqrt((a.x - b.x) * (a.x - b.x) + (a.y - b.y) * (a.y - b.y) + (a.z - b.z) * (a.z - b.z)); }

Xform ToXform(const FTransform& t) {
    Xform x;
    x.pos = ToV(t.translation);
    x.rot = sync::Q4{ t.rotation.x, t.rotation.y, t.rotation.z, t.rotation.w };
    x.scale = ToV(t.scale);
    return x;
}

}  // namespace

void GrabSync::Start(Adapter& adapter, Env env) {
    Stop();
    adapter_ = &adapter;
    env_ = std::move(env);
    active_ = true;
    HSM_LOG("grabs: on (holds on stand-ins are streamed; holds on my fighters pull them)");
}

void GrabSync::Stop() {
    if (active_) {
        std::vector<uint8_t> marked;
        for (const auto& [e, m] : marks_) marked.push_back(e);
        for (uint8_t e : marked) Unmark(e);
        HSM_LOG("grabs: off (%s)", HealthLine().c_str());
    }
    *this = GrabSync{};
}

void GrabSync::Clear() {
    inbox_.Clear();
    for (Grabber& g : grabbers_) { g.sender.Reset(); g.willie = nullptr; g.live = false; }   // seq keeps counting
    marks_.clear();
    lastGrab_ = {};
    heldBones_ = {};
}

void GrabSync::Forget(uint8_t entity) {
    if (entity == 0 || entity > kMaxWillies) return;
    inbox_.Forget(entity, SteadyNowMs());                   // its old grabs stay retired a while (same clock as Tick)
    inbox_.DropTarget(entity);                              // holds named on its old body die with it
    lastGrab_[entity] = {};
    grabbers_[entity].sender.Reset(); grabbers_[entity].willie = nullptr; grabbers_[entity].live = false;
    marks_.erase(entity);
}

void GrabSync::OnGrabState(const GrabStateMsg& m, double nowMs) {
    if (!active_) return;
    if (inbox_.Accept(m, nowMs)) ++n_.heard;
    else ++n_.stale;
}

void GrabSync::Tick(double nowMs, float dt, const std::vector<std::pair<uint8_t, UObject*>>& owned) {
    if (!active_) return;
    // Grabber side: what each fighter simulated here holds. A replaced or vanished fighter lets go first.
    std::array<UObject*, kMaxWillies + 1> mine{};
    heldBones_ = {};
    for (const auto& [e, w] : owned) {
        if (e == 0 || e > kMaxWillies || !w) continue;
        mine[e] = w;
        Grabber& g = grabbers_[e];
        if (!g.live || g.willie != w) inbox_.DropTarget(e);   // a body new to me: what is held of e so far was an earlier one
        if (g.live && g.willie != w) { SendFor(e, {}, nowMs); g.sender.Reset(); }
        g.willie = w; g.live = true;
        SendFor(e, ReadHands(w), nowMs);
    }
    for (size_t e = 1; e <= kMaxWillies; ++e)
        if (grabbers_[e].live && !mine[e]) {
            SendFor(uint8_t(e), {}, nowMs);
            grabbers_[e].sender.Reset(); grabbers_[e].willie = nullptr; grabbers_[e].live = false;
        }

    // Held side: pull every hold on a fighter of mine; a fighter no longer pulled is unmarked.
    std::array<bool, kMaxWillies + 1> held{};
    const auto isMine = [&](uint8_t e) { return e != 0 && e <= kMaxWillies && mine[e] != nullptr; };
    const auto dead = [this](uint8_t e) { return env_.dead && env_.dead(e); };
    std::array<int8_t, kMaxWillies + 1> down{};   // -1 unknown yet this tick, read once per held fighter
    down.fill(-1);
    for (const sync::GrabPull& p : inbox_.Pulls(nowMs, isMine, dead)) {
        if (down[p.target] < 0) down[p.target] = DownHere(p.target, mine[p.target]) ? 1 : 0;
        if (!Pull(p, mine[p.target], down[p.target] == 1, nowMs, dt)) continue;
        const bool fresh = lastGrab_[p.grabber][p.hand] != p.grabId;
        lastGrab_[p.grabber][p.hand] = p.grabId;
        if (fresh && n_.grabs < kLogFirst)
            HSM_LOG("grabs: e%u held by e%u (hand %s, bone %s, grab %u%s)", p.target, p.grabber, kHandTag[p.hand], p.bone.c_str(), p.grabId,
                    down[p.target] == 1 ? ", a down body: carried" : "");
        SetMark(p.target, mine[p.target], env_.standInActor ? env_.standInActor(p.grabber) : nullptr, fresh);
        held[p.target] = true;
    }
    std::vector<uint8_t> loose;
    for (const auto& [e, m] : marks_) if (!held[e]) loose.push_back(e);
    for (uint8_t e : loose) Unmark(e);
}

// ---------------------------------------------------------------- grabber side

void GrabSync::SendFor(uint8_t entity, const std::array<GrabHand, kGrabHands>& reads, double nowMs) {
    GrabStateMsg out;
    const sync::GrabSend s = grabbers_[entity].sender.Update(entity, reads, nowMs, out);
    if (s == sync::GrabSend::None) return;
    if (s == sync::GrabSend::Change) {
        if (++n_.changes <= kLogFirst)
            for (size_t i = 0; i < kGrabHands; ++i)
                HSM_LOG("grabs: e%u hand %s %s", entity, kHandTag[i],
                        out.hands[i].held ? ("holds e" + std::to_string(out.hands[i].target) + " " + out.hands[i].bone + " (grab " +
                                             std::to_string(out.hands[i].grabId) + ")").c_str() : "free");
    } else {
        ++n_.refreshes;
    }
    if (env_.send) env_.send(Encode(out), s == sync::GrabSend::Change);
}

// The owned fighter's native holds on stand-ins: "Grabbed R/L", what it holds ("Grab Component R/L", the held
// fighter's Mesh) and where ("Grab Bone R/L"), as Willie_BP ReceiveTick sets them. Holds on anything else (props,
// weapons, fighters simulated here, another component of a stand-in) are not streamed.
std::array<GrabHand, kGrabHands> GrabSync::ReadHands(UObject* willie) {
    std::array<GrabHand, kGrabHands> out{};
    UObject* mesh = adapter_->Mesh(willie);
    if (!mesh || !Ready(mesh)) return out;
    for (size_t i = 0; i < kGrabHands; ++i) {
        bool grabbed = false;
        if (!(i == 0 ? GetBool(willie, "Grabbed R", grabbed) : GetBool(willie, "Grabbed L", grabbed)) || !grabbed) continue;
        UObject** comp = i == 0 ? Field<UObject*>(willie, "Grab Component R") : Field<UObject*>(willie, "Grab Component L");
        FName* bone = i == 0 ? Field<FName>(willie, "Grab Bone R") : Field<FName>(willie, "Grab Bone L");
        if (!comp || !*comp || !bone) continue;
        UObject* standIn = OuterOf(*comp);
        const uint8_t target = standIn && env_.standInEntity ? env_.standInEntity(standIn) : 0;
        if (!target || adapter_->Mesh(standIn) != *comp) continue;
        const std::string name = NameToString(*bone);
        FTransform b, hand;
        if (name.empty() || name == "None" || !BoneWorld(*comp, *bone, b) || !BoneWorld(mesh, hands_[i], hand)) continue;
        out[i].held = true;
        out[i].target = target;
        out[i].bone = name;
        out[i].grip = ToXform(b).ToLocal(ToV(hand.translation));   // the sender clamps it to kGripMaxUu
        heldBones_[target] |= sync::YieldBones(sync::PoseBoneIndex(name), env_.down && env_.down(target));
    }
    return out;
}

// ---------------------------------------------------------------- held side

// One step of one hold: the grip on my bone toward the grabber's stand-in hand (both read, the stand-in untouched),
// a velocity change added to that body. False when nothing could be read, or the hold just broke free.
bool GrabSync::DownHere(uint8_t entity, UObject* me) {
    VitalsMsg v;
    return me && adapter_->ReadVitals(me, entity, v) && ((v.flags & 7u) != 0 || v.health <= 0.f);
}

// A down body (dead, fallen, knocked out) is carried: the stiffer DownPullParams, part of the pull on its pelvis and
// chest as well, and it breaks free only on a glitch (kGrabDownBreakUu/Ms).
bool GrabSync::Pull(const sync::GrabPull& p, UObject* me, bool down, double nowMs, float dt) {
    UObject* mesh = me ? adapter_->Mesh(me) : nullptr;
    if (!mesh || !Ready(mesh)) return false;
    UObject* standIn = env_.standInActor ? env_.standInActor(p.grabber) : nullptr;
    UObject* handMesh = standIn ? adapter_->Mesh(standIn) : nullptr;
    if (!handMesh) { ++n_.noAnchor; return false; }
    const FName bone = NameOf(p.bone);
    boneIndex_.put(0, bone);
    FTransform b, a;
    if (!boneIndex_.invoke(mesh) || boneIndex_.take<int32_t>(1) < 0 || !BoneWorld(mesh, bone, b)) {
        if (++n_.noBone <= 3) HSM_LOG("grabs: e%u holds bone '%s', which my fighter lacks", p.grabber, p.bone.c_str());
        return false;
    }
    if (!BoneWorld(handMesh, hands_[p.hand], a)) { ++n_.noAnchor; return false; }
    const V3 grip = ToXform(b).FromLocal(p.grip);
    velAt_.put(0, ToF(grip)).put(1, bone);
    vel_.put(0, hands_[p.hand]);
    if (!velAt_.invoke(mesh) || !vel_.invoke(handMesh)) { ++n_.noAnchor; return false; }
    const V3 handVel = ToV(vel_.take<FVector>(1));
    const V3 hand = sync::GrabAnchor(ToV(a.translation), handVel);
    const double dist = Dist(grip, hand);
    if (down ? inbox_.NoteDistance(p, dist, nowMs, sync::kGrabDownBreakUu, sync::kGrabDownBreakMs) : inbox_.NoteDistance(p, dist, nowMs)) {
        if (++n_.broke <= kLogFirst) HSM_LOG("grabs: e%u broke free of e%u's hand %s (%.0f cm)", p.target, p.grabber, kHandTag[p.hand], dist);
        return false;
    }
    const V3 dv = sync::GrabPullDv(grip, ToV(velAt_.take<FVector>(2)), hand, handVel, dt, down ? sync::DownPullParams() : sync::GrabPullParams{});
    addVel_.put(0, ToF(dv)).put(1, true).put(2, bone);   // bAddToCurrent: the change, on that body only
    if (addVel_.invoke(mesh)) ++n_.pulls;
    if (down) {
        const V3 share{dv.x * sync::kGrabDownSpread, dv.y * sync::kGrabDownSpread, dv.z * sync::kGrabDownSpread};
        for (const char* torso : {"pelvis", "spine_03"}) {
            if (p.bone == torso) continue;
            addVel_.put(0, ToF(share)).put(1, true).put(2, NameOf(torso));
            addVel_.invoke(mesh);
        }
    }
    return true;
}

// My fighter as a native grab leaves it (Willie_BP ReceiveTick, the grabber's side of a grab): "Grabbed By Willie" (its
// contacts with the grabber then deal no rigidity damage), "Being Grabbed" (a fallen body's root handle and tonus
// weaken), and "Event Is Grabbed" on every new grab (weapons let go of for 0.1 s, limb joints unbreakable for 0.5 s).
// A fighter simulated here that grabbed mine natively has set those itself and clears them on its release: while
// "Being Grabbed" is set and "Grabbed By Willie" names something other than what my last mark wrote or a stand-in, the
// flags are its own, and only the event (a new grab on top of it) and the pull are mine.
void GrabSync::SetMark(uint8_t entity, UObject* willie, UObject* by, bool newGrab) {
    auto it = marks_.find(entity);
    const bool first = it == marks_.end() || it->second.willie != willie;
    if (!first && !newGrab) return;
    bool being = false;
    GetBool(willie, "Being Grabbed", being);
    UObject** holder = Field<UObject*>(willie, "Grabbed By Willie");
    UObject* cur = holder ? *holder : nullptr;
    const bool native = being && cur && (first || cur != it->second.by) && !(env_.standInEntity && env_.standInEntity(cur));
    if (first) marks_[entity] = Mark{ willie, ObjectIndex(willie), by, native };
    else { it->second.by = by; it->second.native = native; }   // what "Grabbed By Willie" holds, for Unmark
    ++n_.grabs;
    if (!native) {
        if (by && holder) *holder = by;
        SetBool(willie, "Being Grabbed", true);
    }
    Call(willie, "Event Is Grabbed").set<bool>("Disable Weapons Momentary", true).invoke();
}

// Released as Release Grab R/L does it ("Being Grabbed" false, "Event Is Grabbed(false)"), unless a fighter simulated
// here has grabbed mine natively (before or since): that grab is its own to release.
void GrabSync::Unmark(uint8_t entity) {
    auto it = marks_.find(entity);
    if (it == marks_.end()) return;
    const Mark m = it->second;
    marks_.erase(it);
    if (++n_.released <= kLogFirst) HSM_LOG("grabs: e%u released", entity);
    if (m.native || !adapter_ || !m.willie || !adapter_->StillValid(m.willie, m.index)) return;
    UObject** by = Field<UObject*>(m.willie, "Grabbed By Willie");
    if (by && *by != m.by) return;
    SetBool(m.willie, "Being Grabbed", false);
    Call(m.willie, "Event Is Grabbed").set<bool>("Disable Weapons Momentary", false).invoke();
}

// ---------------------------------------------------------------- engine calls

bool GrabSync::Ready(UObject* mesh) {
    if (ready_ || readyFailed_ || !mesh) return ready_;
    const UClass* mc = ClassOf(mesh);
    socket_ = PreparedCall(mc, "GetSocketTransform", {"InSocketName", "TransformSpace", "ReturnValue"});
    boneIndex_ = PreparedCall(mc, "GetBoneIndex", {"BoneName", "ReturnValue"});
    velAt_ = PreparedCall(mc, "GetPhysicsLinearVelocityAtPoint", {"Point", "BoneName", "ReturnValue"});
    vel_ = PreparedCall(mc, "GetPhysicsLinearVelocity", {"BoneName", "ReturnValue"});
    addVel_ = PreparedCall(mc, "SetPhysicsLinearVelocity", {"NewVel", "bAddToCurrent", "BoneName"});
    for (size_t i = 0; i < kGrabHands; ++i) hands_[i] = MakeName(kHandBone[i]);
    for (int i = 0; i < sync::kPoseBones; ++i)   // the bodies a hold can name: always cached, whatever a peer sends after
        if (sync::kPoseHasBody[i]) names_[sync::kPoseBoneNames[i]] = MakeName(sync::kPoseBoneNames[i]);
    ready_ = socket_.valid() && boneIndex_.valid() && velAt_.valid() && vel_.valid() && addVel_.valid();
    readyFailed_ = !ready_;
    if (!ready_) HSM_LOG("grabs: per-frame engine calls did not resolve on %s; grabs stay local", ObjectName(mesh).c_str());
    return ready_;
}

bool GrabSync::BoneWorld(UObject* mesh, FName bone, FTransform& out) {
    socket_.put(0, bone);   // TransformSpace is never written: the zeroed buffer holds RTS_World (0)
    if (!socket_.invoke(mesh)) return false;
    out = socket_.take<FTransform>(2);
    return std::isfinite(out.translation.x) && std::isfinite(out.translation.y) && std::isfinite(out.translation.z)
        && std::isfinite(out.rotation.w);
}

FName GrabSync::NameOf(const std::string& bone) {
    if (auto it = names_.find(bone); it != names_.end()) return it->second;
    if (names_.size() >= kMaxNames) return FName{};   // None: GetBoneIndex fails, the hold is not pulled (bounds what peers can intern)
    return names_[bone] = MakeName(bone);
}

std::string GrabSync::HealthLine() {
    const Counters& a = n_;
    const Counters& b = reported_;
    auto d = [](uint64_t now, uint64_t before) { return static_cast<unsigned long long>(now - before); };
    char line[320];
    std::snprintf(line, sizeof line,
                  "grabs: sent %llu changes %llu refreshes | heard %llu (stale %llu) | held %llu grabs, %llu pull steps, "
                  "%llu released, %llu broke free (no anchor %llu, no bone %llu)",
                  d(a.changes, b.changes), d(a.refreshes, b.refreshes), d(a.heard, b.heard), d(a.stale, b.stale), d(a.grabs, b.grabs),
                  d(a.pulls, b.pulls), d(a.released, b.released), d(a.broke, b.broke), d(a.noAnchor, b.noAnchor), d(a.noBone, b.noBone));
    reported_ = n_;
    return line;
}

}  // namespace hsm::game
