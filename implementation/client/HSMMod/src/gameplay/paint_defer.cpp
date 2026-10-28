#include "gameplay/paint_defer.h"
#include "combat/hsm_paintdefer.h"
#include "log.h"
#include "ue/engine.h"
#include "ue/script_hook.h"
#include <cstdio>
#include <cstring>
#include <map>
#include <vector>

namespace hsm::game {

double SteadyNowMs();

using namespace hsm::ue;

namespace {

constexpr const char* kOwner = "paintdefer";
constexpr const char* kCallback = "BndEvt__Willie_BP_BPC_RuntimeVertexPaintAndDetectionComponent_K2Node_ComponentBoundEvent_0_VertexColorGetAllVertexColorsOnly__DelegateSignature";
constexpr uint32_t kLogFirst = 40;       // deferrals and releases logged in full (test evidence), then every 25th
constexpr int kMaxFailures = 3;          // decoy / lookup failures before the feature stays off for the run

struct FObjArray { UObject** data = nullptr; int32_t num = 0, max = 0; };   // TArray<UObject*> as the engine lays it out

struct Parked {
    UObject* comp = nullptr; int32_t compIndex = -1;
    UObject* owner = nullptr; int32_t ownerIndex = -1;
    UObject* paint = nullptr; int32_t paintIndex = -1;
};

// The callback's result struct (RVPDPTaskResults), resolved once per level.
struct ResultLayout {
    bool tried = false, ok = false;
    int32_t meshOffset = -1, paintOffset = -1;
};

struct State {
    bool installed = false;
    bool off = false;                    // too many failures: the game destroys pieces as before
    int failures = 0;
    ResultLayout layout;
    UClass* skeletalClass = nullptr;
    combat::PaintDefer queue;
    std::map<uint64_t, Parked> parked;   // key: the piece's object index
    // The result struct slot being borrowed during a deferred callback, restored by the post-hook.
    struct Borrow { UObject** slot = nullptr; UObject* original = nullptr; };
    std::vector<Borrow> borrowed;
    struct Counters {
        uint64_t seen = 0, notSwap = 0, notBusy = 0, deferred = 0, released = 0, timedOut = 0, gone = 0, refused = 0, failed = 0;
        double waitedMs = 0, maxWaitMs = 0;
    } n, reported;
    uint32_t logged = 0;
} g;

bool Live(UObject* o, int32_t index) {
    return o && index >= 0 && index < ObjectCount() && ObjectAt(index) == o && !IsGarbage(o);
}

bool ResolveLayout() {
    ResultLayout& l = g.layout;
    if (l.tried) return l.ok;
    l.tried = true;
    UObject* results = FindStruct("RVPDPTaskResults");
    FProperty* mesh = results ? StructMember(results, "MeshComponent") : nullptr;
    FProperty* paint = results ? StructMember(results, "AssociatedPaintComponent") : nullptr;
    if (mesh && paint && PropertyType(mesh) == "ObjectProperty" && PropertyType(paint) == "ObjectProperty" &&
        PropertySize(mesh) == 8 && PropertySize(paint) == 8) {
        l.meshOffset = PropertyOffset(mesh);
        l.paintOffset = PropertyOffset(paint);
        l.ok = true;
    }
    HSM_LOG("paintdefer: result struct %s (mesh at +%d, paint component at +%d)", l.ok ? "ok" : "NOT FOUND; pieces are destroyed as before",
            l.meshOffset, l.paintOffset);
    return l.ok;
}

// Do the paint component's running or queued tasks reference mesh? -1: cannot tell (call missing or failed).
int TasksReference(UObject* paint, UObject* mesh) {
    Call get(paint, "GetCurrentTasksMeshComponents");
    if (!get.valid() || !get.invoke()) return -1;
    auto* list = static_cast<FObjArray*>(get.paramPtr("ReturnValue"));
    FProperty* prop = FindProperty(get.function(), "ReturnValue");
    if (!list || !prop || PropertyType(prop) != "ArrayProperty" || PropertySize(prop) != int32_t(sizeof(FObjArray))) return -1;
    bool found = false;
    if (list->data && list->num > 0 && list->num <= (1 << 20))
        for (int32_t i = 0; i < list->num && !found; ++i) found = list->data[i] == mesh;
    ClearPropertyValue(prop, list);   // the engine allocated the list; release it
    return found ? 1 : 0;
}

bool Flag(UObject* o, const char* name) {
    bool v = false;
    return GetBool(o, name, v) && v;
}

// A hidden stand-in for the piece in the callback's result struct. It must be a skeletal mesh component of the same
// asset (the callback keys its paint record by GetSkeletalMeshAsset and destroys what it is given) and must not do
// anything while it exists: no collision, not visible.
UObject* MakeDecoy(UObject* owner, UObject* asset) {
    if (!g.skeletalClass) g.skeletalClass = FindClass("SkeletalMeshComponent");
    if (!g.skeletalClass) return nullptr;
    FTransform identity{};
    identity.rotation = FQuat{0, 0, 0, 1};
    identity.scale = FVector{1, 1, 1};
    Call add(owner, "AddComponentByClass");
    if (!add.valid()) return nullptr;
    add.set<UClass*>("Class", g.skeletalClass).set<bool>("bManualAttachment", true).set<FTransform>("RelativeTransform", identity)
        .set<bool>("bDeferredFinish", false);
    if (!add.invoke()) return nullptr;
    UObject* decoy = add.get<UObject*>("ReturnValue");
    if (!decoy) return nullptr;
    auto discard = [&] { Call kill(decoy, "K2_DestroyComponent"); kill.set<UObject*>("Object", owner); if (kill.valid()) kill.invoke(); };
    Call collide(decoy, "SetCollisionEnabled");
    Call hide(decoy, "SetVisibility");
    if (!collide.valid() || !hide.valid()) { discard(); return nullptr; }
    collide.set<uint8_t>("NewType", 0).invoke();                           // NoCollision, before a mesh could build bodies
    hide.set<bool>("bNewVisibility", false).set<bool>("bPropagateToChildren", false).invoke();
    Call set(decoy, "SetSkeletalMeshAsset");
    if (!set.valid() || !set.set<UObject*>("NewMesh", asset).invoke()) { discard(); return nullptr; }
    Call check(decoy, "GetSkeletalMeshAsset");
    if (!check.valid() || !check.invoke() || check.get<UObject*>("ReturnValue") != asset) { discard(); return nullptr; }
    return decoy;
}

// The callback's own Array_RemoveItem(Worn Armor, piece), done here for the real piece (the decoy is not in the list).
bool RemoveFromWornArmor(UObject* owner, UObject* piece) {
    FProperty* p = FindProperty(ClassOf(owner), "Worn Armor");
    if (!p || PropertyType(p) != "ArrayProperty" || PropertySize(p) != int32_t(sizeof(FObjArray))) return false;
    auto* list = reinterpret_cast<FObjArray*>(reinterpret_cast<uint8_t*>(owner) + PropertyOffset(p));
    if (!list->data || list->num <= 0 || list->num > (1 << 20)) return false;
    bool removed = false;
    for (int32_t i = 0; i < list->num;) {
        if (list->data[i] == piece) {
            std::memmove(list->data + i, list->data + i + 1, size_t(list->num - i - 1) * sizeof(UObject*));
            --list->num;
            removed = true;
        } else ++i;
    }
    return removed;
}

void Hide(UObject* piece) {
    Call hide(piece, "SetVisibility");
    if (hide.valid()) hide.set<bool>("bNewVisibility", false).set<bool>("bPropagateToChildren", true).invoke();
    Call collide(piece, "SetCollisionEnabled");
    if (collide.valid()) collide.set<uint8_t>("NewType", 0).invoke();
}

bool LogThis() { return g.logged++ < kLogFirst || g.logged % 25 == 0; }

void Fail(const char* why) {
    ++g.n.failed;
    HSM_LOG("paintdefer: %s; the piece is destroyed as before (failure %d of %d)", why, g.failures + 1, kMaxFailures);
    if (++g.failures >= kMaxFailures && !g.off) {
        g.off = true;
        HSM_LOG("paintdefer: off for this run after %d failures; the game destroys swapped-out armour as before", g.failures);
    }
}

// Pre-hook on the Blueprint callback of a GetAllVertexColorsOnly task. Never blocks it.
bool OnCallback(ScriptCall& c) {
    ++g.n.seen;
    g.borrowed.clear();   // a stale entry (the post-hook never ran) must not be written back
    if (g.off) return false;
    UObject* owner = c.self();
    // The callback destroys the piece only while the armour swap runs (:8009); otherwise it keeps dismember paint.
    if (!owner || !Flag(owner, "Armor Swap In Process")) { ++g.n.notSwap; return false; }
    if (!ResolveLayout()) return false;
    auto* result = static_cast<uint8_t*>(c.param("TaskResultInfo"));
    if (!result) return false;
    auto** meshSlot = reinterpret_cast<UObject**>(result + g.layout.meshOffset);
    UObject* piece = *meshSlot;
    UObject* paint = *reinterpret_cast<UObject**>(result + g.layout.paintOffset);
    if (!piece || IsGarbage(piece) || !paint || IsGarbage(paint)) return false;
    Call dying(piece, "IsBeingDestroyed");
    if (!dying.valid() || !dying.invoke() || dying.get<bool>("ReturnValue")) return false;   // the callback returns early itself
    const int busy = TasksReference(paint, piece);
    if (busy < 0) { Fail("cannot read the paint component's task list"); return false; }
    if (busy == 0) { ++g.n.notBusy; return false; }
    const int32_t index = ObjectIndex(piece);
    const uint64_t key = uint64_t(uint32_t(index));
    if (g.parked.count(key)) return false;
    if (g.queue.Size() >= combat::kPaintDeferMax) {
        ++g.n.refused;
        HSM_LOG("paintdefer: %zu pieces already waiting; %s is destroyed as before", g.queue.Size(), ObjectName(piece).c_str());
        return false;
    }
    Call assetGet(piece, "GetSkeletalMeshAsset");
    UObject* asset = assetGet.valid() && assetGet.invoke() ? assetGet.get<UObject*>("ReturnValue") : nullptr;
    if (!asset) return false;   // not a skeletal piece: nothing to key the paint record by; the game's path is unchanged
    UObject* decoy = MakeDecoy(owner, asset);
    if (!decoy) { Fail("could not make the stand-in component"); return false; }
    // Committed. The real piece leaves the armour list and the scene; the callback sees the decoy.
    const bool listed = RemoveFromWornArmor(owner, piece);
    Hide(piece);
    *meshSlot = decoy;
    g.borrowed.push_back({meshSlot, piece});
    const double now = SteadyNowMs();
    g.queue.Add(key, now);
    Parked p;
    p.comp = piece; p.compIndex = index;
    p.owner = owner; p.ownerIndex = ObjectIndex(owner);
    p.paint = paint; p.paintIndex = ObjectIndex(paint);
    g.parked[key] = p;
    ++g.n.deferred;
    if (LogThis())
        HSM_LOG("paintdefer: %s on %s still has paint tasks; destroy deferred (%zu waiting%s)", ObjectName(piece).c_str(),
                ObjectName(owner).c_str(), g.queue.Size(), listed ? "" : ", was not in Worn Armor");
    return false;
}

void OnCallbackDone(ScriptCall&) {
    while (!g.borrowed.empty()) {
        State::Borrow b = g.borrowed.back();
        g.borrowed.pop_back();
        if (b.slot) *b.slot = b.original;
    }
}

}  // namespace

bool InstallPaintDefer() {
    if (!ScriptHookActive()) return false;
    g.installed = HookScript(kOwner, "Willie_BP_C", kCallback, OnCallback, OnCallbackDone) >= 0;
    HSM_LOG("paintdefer: %s (%d function(s) hooked now)", g.installed ? "armed: swapped-out armour is destroyed only when no paint task reads it" : "NOT registered",
            ResolveScriptHooks());
    return g.installed;
}

void TickPaintDefer() {
    if (g.queue.Size() == 0) return;
    const double now = SteadyNowMs();
    // Pieces the game or a level change already destroyed are forgotten.
    for (auto it = g.parked.begin(); it != g.parked.end();) {
        if (!Live(it->second.comp, it->second.compIndex)) {
            g.queue.Drop(it->first);
            ++g.n.gone;
            if (LogThis()) HSM_LOG("paintdefer: a waiting piece is gone (destroyed elsewhere or the level changed)");
            it = g.parked.erase(it);
        } else ++it;
    }
    std::vector<combat::PaintRelease> due;
    g.queue.Poll(now, [](uint64_t key) {
        auto it = g.parked.find(key);
        if (it == g.parked.end()) return false;
        const Parked& p = it->second;
        if (!Live(p.paint, p.paintIndex)) return false;   // no paint component: nothing can be reading
        return TasksReference(p.paint, p.comp) != 0;      // unknown counts as busy until the time limit
    }, due);
    for (const combat::PaintRelease& r : due) {
        auto it = g.parked.find(r.key);
        if (it == g.parked.end()) continue;
        const Parked p = it->second;
        g.parked.erase(it);
        if (Live(p.comp, p.compIndex)) {
            Call kill(p.comp, "K2_DestroyComponent");
            kill.set<UObject*>("Object", Live(p.owner, p.ownerIndex) ? p.owner : nullptr);
            if (kill.valid()) kill.invoke();
        }
        ++g.n.released;
        if (r.timedOut) ++g.n.timedOut;
        g.n.waitedMs += r.waitedMs;
        if (r.waitedMs > g.n.maxWaitMs) g.n.maxWaitMs = r.waitedMs;
        if (LogThis())
            HSM_LOG("paintdefer: piece destroyed after %.0f ms (%s)", r.waitedMs, r.timedOut ? "time limit, tasks still listed it" : "no task references it");
    }
}

void PaintDeferLevelChanged() {
    if (g.queue.Size())
        HSM_LOG("paintdefer: level changed with %zu piece(s) waiting; forgotten with the old world", g.queue.Size());
    g.queue.Clear();
    g.parked.clear();
    g.borrowed.clear();
    g.layout = ResultLayout{};
    g.skeletalClass = nullptr;
}

std::string PaintDeferHealthLine() {
    const State::Counters d{g.n.seen - g.reported.seen, g.n.notSwap - g.reported.notSwap, g.n.notBusy - g.reported.notBusy,
                            g.n.deferred - g.reported.deferred, g.n.released - g.reported.released,
                            g.n.timedOut - g.reported.timedOut, g.n.gone - g.reported.gone, g.n.refused - g.reported.refused,
                            g.n.failed - g.reported.failed, g.n.waitedMs - g.reported.waitedMs, g.n.maxWaitMs};
    g.reported = g.n;
    g.reported.maxWaitMs = 0;
    g.n.maxWaitMs = 0;
    if (!d.seen) return {};
    char b[256];
    std::snprintf(b, sizeof b, "paintdefer: %llu callbacks (%llu outside a swap, %llu with no task on the piece), %llu deferred, %llu released (%llu by the time limit, avg %.0f ms, max %.0f ms), %llu gone, %llu refused, %llu failed, %zu waiting",
                  (unsigned long long)d.seen, (unsigned long long)d.notSwap, (unsigned long long)d.notBusy, (unsigned long long)d.deferred,
                  (unsigned long long)d.released, (unsigned long long)d.timedOut, d.released ? d.waitedMs / double(d.released) : 0.0, d.maxWaitMs,
                  (unsigned long long)d.gone, (unsigned long long)d.refused, (unsigned long long)d.failed, g.queue.Size());
    return b;
}

}  // namespace hsm::game
