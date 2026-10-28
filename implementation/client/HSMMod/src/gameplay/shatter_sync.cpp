#include "gameplay/object_sync.h"
#include "log.h"
#include "ue/script_hook.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <map>
#include <set>
#include <string>
#include <unordered_map>

// Shattering objects (shatter_sync.h). The members of ObjectSync that detect a geometry collection's break where the object
// is streamed, send it as an ObjectEvent Fracture, and make the same break everywhere else.
//
// How a break is detected. A geometry collection's break notifications (OnChaosBreakEvent) are a multicast delegate, which no
// ProcessEvent hook sees, and the cluster state itself (the dynamic collection) is not a reflected property. What IS reflected
// is UGeometryCollectionComponent::IsRootBroken (an exec function, tools/check_reflection.py verifies its name), so it is
// polled on the object's streamer at the event cadence (kEventPollMs): a root cluster that has broken is the shattering of a
// single-level collection, and one Fracture goes out for it (item = GetRootIndex). SetNotifyBreaks(true) is still turned on
// there so the engine keeps the break data current for the day a delegate can be reached.
//
// How it is made everywhere else. The copy's SetEnableDamageFromCollision is turned off (it can never shatter by itself, and
// differently), and a Fracture applies ApplyExternalStrain at the item with the sender's place, radius and strain (clamped
// by the wire format). If the root is still whole kVerifyMs later, CrumbleCluster(item) finishes it. The calls run under a
// ScriptHookBypass, so no hook of ours reacts to them.

namespace hsm::game {
using namespace hsm::ue;
using namespace hsm::game::hands;

namespace {
constexpr double kVerifyMs = 1500.0;        // an applied break is checked this long after
constexpr float kBreakStrain = 1.0e6f;      // what the streamer says it broke with: far above any cluster's threshold
constexpr float kMinBreakRadius = 50.0f;    // cm
constexpr size_t kMaxBreakableLines = 48;   // the per-level breakables log is bounded

bool Contains(const std::vector<uint16_t>& v, uint16_t item) { return std::find(v.begin(), v.end(), item) != v.end(); }

bool RootBroken(UObject* comp) {
    Call ib(comp, "IsRootBroken");
    return ib.invoke() && ib.get<bool>("ReturnValue");
}
}  // namespace

bool IsGeometryCollection(const UObject* comp) {
    if (!comp) return false;
    static UClass* gcClass = nullptr;
    static unsigned misses = 0;
    static std::unordered_map<const UClass*, bool> known;
    if (!gcClass) {
        if ((misses++ & 0xFFF) != 0) return false;   // the class is native; if it is missing, look again only now and then
        gcClass = FindClass("GeometryCollectionComponent");
        if (!gcClass) return false;
    }
    const UClass* cls = ClassOf(comp);
    auto it = known.find(cls);
    if (it == known.end()) it = known.emplace(cls, IsChildOf(cls, gcClass)).first;
    return it->second;
}

// ---------------------------------------------------------------- per frame

// The geometry collection bodies of an object, scanned once per actor (a re-key or a new actor scans again).
ShatterObject& ObjectSync::ShatterScan(uint64_t id, Prop& p) {
    ShatterObject& so = shatter_.objs[id];
    if (so.actor == p.actor && so.index == p.index) return so;
    so = ShatterObject{};
    so.actor = p.actor;
    so.index = p.index;
    for (size_t b = 0; b < p.bodies.size(); ++b) {
        const BodyRef& r = p.bodies[b];
        if (r.detached || !r.comp || !IsGeometryCollection(r.comp)) continue;
        ShatterBody sb;
        sb.comp = r.comp;
        sb.index = r.index;
        sb.body = uint8_t(b);
        Call ri(r.comp, "GetRootIndex");
        sb.rootItem = ri.invoke() ? ri.get<int32_t>("ReturnValue") : -1;
        so.gcs.push_back(std::move(sb));
        ++shatter_.found;
        if (shatter_.found <= kLogFirst)
            HSM_LOG("shatter: %s (%016llx) has a geometry collection (%s, body %zu, root item %d)", ObjectName(p.actor).c_str(),
                    static_cast<unsigned long long>(id), ObjectName(r.comp).c_str(), b, int(so.gcs.back().rootItem));
    }
    return so;
}

void ObjectSync::ShatterPoll(double nowMs) {
    if (!active_) return;
    for (auto it = shatter_.objs.begin(); it != shatter_.objs.end();) {   // gone, or its actor is another now
        auto p = props_.find(it->first);
        it = (p == props_.end() || p->second.actor != it->second.actor) ? shatter_.objs.erase(it) : std::next(it);
    }
    const uint8_t me = MySlot();
    for (auto& [id, p] : props_) {
        if (IsWeaponId(id) || p.provisional || p.sub || !adapter_->StillValid(p.actor, p.index)) continue;
        ShatterObject& so = ShatterScan(id, p);
        if (so.gcs.empty()) continue;
        const bool streams = view_.Streamer(id) == me && !p.attachedSent;
        for (ShatterBody& b : so.gcs) ShatterStep(id, b, streams, nowMs);
    }
    // Once per fight level, after the first census (the host's sent, the client's matched): what can break here.
    if (!shatter_.breakablesLogged && (env_.host ? haveSent_ : matched_) && env_.fightLevel && env_.fightLevel()) {
        shatter_.breakablesLogged = true;
        LogBreakables();
    }
}

// One geometry collection: the stance of this machine toward it (it breaks it, or it is told), the detection, the check of a
// break applied earlier.
void ObjectSync::ShatterStep(uint64_t id, ShatterBody& b, bool streams, double nowMs) {
    if (!adapter_->StillValid(b.comp, b.index)) return;
    if (streams) {
        if (!b.notify) {
            b.notify = true;
            Call(b.comp, "SetNotifyBreaks").set<bool>("bNewNotifyBreaks", true).invoke();
        }
        if (b.damageOff) {   // became its streamer (a lease): it breaks on its own collisions again
            b.damageOff = false;
            Call(b.comp, "SetEnableDamageFromCollision").set<bool>("bValue", true).invoke();
        }
        const uint16_t item = uint16_t(std::clamp<int32_t>(b.rootItem, 0, kMaxFractureItem));
        if (!Contains(b.broken, item) && RootBroken(b.comp)) ShatterBreak(id, b, nowMs);
    } else if (!b.damageOff) {
        b.damageOff = true;
        Call(b.comp, "SetEnableDamageFromCollision").set<bool>("bValue", false).invoke();
    }
    if (b.verifyMs > 0 && nowMs >= b.verifyMs) {
        b.verifyMs = 0;
        if (!RootBroken(b.comp)) {
            ScriptHookBypass bypass;
            Call(b.comp, "CrumbleCluster").set<int32_t>("ItemIndex", int32_t(b.verifyItem)).invoke();
            ++shatter_.crumbled;
            HSM_LOG("shatter: the strain left %s whole; crumbled item %u", ObjectName(b.comp).c_str(), unsigned(b.verifyItem));
        }
    }
}

// The root of a collection I stream broke: tell everyone (once, rate capped per object).
void ObjectSync::ShatterBreak(uint64_t id, ShatterBody& b, double nowMs) {
    if (!shatter_.rate.Allow(id, nowMs)) { ++shatter_.rateDropped; return; }   // not marked: it is sent on a later poll
    ObjectEventMsg e;
    e.objId = id;
    e.body = b.body;
    e.kind = ObjectEventKind::Fracture;
    e.item = uint16_t(std::clamp<int32_t>(b.rootItem, 0, kMaxFractureItem));
    auto so = shatter_.objs.find(id);
    if (so == shatter_.objs.end()) return;
    const FVector at = adapter_->ActorLocation(so->second.actor);   // the collection's place (its actor's position)
    const double lim = double(kMaxPropCoord);
    if (!std::isfinite(at.x) || !std::isfinite(at.y) || !std::isfinite(at.z) || std::fabs(at.x) > lim || std::fabs(at.y) > lim || std::fabs(at.z) > lim) {
        b.broken.push_back(e.item);   // a place the wire would refuse: nothing to say; the break stays local
        return;
    }
    e.x = float(at.x);
    e.y = float(at.y);
    e.z = float(at.z);
    float radius = kMinBreakRadius;
    Call lb(b.comp, "GetLocalBounds");
    if (lb.invoke()) {
        const FVector lo = lb.get<FVector>("Min"), hi = lb.get<FVector>("Max");
        const double d = 0.5 * std::sqrt((hi.x - lo.x) * (hi.x - lo.x) + (hi.y - lo.y) * (hi.y - lo.y) + (hi.z - lo.z) * (hi.z - lo.z));
        if (std::isfinite(d)) radius = float(std::clamp(d, double(kMinBreakRadius), double(kMaxFractureRadius)));
    }
    e.radius = radius;
    e.strain = kBreakStrain;
    if (b.broken.size() < kMaxFracturesPerObject) b.broken.push_back(e.item);
    SendEvent(e);
    ++shatter_.sent;
    if (shatter_.sent <= kLogFirst)
        HSM_LOG("shatter: %s (%016llx) broke here; sent item %u at (%.0f, %.0f, %.0f), radius %.0f", ObjectName(b.comp).c_str(),
                static_cast<unsigned long long>(id), unsigned(e.item), double(e.x), double(e.y), double(e.z), double(e.radius));
}

// ---------------------------------------------------------------- the event

// Every machine but the sender: the same break on my copy. false: the object is not here yet (the event waits).
bool ObjectSync::ShatterReplay(const ObjectEventMsg& m, double nowMs) {
    auto it = props_.find(m.objId);
    if (it == props_.end()) return false;
    Prop& p = it->second;
    if (p.provisional || !adapter_->StillValid(p.actor, p.index)) return false;
    ShatterObject& so = ShatterScan(m.objId, p);
    ShatterBody* b = nullptr;
    for (ShatterBody& g : so.gcs)
        if (g.body == m.body) b = &g;
    if (!b) return true;   // the object here has no geometry collection at that body: nothing to break
    if (Contains(b->broken, m.item)) return true;   // idempotent: sent from here, or applied already
    if (b->broken.size() < kMaxFracturesPerObject) b->broken.push_back(m.item);   // set first: breaking it here is not an event of mine
    if (!adapter_->StillValid(b->comp, b->index)) return true;
    if (RootBroken(b->comp)) { ++shatter_.alreadyBroken; return true; }
    ScriptHookBypass bypass;
    Call(b->comp, "ApplyExternalStrain")
        .set<int32_t>("ItemIndex", int32_t(m.item))
        .set<FVector>("Location", FVector{double(m.x), double(m.y), double(m.z)})
        .setReal("Radius", double(m.radius))
        .set<int32_t>("PropagationDepth", 0)
        .setReal("PropagationFactor", 0.0)
        .setReal("Strain", double(m.strain))
        .invoke();
    b->verifyMs = nowMs + kVerifyMs;
    b->verifyItem = m.item;
    ++shatter_.applied;
    ++n_.eventsApplied;
    if (shatter_.applied <= kLogFirst)
        HSM_LOG("shatter: applied a break to %s (%016llx) item %u at (%.0f, %.0f, %.0f)", ObjectName(b->comp).c_str(),
                static_cast<unsigned long long>(m.objId), unsigned(m.item), double(m.x), double(m.y), double(m.z));
    return true;
}

// ---------------------------------------------------------------- the breakables line

// One line per breakable actor class of the level, once: geometry collections (and how many of those are synced here),
// constraint-built destructibles (several bodies joined by physics constraints) and implementers of BPI_Destructible.
void ObjectSync::LogBreakables() {
    UClass* gcClass = FindClass("GeometryCollectionComponent");
    UClass* actorClass = FindClass("Actor");
    UClass* iface = FindClass("BPI_Destructible_C");
    UClass* worldClass = FindClass("World");
    UObject* ksl = nullptr;
    if (iface) if (UObject* c = FindObject("KismetSystemLibrary", "Class")) ksl = DefaultObject(reinterpret_cast<UClass*>(c));
    struct Count { size_t n = 0, synced = 0; };
    std::map<std::pair<std::string, int>, Count> counts;   // (class name, kind 0 collection / 1 constraint-built / 2 interface)
    std::set<const UObject*> gcActors;
    std::unordered_map<const UClass*, bool> implementers;
    std::string level;
    for (int32_t i = 0, n = ObjectCount(); i < n; ++i) {
        UObject* o = ObjectAt(i);
        if (!o || IsDefaultObject(o) || IsGarbage(o)) continue;
        const UClass* cls = ClassOf(o);
        if (level.empty() && worldClass && IsChildOf(cls, worldClass)) level = ObjectName(o);
        if (gcClass && IsChildOf(cls, gcClass)) {
            UObject* actor = OuterOf(o);
            if (actor && !IsDefaultObject(actor) && !IsGarbage(actor) && gcActors.insert(actor).second) {
                Count& c = counts[{ObjectName(reinterpret_cast<const UObject*>(ClassOf(actor))), 0}];
                ++c.n;
                if (byActor_.count(actor)) ++c.synced;
            }
            continue;
        }
        if (!iface || !ksl || !actorClass || !IsChildOf(cls, actorClass)) continue;
        auto im = implementers.find(cls);
        if (im == implementers.end()) {
            Call d(ksl, "DoesImplementInterface");
            d.set<UObject*>("TestObject", o).set<UObject*>("Interface", reinterpret_cast<UObject*>(iface));
            im = implementers.emplace(cls, d.invoke() && d.get<bool>("ReturnValue")).first;
        }
        if (im->second) ++counts[{ObjectName(reinterpret_cast<const UObject*>(cls)), 2}].n;
    }
    for (const auto& [id, p] : props_) {
        if (IsWeaponId(id) || p.provisional || p.sub || p.bodies.size() < 2 || p.constraints.empty() || !adapter_->StillValid(p.actor, p.index)) continue;
        Count& c = counts[{ObjectName(reinterpret_cast<const UObject*>(ClassOf(p.actor))), 1}];
        ++c.n;
        ++c.synced;
    }
    if (level.empty()) level = "?";
    if (counts.empty()) { HSM_LOG("objects: breakables on %s: none", level.c_str()); return; }
    size_t lines = 0;
    for (const auto& [key, c] : counts) {
        if (lines >= kMaxBreakableLines) { HSM_LOG("objects: breakables on %s: %zu more classes not listed", level.c_str(), counts.size() - lines); break; }
        ++lines;
        if (key.second == 0)
            HSM_LOG("objects: breakables on %s: %s x%zu (geometry collection, %zu synced here)", level.c_str(), key.first.c_str(), c.n, c.synced);
        else if (key.second == 1)
            HSM_LOG("objects: breakables on %s: %s x%zu (constraint-built, bodies joined by physics constraints)", level.c_str(), key.first.c_str(), c.n);
        else
            HSM_LOG("objects: breakables on %s: %s x%zu (BPI_Destructible)", level.c_str(), key.first.c_str(), c.n);
    }
}

}  // namespace hsm::game
