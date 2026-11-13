#include "gameplay/crypts_probe.h"
#include "log.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <map>
#include <string>
#include <unordered_map>
#include <vector>

using namespace hsm::ue;

namespace hsm::game {
namespace {

constexpr const char* kBlockClass = "BP_Procedural_Block_Master_C";
constexpr const char* kSpawnerClass = "BP_SpawnerPoint_Willies_C";
constexpr const char* kDoorClass = "BP_Door_Gate_A_001_C";
constexpr const char* kStaticMeshActorClass = "StaticMeshActor";
constexpr const char* kActorClass = "Actor";
constexpr const char* kStreamingClass = "LevelStreamingDynamic";
constexpr const char* kBlockBoolFields[] = {"Is Entrance?", "Is Exit?", "Is Treasury?", "Is Boss Arena?", "Pre Boss", "Is Shortcut",
                                            "Has Shortcut", "Is Purgatory", "Has Treasury", "Has Exit", "Has Stairs Up",
                                            "Has Stairs Down", "Leads To Exit", "Door Barricated", "Fully Loaded"};

constexpr uint32_t kCatalogAfterMs = 8000;   // the first room is spawned 2 s after the level loads
constexpr uint32_t kScanGapMs = 2000;
constexpr uint32_t kFarGapMs = 1500;         // between one far room's unload and the next load
constexpr uint32_t kFarTimeoutMs = 25000;
constexpr double kFarX = 300000, kFarStep = 25000;   // 3 km out, 250 m apart
constexpr int kMaxListed = 24;                       // spawn points per log line

template <class T> struct ArrayView { T* data; int32_t num, max; };

FVector Add(FVector a, FVector b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
FVector Sub(FVector a, FVector b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
FVector Mul(FVector a, double s) { return {a.x * s, a.y * s, a.z * s}; }
FVector Cross(FVector a, FVector b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
double Dist(FVector a, FVector b) { const FVector d = Sub(a, b); return std::sqrt(d.x * d.x + d.y * d.y + d.z * d.z); }
FVector Rotate(const FQuat& q, FVector v) {
    const FVector u{q.x, q.y, q.z};
    const FVector t = Mul(Cross(u, v), 2.0);
    return Add(Add(v, Mul(t, q.w)), Cross(u, t));
}
FVector Unrotate(const FQuat& q, FVector v) { return Rotate(FQuat{-q.x, -q.y, -q.z, q.w}, v); }
FVector ToLocal(const FTransform& t, FVector world) { return Unrotate(t.rotation, Sub(world, t.translation)); }

double Wrap(double a) { a = std::fmod(a + 180.0, 360.0); if (a < 0) a += 360.0; return a - 180.0; }
FRotator ToRotator(const FQuat& q) {   // FQuat::Rotator
    constexpr double kThr = 0.4999995, kDeg = 57.29577951308232;
    const double st = q.z * q.x - q.w * q.y;
    const double yawY = 2 * (q.w * q.z + q.x * q.y), yawX = 1 - 2 * (q.y * q.y + q.z * q.z);
    FRotator r;
    r.yaw = std::atan2(yawY, yawX) * kDeg;
    if (st < -kThr) { r.pitch = -90; r.roll = Wrap(-r.yaw - 2 * std::atan2(q.x, q.w) * kDeg); }
    else if (st > kThr) { r.pitch = 90; r.roll = Wrap(r.yaw - 2 * std::atan2(q.x, q.w) * kDeg); }
    else { r.pitch = std::asin(2 * st) * kDeg; r.roll = std::atan2(-2 * (q.w * q.x + q.y * q.z), 1 - 2 * (q.x * q.x + q.y * q.y)) * kDeg; }
    return r;
}

std::string V(FVector v) { char b[96]; std::snprintf(b, sizeof b, "(%.0f,%.0f,%.0f)", v.x, v.y, v.z); return b; }
std::string Tr(const FTransform* t) {
    if (!t) return "?";
    const FRotator r = ToRotator(t->rotation);
    char b[200];
    std::snprintf(b, sizeof b, "loc%s rot(%.1f,%.1f,%.1f)", V(t->translation).c_str(), r.pitch, r.yaw, r.roll);
    std::string s = b;
    if (std::fabs(t->scale.x - 1) + std::fabs(t->scale.y - 1) + std::fabs(t->scale.z - 1) > 1e-3) s += " scale" + V(t->scale);
    return s;
}

struct Box {
    FVector lo{1e300, 1e300, 1e300}, hi{-1e300, -1e300, -1e300};
    int n = 0;
    void Add(FVector p) {
        lo = {std::min(lo.x, p.x), std::min(lo.y, p.y), std::min(lo.z, p.z)};
        hi = {std::max(hi.x, p.x), std::max(hi.y, p.y), std::max(hi.z, p.z)};
    }
    void AddWorldBox(const FTransform& frame, FVector c, FVector e) {   // the 8 corners, into the frame
        ++n;
        for (int i = 0; i < 8; ++i)
            Add(ToLocal(frame, {c.x + (i & 1 ? e.x : -e.x), c.y + (i & 2 ? e.y : -e.y), c.z + (i & 4 ? e.z : -e.z)}));
    }
    std::string Str() const { return n ? V(lo) + ".." + V(hi) + " (" + std::to_string(n) + ")" : std::string("none"); }
};

UObject* Ksl() {
    static UObject* k = nullptr;
    if (!k) if (UObject* c = FindObject("KismetSystemLibrary", "Class")) k = DefaultObject(reinterpret_cast<UClass*>(c));
    return k;
}

std::string SoftPath(void* softPtr) {
    UObject* k = Ksl();
    if (!softPtr || !k) return "?";
    Call c(k, "Conv_SoftObjectReferenceToString");
    c.setFrom("SoftObjectReference", softPtr);
    if (!c.invoke()) return "?";
    void* r = c.paramPtr("ReturnValue");
    const std::string s = r ? ReadFString(r) : std::string("?");
    return s.empty() ? std::string("-") : s;
}

const FTransform* Tf(void* p) { return static_cast<const FTransform*>(p); }

std::string ClassName(UClass* c) { return ObjectName(reinterpret_cast<UObject*>(c)); }

bool Alive(UObject* o, int32_t index) { return o && index >= 0 && ObjectAt(index) == o && !IsGarbage(o); }

struct RoomClass {
    UClass* cls = nullptr;
    UObject* cdo = nullptr;
    std::string name, module, lighting, logic;
    std::vector<FTransform> entrances;
};

struct LiveRoom {
    UObject* block = nullptr;
    int32_t index = -1;
    std::string name;
    bool measured[3] = {};
    bool gone = false;
};

struct FarJob {
    size_t room = 0;
    FTransform at;
    UObject* streams[3] = {};
    int32_t index[3] = {-1, -1, -1};
    uint32_t startMs = 0;
};

struct State {
    bool on = false;
    UObject* world = nullptr;
    uint32_t armedAt = 0, nextScan = 0, nextFar = 0;
    bool catalogued = false, farDone = false, layoutOk = false;
    UClass* blockCls = nullptr; UClass* spawnerCls = nullptr; UClass* doorCls = nullptr; UClass* smaCls = nullptr; UClass* actorCls = nullptr;
    UObject* streamingCdo = nullptr;
    std::vector<RoomClass> rooms;
    std::map<UObject*, LiveRoom> live;
    std::unordered_map<UObject*, int32_t> doorsSeen;   // door-like actors anywhere, logged once
    std::unordered_map<UClass*, uint8_t> kind;         // 0 other, 1 room, 2 door-like
    std::vector<size_t> farQueue;                      // rooms (by distinct Module level) still to measure far away
    bool farBusy = false;
    FarJob job;
    int farCount = 0;
};
State g;

const RoomClass* RoomOf(UClass* c) {
    for (const RoomClass& r : g.rooms) if (r.cls == c) return &r;
    return nullptr;
}

uint8_t KindOf(UClass* c) {
    auto it = g.kind.find(c);
    if (it != g.kind.end()) return it->second;
    uint8_t k = 0;
    if (g.blockCls && IsChildOf(c, g.blockCls)) k = 1;
    else {
        const std::string n = ClassName(c);
        if (n.find("Door") != std::string::npos || n.find("Barricade") != std::string::npos) k = 2;
    }
    g.kind.emplace(c, k);
    return k;
}

std::string Flags(UObject* o) {
    std::string s;
    for (const char* f : kBlockBoolFields) {
        bool v = false;
        if (!GetBool(o, f, v) || !v) continue;
        if (!s.empty()) s += ",";
        s += f;
    }
    return s.empty() ? std::string("-") : s;
}

void Catalog() {
    g.catalogued = true;
    UObject* bgc = FindObject("BlueprintGeneratedClass", "Class");
    for (int32_t i = 0, n = ObjectCount(); i < n; ++i) {
        UObject* o = ObjectAt(i);
        if (!o || ClassOf(o) != reinterpret_cast<UClass*>(bgc)) continue;
        UClass* c = reinterpret_cast<UClass*>(o);
        if (c == g.blockCls || !IsChildOf(c, g.blockCls)) continue;
        RoomClass r;
        r.cls = c;
        r.cdo = DefaultObject(c);
        r.name = ClassName(c);
        if (!r.cdo) continue;
        r.module = SoftPath(FieldPtr(r.cdo, "Level"));
        r.lighting = SoftPath(FieldPtr(r.cdo, "Lighting Level"));
        r.logic = SoftPath(FieldPtr(r.cdo, "Logic Level"));
        if (auto* a = static_cast<const ArrayView<FTransform>*>(FieldPtr(r.cdo, "Entrances List")); a && a->data && a->num > 0 && a->num < 64)
            r.entrances.assign(a->data, a->data + a->num);
        g.rooms.push_back(r);
    }
    std::sort(g.rooms.begin(), g.rooms.end(), [](const RoomClass& a, const RoomClass& b) { return a.name < b.name; });
    HSM_LOG("cryptsprobe: catalog: %zu room classes loaded", g.rooms.size());
    std::map<std::string, size_t> firstOfModule;
    for (size_t i = 0; i < g.rooms.size(); ++i) {
        const RoomClass& r = g.rooms[i];
        UObject* d = r.cdo;
        const int32_t* fixedInt = Field<int32_t>(d, "Fixed Entrance Int");
        const int32_t* tierMod = Field<int32_t>(d, "Room Tier Modifier");
        const float* radius = Field<float>(d, "Spawn Radius");
        const float* chance = Field<float>(d, "Spawn Chance");
        bool fixed = false;
        GetBool(d, "Fixed Entrance", fixed);
        HSM_LOG("cryptsprobe: class %s module=%s lighting=%s logic=%s entrances=%zu fixed=%d/%d tierMod=%d spawnRadius=%.0f spawnChance=%.2f flags=%s",
                r.name.c_str(), r.module.c_str(), r.lighting.c_str(), r.logic.c_str(), r.entrances.size(), fixed ? 1 : 0,
                fixedInt ? *fixedInt : -1, tierMod ? *tierMod : -1, radius ? double(*radius) : -1.0, chance ? double(*chance) : -1.0,
                Flags(d).c_str());
        for (size_t e = 0; e < r.entrances.size(); ++e)
            HSM_LOG("cryptsprobe: class %s entrance %zu %s", r.name.c_str(), e, Tr(&r.entrances[e]).c_str());
        if (auto* a = static_cast<const ArrayView<UClass*>*>(FieldPtr(d, "Spawnable Levels Array")); a && a->data && a->num >= 0 && a->num < 256) {
            std::string list;
            for (int32_t k = 0; k < a->num; ++k) { if (k) list += ","; list += a->data[k] ? ClassName(a->data[k]) : std::string("null"); }
            HSM_LOG("cryptsprobe: class %s spawnable[%d] %s", r.name.c_str(), a->num, list.c_str());
        }
        if (r.module.size() > 1 && firstOfModule.emplace(r.module, i).second) g.farQueue.push_back(i);
    }
    HSM_LOG("cryptsprobe: catalog: %zu distinct room levels to measure far away", g.farQueue.size());
}

// What a loaded level holds, in `frame` (the room's Origin Point, or the far spot it was loaded at).
void MeasureLevel(UObject* level, const FTransform& frame, const std::string& tag, const RoomClass* room) {
    std::map<std::string, int> classes;
    Box mesh, all;
    std::vector<std::pair<double, std::string>> big;
    std::vector<FVector> spawns;
    int loot = 0, actors = 0;
    const double frameYaw = ToRotator(frame.rotation).yaw;
    for (int32_t i = 0, n = ObjectCount(); i < n; ++i) {
        UObject* o = ObjectAt(i);
        if (!o || OuterOf(o) != level || IsDefaultObject(o) || IsGarbage(o) || !IsA(o, g.actorCls)) continue;
        ++actors;
        UClass* c = ClassOf(o);
        const std::string cn = ClassName(c);
        ++classes[cn];
        Call b(o, "GetActorBounds");
        b.set<bool>("bOnlyCollidingComponents", true).set<bool>("bIncludeFromChildActors", false);
        if (b.invoke()) {
            const FVector ce = b.get<FVector>("Origin"), ex = b.get<FVector>("BoxExtent");
            if (ex.x + ex.y + ex.z > 1.0) {
                all.AddWorldBox(frame, ce, ex);
                if (IsA(o, g.smaCls)) mesh.AddWorldBox(frame, ce, ex);
                big.push_back({std::max({ex.x, ex.y, ex.z}), cn + V(ex)});
            }
        }
        Call l(o, "K2_GetActorLocation");
        const FVector at = l.invoke() ? ToLocal(frame, l.get<FVector>("ReturnValue")) : FVector{};
        if (g.spawnerCls && IsA(o, g.spawnerCls)) spawns.push_back(at);
        if (cn.find("LootPoint") != std::string::npos) ++loot;
        if (KindOf(c) == 2) {
            Call r(o, "K2_GetActorRotation");
            const double yaw = r.invoke() ? Wrap(r.get<FRotator>("ReturnValue").yaw - frameYaw) : 0.0;
            std::string near = "-";
            if (room && !room->entrances.empty()) {
                size_t best = 0;
                for (size_t e = 1; e < room->entrances.size(); ++e)
                    if (Dist(at, room->entrances[e].translation) < Dist(at, room->entrances[best].translation)) best = e;
                char nb[48];
                std::snprintf(nb, sizeof nb, "entrance %zu at %.0f", best, Dist(at, room->entrances[best].translation));
                near = nb;
            }
            bool locked = false, open = false;
            std::string phys = "-";
            if (g.doorCls && IsA(o, g.doorCls)) {
                GetBool(o, "Locked", locked);
                GetBool(o, "Start Open", open);
                if (UObject** door = Field<UObject*>(o, "Door"); door && *door) {
                    Call p(*door, "IsSimulatingPhysics");
                    p.set<FName>("BoneName", FName{});
                    phys = p.invoke() ? (p.get<bool>("ReturnValue") ? "physics" : "static") : "?";
                }
            }
            HSM_LOG("cryptsprobe: %s door %s at %s yaw %.0f locked=%d startOpen=%d %s; nearest %s", tag.c_str(), cn.c_str(),
                    V(at).c_str(), yaw, locked ? 1 : 0, open ? 1 : 0, phys.c_str(), near.c_str());
        }
    }
    std::sort(big.begin(), big.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
    std::string bigS;
    for (size_t i = 0; i < big.size() && i < 3; ++i) bigS += (i ? " " : "") + big[i].second;
    std::vector<std::pair<int, std::string>> top;
    for (const auto& [k, v] : classes) top.push_back({v, k});
    std::sort(top.begin(), top.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
    std::string topS;
    for (size_t i = 0; i < top.size() && i < 14; ++i) topS += (i ? " " : "") + top[i].second + "x" + std::to_string(top[i].first);
    HSM_LOG("cryptsprobe: %s level %s: %d actors, meshes %s, all %s, biggest %s, loot points %d, spawn points %zu",
            tag.c_str(), ObjectName(OuterOf(level)).c_str(), actors, mesh.Str().c_str(), all.Str().c_str(),
            bigS.empty() ? "-" : bigS.c_str(), loot, spawns.size());
    HSM_LOG("cryptsprobe: %s classes %s", tag.c_str(), topS.c_str());
    for (size_t i = 0; i < spawns.size(); i += kMaxListed) {
        std::string s;
        for (size_t k = i; k < spawns.size() && k < i + kMaxListed; ++k) s += (k > i ? " " : "") + V(spawns[k]);
        HSM_LOG("cryptsprobe: %s spawns %zu-%zu %s", tag.c_str(), i, std::min(spawns.size(), i + kMaxListed) - 1, s.c_str());
    }
}

// A streaming level that is loaded and shown: its ULevel, else nullptr.
UObject* ShownLevel(UObject* stream) {
    if (!stream || IsGarbage(stream)) return nullptr;
    Call v(stream, "IsLevelVisible");
    if (!v.invoke() || !v.get<bool>("ReturnValue")) return nullptr;
    Call l(stream, "GetLoadedLevel");
    return l.invoke() ? l.get<UObject*>("ReturnValue") : nullptr;
}

void ScanLive(uint32_t now) {
    for (auto& [o, r] : g.live)
        if (!r.gone && !Alive(o, r.index)) { r.gone = true; HSM_LOG("cryptsprobe: room %s gone", r.name.c_str()); }
    for (int32_t i = 0, n = ObjectCount(); i < n; ++i) {
        UObject* o = ObjectAt(i);
        if (!o || IsDefaultObject(o) || IsGarbage(o)) continue;
        UClass* c = ClassOf(o);
        if (!c) continue;
        const uint8_t k = KindOf(c);
        if (k == 2 && !g.doorsSeen.count(o) && IsA(o, g.actorCls)) {
            g.doorsSeen.emplace(o, i);
            Call l(o, "K2_GetActorLocation");
            Call r(o, "K2_GetActorRotation");
            const FVector at = l.invoke() ? l.get<FVector>("ReturnValue") : FVector{};
            if (at.x > kFarX - kFarStep) continue;   // a far measured room's door: logged with its room
            const double yaw = r.invoke() ? r.get<FRotator>("ReturnValue").yaw : 0.0;
            bool locked = false, open = false;
            if (g.doorCls && IsA(o, g.doorCls)) { GetBool(o, "Locked", locked); GetBool(o, "Start Open", open); }
            HSM_LOG("cryptsprobe: door %s %s in %s at %s yaw %.0f locked=%d startOpen=%d (t=%us)", ObjectName(o).c_str(),
                    ClassName(c).c_str(), ObjectName(OuterOf(OuterOf(o))).c_str(), V(at).c_str(), yaw, locked ? 1 : 0, open ? 1 : 0,
                    (now - g.armedAt) / 1000);
        }
        if (k != 1) continue;
        auto [it, fresh] = g.live.try_emplace(o);
        LiveRoom& r = it->second;
        if (fresh) {
            r.block = o; r.index = i; r.name = ObjectName(o);
            const int32_t* gen = Field<int32_t>(o, "Generation");
            UObject** parent = Field<UObject*>(o, "My Parent");
            const uint8_t* tier = Field<uint8_t>(o, "Room Tier");
            Call l(o, "K2_GetActorLocation");
            Call rot(o, "K2_GetActorRotation");
            const FVector at = l.invoke() ? l.get<FVector>("ReturnValue") : FVector{};
            const FRotator ar = rot.invoke() ? rot.get<FRotator>("ReturnValue") : FRotator{};
            const auto* ent = static_cast<const ArrayView<FTransform>*>(FieldPtr(o, "Entrances List"));
            HSM_LOG("cryptsprobe: room %s %s gen %d parent %s tier %d actor %s yaw %.1f flags %s (t=%us)", r.name.c_str(), ClassName(c).c_str(),
                    gen ? *gen : -1, parent && *parent ? ObjectName(*parent).c_str() : "-", tier ? int(*tier) : -1, V(at).c_str(), ar.yaw,
                    Flags(o).c_str(), (now - g.armedAt) / 1000);
            HSM_LOG("cryptsprobe: room %s start %s", r.name.c_str(), Tr(Tf(FieldPtr(o, "Start Position"))).c_str());
            HSM_LOG("cryptsprobe: room %s startEntrance %s", r.name.c_str(), Tr(Tf(FieldPtr(o, "Start Entrance"))).c_str());
            HSM_LOG("cryptsprobe: room %s origin %s; %d entrances left open", r.name.c_str(), Tr(Tf(FieldPtr(o, "Origin Point"))).c_str(),
                    ent && ent->data ? ent->num : -1);
        }
        if (r.measured[0] && r.measured[1] && r.measured[2]) continue;
        const FTransform* origin = Tf(FieldPtr(o, "Origin Point"));
        if (!origin) continue;
        UObject** streams[3] = {Field<UObject*>(o, "My Level"), Field<UObject*>(o, "My Lighting Level"), Field<UObject*>(o, "My Logic Level")};
        static const char* const kPart[3] = {"module", "lighting", "logic"};
        for (int s = 0; s < 3; ++s) {
            if (r.measured[s] || !streams[s] || !*streams[s]) continue;
            if (UObject* level = ShownLevel(*streams[s])) {
                r.measured[s] = true;
                MeasureLevel(level, *origin, "room " + r.name + " " + kPart[s], RoomOf(c));
            }
        }
    }
}

UObject* LoadFar(void* softPtr, const FTransform& at) {
    if (!g.streamingCdo || !softPtr || !g.world) return nullptr;
    Call c(g.streamingCdo, "LoadLevelInstanceBySoftObjectPtr");
    c.set<UObject*>("WorldContextObject", g.world).setFrom("Level", softPtr).set("Location", at.translation).set("Rotation", FRotator{})
        .set<bool>("bLoadAsTempPackage", false);
    if (!c.invoke()) return nullptr;
    return c.get<bool>("bOutSuccess") ? c.get<UObject*>("ReturnValue") : nullptr;
}

void UnloadFar() {
    for (int s = 0; s < 3; ++s) {
        UObject* st = g.job.streams[s];
        if (!Alive(st, g.job.index[s])) continue;
        Call(st, "SetIsRequestingUnloadAndRemoval").set<bool>("bInIsRequestingUnloadAndRemoval", true).invoke();
        Call(st, "SetShouldBeLoaded").set<bool>("bInShouldBeLoaded", false).invoke();
    }
    g.job = FarJob{};
    g.farBusy = false;
}

void FarTick(uint32_t now) {
    if (!g.farBusy) {
        if (g.farQueue.empty()) {
            if (!g.farDone) { g.farDone = true; HSM_LOG("cryptsprobe: far measure: done, %d room levels", g.farCount); }
            return;
        }
        if (int32_t(now - g.nextFar) < 0) return;
        const size_t i = g.farQueue.front();
        g.farQueue.erase(g.farQueue.begin());
        const RoomClass& r = g.rooms[i];
        g.job = FarJob{};
        g.job.room = i;
        g.job.at.translation = {kFarX + kFarStep * g.farCount, 0, 0};
        g.job.startMs = now;
        void* soft[3] = {FieldPtr(r.cdo, "Level"), FieldPtr(r.cdo, "Lighting Level"), FieldPtr(r.cdo, "Logic Level")};
        const std::string* path[3] = {&r.module, &r.lighting, &r.logic};
        for (int s = 0; s < 3; ++s) {
            if (path[s]->size() <= 1) continue;   // "-": the class names no such level
            g.job.streams[s] = LoadFar(soft[s], g.job.at);
            g.job.index[s] = g.job.streams[s] ? ObjectIndex(g.job.streams[s]) : -1;
        }
        ++g.farCount;
        g.farBusy = true;
        HSM_LOG("cryptsprobe: far measure %d: %s at %s (module %s, lighting %s, logic %s)", g.farCount, r.name.c_str(),
                V(g.job.at.translation).c_str(), g.job.streams[0] ? "loading" : "not loaded", g.job.streams[1] ? "loading" : "-",
                g.job.streams[2] ? "loading" : "-");
        return;
    }
    UObject* levels[3] = {};
    bool ready = true;
    for (int s = 0; s < 3; ++s) {
        if (!g.job.streams[s]) continue;
        if (!Alive(g.job.streams[s], g.job.index[s])) { g.job.streams[s] = nullptr; continue; }
        levels[s] = ShownLevel(g.job.streams[s]);
        if (!levels[s]) ready = false;
    }
    const RoomClass& r = g.rooms[g.job.room];
    if (!ready && now - g.job.startMs < kFarTimeoutMs) return;
    if (!ready) HSM_LOG("cryptsprobe: far %s: not all levels shown after %us; measuring what is", r.name.c_str(), kFarTimeoutMs / 1000);
    static const char* const kPart[3] = {"module", "lighting", "logic"};
    for (int s = 0; s < 3; ++s)
        if (levels[s]) MeasureLevel(levels[s], g.job.at, "far " + r.name + " " + kPart[s], &r);
    HSM_LOG("cryptsprobe: far %s: shown after %ums", r.name.c_str(), now - g.job.startMs);
    UnloadFar();
    g.nextFar = now + kFarGapMs;
}

}  // namespace

void CryptsProbeLevel(bool on, bool cryptsLevel, UObject* world, uint32_t nowMs) {
    g = State{};   // the old world took its streaming levels (the far ones too) with it
    g.on = on && cryptsLevel;
    if (!g.on) return;
    g.world = world;
    g.armedAt = nowMs;
    g.blockCls = FindClass(kBlockClass);
    g.spawnerCls = FindClass(kSpawnerClass);
    g.doorCls = FindClass(kDoorClass);
    g.smaCls = FindClass(kStaticMeshActorClass);
    g.actorCls = FindClass(kActorClass);
    g.streamingCdo = DefaultObject(FindClass(kStreamingClass));
    FProperty* sp = g.blockCls ? FindProperty(g.blockCls, "Start Position") : nullptr;
    g.layoutOk = g.blockCls && g.actorCls && sp && PropertySize(sp) == int32_t(sizeof(FTransform));
    HSM_LOG("cryptsprobe: armed (room class %s, transform %d bytes, spawner %s, door %s, streaming %s)", g.blockCls ? "found" : "MISSING",
            sp ? PropertySize(sp) : -1, g.spawnerCls ? "found" : "missing", g.doorCls ? "found" : "missing", g.streamingCdo ? "found" : "missing");
    if (!g.layoutOk) { HSM_LOG("cryptsprobe: off: the room class or its transform layout is not as expected"); g.on = false; }
}

void CryptsProbeTick(uint32_t now) {
    if (!g.on) return;
    if (!g.catalogued) {
        if (now - g.armedAt < kCatalogAfterMs) return;
        Catalog();
        g.nextScan = g.nextFar = now;
    }
    if (int32_t(now - g.nextScan) >= 0) { g.nextScan = now + kScanGapMs; ScanLive(now); }
    FarTick(now);
}

}  // namespace hsm::game
