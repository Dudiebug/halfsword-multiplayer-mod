#include "gameplay/standin_gore.h"
#include "combat/hsm_sever.h"
#include "log.h"
#include <cstdio>

namespace hsm::game {

using namespace hsm::ue;

namespace {

struct FObjArray { UObject** data = nullptr; int32_t num = 0, max = 0; };

constexpr uint64_t kLogFirst = 10;                  // the first fallbacks and dislocations are logged in full (test evidence)
constexpr uint8_t kPhysBodyTerm = 1;                // EPhysBodyOp::PBO_Term, the flag Willie_BP.cpp:3429 hides with
constexpr double kRetryMs = 500.0;                  // a call that failed waits this long before it is tried again
constexpr uint8_t kMaxTries = 4;                    // and is given up after this many failures
constexpr double kReassertMs = 2000.0;              // a hidden bone is checked still hidden this often (a rebuild resets it)
constexpr const char* kPassTag = "Dismember Pass";  // Willie_BP.cpp:3679, the children the cut hides bones on as well
constexpr int32_t kMaxBoneConstraints = 16;         // a bone has a handful of constraints; more is a misread array
// The bone each dislocatable joint (hsm_gore.h bit order) is freed at: the child of its constraint, where the game
// looks the constraints up (GetConstraintsFromBody, Willie_BP.cpp:8999, 9046, 9207, 9252, 9411, 9458) and parks the loose
// one (:12170-12300).
constexpr const char* kJointBone[sync::kGoreJoints] = { "upperarm_r", "upperarm_l", "calf_r", "calf_l", "neck_02", "spine_04" };
constexpr const char* kJointName[sync::kGoreJoints] = { "arm R", "arm L", "leg R", "leg L", "neck", "spine" };

}  // namespace

void StandInGore::Start(Adapter& adapter, Env env) {
    Stop();
    adapter_ = &adapter;
    env_ = std::move(env);
    active_ = true;
    HSM_LOG("gore: on (severed parts and dislocated joints the owners report are shown on their stand-ins)");
}

void StandInGore::Stop() {
    if (active_) HSM_LOG("gore: off (%s)", HealthLine().c_str());
    *this = StandInGore{};
}

void StandInGore::Clear() {
    for (Entry& e : entries_) e = Entry{};          // actors are gone with the level: records and trackers start over
}

void StandInGore::Forget(uint8_t entity) {
    if (entity == 0 || entity > kMaxWillies) return;
    entries_[entity] = Entry{};
    entries_[entity].tracker.AwaitWhole();          // the old body's samples still in flight must not reach the new one
}

// ---------------------------------------------------------------- the owner's reports

void StandInGore::OnVitals(uint8_t entity, uint32_t severed, uint32_t dislocated, double nowMs) {
    if (!active_ || entity == 0 || entity > kMaxWillies) return;
    Entry& e = entries_[entity];
    ++n_.vitals;
    Bind(entity, e);                                // a new actor first: its wait must see this sample
    const sync::GoreActions a = e.tracker.OnVitals(severed, dislocated, nowMs);
    if (a.reset) ++n_.resets;                       // a fresh body, or a reordered sample: the record keeps repeats harmless
    e.owed.Apply(a);
    for (unsigned j = 0; j < sync::kGoreJoints; ++j)
        if ((a.relocate >> j) & 1u) Relocate(entity, j);
    Reconcile(entity, e, nowMs);
}

void StandInGore::NoteNativeSever(uint8_t entity, uint8_t part) {
    if (!active_ || entity == 0 || entity > kMaxWillies || part >= sync::kGoreSeveredBits) return;
    Entry& e = entries_[entity];
    UObject* w = Bind(entity, e);                   // before the note: a new actor's reset must not wipe it
    e.tracker.NoteNativeSever(part);
    ++n_.native;
    if (w) e.rec.did.native |= 1u << part;
}

void StandInGore::Tick(double nowMs) {
    if (!active_) return;
    for (size_t entity = 1; entity <= kMaxWillies; ++entity) {
        Entry& e = entries_[entity];
        Bind(uint8_t(entity), e);                   // every frame, so a new actor is noticed before anything is owed to it
        e.owed.Apply(e.tracker.Tick(nowMs));
        Reconcile(uint8_t(entity), e, nowMs);
    }
}

uint32_t StandInGore::DeadBones(uint8_t entity) const {
    if (!active_ || entity == 0 || entity > kMaxWillies) return 0;
    const Record& r = entries_[entity].rec;
    if (!r.willie || !env_.standInActor || env_.standInActor(entity) != r.willie) return 0;   // another actor's record
    return r.did.DeadBones();
}

// ---------------------------------------------------------------- applying

// The entity's live stand-in. A new actor (first attach, respawn, new level, rebuild) gets a new record - what was hidden
// or freed on the old actor says nothing about this one - and nothing owed: what was owed may be the old body's, so the
// tracker waits for a whole sample (see the header). Gore its owner still reports after that comes back as usual.
UObject* StandInGore::Bind(uint8_t entity, Entry& e) {
    UObject* w = env_.standInActor ? env_.standInActor(entity) : nullptr;
    if (!w) return nullptr;
    const int32_t index = ObjectIndex(w);
    if (e.rec.willie != w || e.rec.index != index) {
        if (e.rec.willie) ++n_.swaps;
        if (++n_.actors <= kLogFirst) HSM_LOG("gore: e%u has a new stand-in: its gore waits for a whole report first", entity);
        e.rec = Record{};
        e.rec.willie = w;
        e.rec.index = index;
        e.owed = sync::GoreOwed{};
        e.tracker.AwaitWhole();
    }
    return w;
}

void StandInGore::Reconcile(uint8_t entity, Entry& e, double nowMs) {
    if (!e.owed.due && !e.owed.dislocated && !e.rec.did.hidden) return;
    UObject* w = Bind(entity, e);
    if (!w) return;                                  // no stand-in yet: what is due waits for one
    Record& r = e.rec;
    if (nowMs >= r.retryAtMs) {
        const uint32_t parts = r.did.PartsToShow(e.owed.due);
        for (uint8_t part = 0; part < sync::kGoreSeveredBits; ++part) {
            const uint32_t bit = 1u << part;
            if (!(parts & bit)) continue;
            const Outcome o = ShowSeverFallback(w, part);
            if (o == Outcome::Done) {
                r.did.hidden |= bit;
                if (++n_.fallbacks <= kLogFirst)
                    HSM_LOG("gore: e%u part %u: no game cut arrived, hid %s and disabled the bodies below it", entity, part,
                            combat::SeverSpawnBone(part));
            } else if (o == Outcome::Nothing) {
                r.did.gaveUp |= bit;
                if (++n_.nothing <= kLogFirst) HSM_LOG("gore: e%u part %u: nothing to hide for it (the pelvis stays)", entity, part);
            } else if (++r.tries[part] >= kMaxTries) {
                r.did.gaveUp |= bit;
                ++n_.failed;
                HSM_LOG("gore: e%u part %u: gave up (no mesh or no HideBoneByName)", entity, part);
            } else {
                r.retryAtMs = nowMs + kRetryMs;
            }
        }
        const uint32_t joints = r.did.JointsToFree(e.owed.dislocated);
        for (unsigned joint = 0; joint < sync::kGoreJoints; ++joint) {
            const uint32_t bit = 1u << joint;
            if (!(joints & bit)) continue;
            const int32_t made = Dislocate(w, joint);
            if (made > 0) {
                r.did.freed |= bit;
                if (++n_.dislocated <= kLogFirst)
                    HSM_LOG("gore: e%u %s dislocated: %d constraint(s) at %s made breakable at 0", entity, kJointName[joint], made,
                            kJointBone[joint]);
            } else if (++r.tries[sync::kGoreSeveredBits + joint] >= kMaxTries) {
                r.did.jointsGaveUp |= bit;
                ++n_.failed;
                HSM_LOG("gore: e%u %s: gave up freeing the joint at %s (%s)", entity, kJointName[joint], kJointBone[joint],
                        made == 0 ? "no live constraint on that bone" : "no mesh or no constraint calls");
            } else {
                r.retryAtMs = nowMs + kRetryMs;
            }
        }
    }
    if (r.did.hidden && nowMs >= r.checkAtMs) {
        r.checkAtMs = nowMs + kReassertMs;
        Reassert(w, r.did.hidden);
    }
}

// Hides the part's Spawn Bone subtree on the stand-in. The bodies below it are physics-disabled first, while they are
// all still valid (the servo then has nothing to carry there and the hidden limb cannot be struck); then the bone is
// hidden with PBO_Term on the Mesh and on the children the game's own cut hides it on (followers of the Mesh's pose
// take its bone visibility, but the game does not rely on that and neither does this). Repeating it is harmless.
StandInGore::Outcome StandInGore::ShowSeverFallback(UObject* willie, uint8_t part) {
    if (part >= sync::kGoreSeveredBits || sync::kGoreFallbackRoot[part] < 0) return Outcome::Nothing;
    const char* bone = combat::SeverSpawnBone(part);
    UObject* mesh = adapter_ ? adapter_->Mesh(willie) : nullptr;
    if (!bone || !mesh) return Outcome::Failed;
    const FName name = NameOf(bone);
    Call below(mesh, "SetAllBodiesBelowPhysicsDisabled");
    if (below.valid()) below.setName("InBoneName", name).set<bool>("bDisabled", true).set<bool>("bIncludeSelf", true).invoke();
    if (!HideBone(mesh, name)) return Outcome::Failed;
    for (UObject* child : PassChildren(mesh)) HideBone(child, name);
    if (part <= 1) Call(willie, "Destroy Hair").invoke();   // the hair is attached to the head bone, which is hidden
    return Outcome::Done;
}

// Willie_BP.cpp:8998-9012 (and the same for the other five joints): GetConstraintsFromBody(bone, true, false, false), then
// SetLinearBreakable(accessor, true, 0) and SetAngularBreakable(accessor, true, 0) for each. The constraints made
// breakable, 0 when the bone has no live one, -1 when a call or the mesh is missing.
int32_t StandInGore::Dislocate(UObject* willie, unsigned joint) {
    UObject* mesh = adapter_ && joint < sync::kGoreJoints ? adapter_->Mesh(willie) : nullptr;
    if (!constraintLib_)
        if (UClass* lib = FindClass("ConstraintInstanceBlueprintLibrary")) constraintLib_ = DefaultObject(lib);
    if (!mesh || !constraintLib_) return -1;
    Call get(mesh, "GetConstraintsFromBody");
    Call linear(constraintLib_, "SetLinearBreakable");
    Call angular(constraintLib_, "SetAngularBreakable");
    FProperty* outProp = get.valid() ? FindProperty(get.function(), "OutConstraints") : nullptr;
    FProperty* accessor = linear.valid() ? FindProperty(linear.function(), "Accessor") : nullptr;
    const int32_t stride = accessor ? PropertySize(accessor) : 0;
    if (!outProp || stride <= 0 || !angular.valid()) return -1;
    get.setName("BodyName", NameOf(kJointBone[joint]))
        .set<bool>("bParentConstraints", true)
        .set<bool>("bChildConstraints", false)
        .set<bool>("bIncludesTerminated", false);
    if (!get.invoke()) return -1;
    void* out = get.paramPtr("OutConstraints");
    const FByteArray* arr = static_cast<const FByteArray*>(out);
    int32_t made = 0;
    if (arr->data && arr->num > 0 && arr->num <= kMaxBoneConstraints) {
        for (int32_t i = 0; i < arr->num; ++i) {
            const uint8_t* item = arr->data + size_t(i) * size_t(stride);
            Call(constraintLib_, "SetLinearBreakable").setFrom("Accessor", item).set<bool>("bLinearBreakable", true)
                .setReal("LinearBreakThreshold", 0.0).invoke();
            Call(constraintLib_, "SetAngularBreakable").setFrom("Accessor", item).set<bool>("bAngularBreakable", true)
                .setReal("AngularBreakThreshold", 0.0).invoke();
            ++made;
        }
    }
    ClearPropertyValue(outProp, out);               // the engine filled the array: its own ClearValue releases it
    return made;
}

// Nothing to undo (see the header): the record keeps the joint as freed, so a flip back to dislocated is not freed again.
void StandInGore::Relocate(uint8_t entity, unsigned joint) {
    if (++n_.relocated <= kLogFirst) HSM_LOG("gore: e%u %s no longer dislocated (the freed joint stays freed)", entity, kJointName[joint]);
}

void StandInGore::Reassert(UObject* willie, uint32_t parts) {
    UObject* mesh = adapter_ ? adapter_->Mesh(willie) : nullptr;
    if (!mesh) return;
    for (uint8_t part = 0; part < sync::kGoreSeveredBits; ++part) {
        if (!((parts >> part) & 1u)) continue;
        const char* bone = combat::SeverSpawnBone(part);
        if (!bone || BoneHidden(mesh, NameOf(bone))) continue;
        ShowSeverFallback(willie, part);
        ++n_.reasserted;
    }
}

bool StandInGore::HideBone(UObject* component, FName bone) {
    Call hide(component, "HideBoneByName");
    if (!hide.valid()) return false;
    hide.setName("BoneName", bone).set<uint8_t>("PhysBodyOption", kPhysBodyTerm).invoke();
    return true;
}

bool StandInGore::BoneHidden(UObject* mesh, FName bone) {
    Call hidden(mesh, "IsBoneHiddenByName");
    hidden.setName("BoneName", bone);
    return hidden.invoke() && hidden.get<bool>("ReturnValue");
}

// Willie_BP.cpp:3678-3690: GetChildrenComponents(false) of the master mesh, kept when ComponentHasTag('Dismember Pass')
// and a SkeletalMeshComponent (here: one that has HideBoneByName).
std::vector<UObject*> StandInGore::PassChildren(UObject* mesh) {
    std::vector<UObject*> out;
    Call kids(mesh, "GetChildrenComponents");
    kids.set<bool>("bIncludeAllDescendants", false);
    if (!kids.invoke()) return out;
    const FObjArray* arr = static_cast<const FObjArray*>(kids.paramPtr("Children"));
    if (!arr || !arr->data || arr->num <= 0 || arr->num > 256) return out;
    for (int32_t i = 0; i < arr->num; ++i) {
        UObject* child = arr->data[i];
        if (!child) continue;
        Call tagged(child, "ComponentHasTag");
        tagged.setName("Tag", NameOf(kPassTag));
        if (tagged.invoke() && tagged.get<bool>("ReturnValue") && Call(child, "HideBoneByName").valid()) out.push_back(child);
    }
    return out;
}

FName StandInGore::NameOf(const std::string& bone) {
    if (auto it = names_.find(bone); it != names_.end()) return it->second;
    const FName n = MakeName(bone);
    if (names_.size() < 64) names_[bone] = n;       // a few dozen identifiers ever: the bone table and the tag
    return n;
}

std::string StandInGore::HealthLine() {
    const Counters& a = n_;
    const Counters& b = reported_;
    auto d = [](uint64_t now, uint64_t before) { return static_cast<unsigned long long>(now - before); };
    char line[320];
    std::snprintf(line, sizeof line,
                  "gore: vitals %llu (resets %llu) | fallbacks %llu (nothing to hide %llu reasserted %llu failed %llu) "
                  "game cuts noted %llu | joints freed %llu (back to normal %llu) | new stand-ins %llu (rebuilt %llu)",
                  d(a.vitals, b.vitals), d(a.resets, b.resets), d(a.fallbacks, b.fallbacks), d(a.nothing, b.nothing),
                  d(a.reasserted, b.reasserted), d(a.failed, b.failed), d(a.native, b.native), d(a.dislocated, b.dislocated),
                  d(a.relocated, b.relocated), d(a.actors, b.actors), d(a.swaps, b.swaps));
    reported_ = n_;
    return line;
}

}  // namespace hsm::game
