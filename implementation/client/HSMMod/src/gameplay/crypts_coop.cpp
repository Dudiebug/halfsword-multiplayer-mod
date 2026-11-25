#include "gameplay/crypts_coop.h"
#include "log.h"
#include "session/hsm_crypt_enemies.h"
#include "session/hsm_crypt_spawn.h"
#include "session/hsm_crypt_walk.h"
#include "ue/hook.h"
#include "ue/script_hook.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <random>
#include <unordered_map>
#include <unordered_set>

using namespace hsm::ue;

namespace hsm::game::cryptscoop {
namespace {

constexpr const char* kBlockClass = "BP_Procedural_Block_Master_C";
constexpr const char* kDirectorClass = "BP_Procedural_Dungeon_Director_C";
constexpr const char* kRoomClassPrefix = "BP_Procedural_Block_";
constexpr const char* kScriptOwner = "cryptcoop";

constexpr uint32_t kScanMs = 500;        // the director's list
constexpr uint32_t kPollMs = 100;        // remote bodies against the rooms' boxes
constexpr uint32_t kLiveLogMs = 30000;
constexpr int kBlockLogsFirst = 3;
constexpr int kBlockLogsEvery = 50;
// The SpawnCharacters box fills a room with the host's enemies (its Event Populate Level With Characters). The host adopts each
// one as a bot (session.cpp ScanStockWillies) and the clients show it, so a remote body walking into the box starts it as the
// host's own pawn does.
constexpr bool kFireSpawnCharacters = true;

template <class T> struct ArrayView { T* data; int32_t num, max; };

using crypt::BoxKind;

enum Hk : uint8_t { kHkNone = 0, kHkSpawnLevelEnd, kHkFall, kHkSpawnLevel, kHkSpawnChars, kHkBox, kHkCount };
const char* const kHkName[kHkCount] = {"-", "SpawnLevel end overlap", "fall box", "SpawnLevel box", "SpawnCharacters box", "Box"};

struct Room {
    UObject* obj = nullptr;
    int32_t index = -1;
    uint16_t seq = 0;
    std::string cls;
    crypt::WorldBox box[3];
    bool boxRead[3] = {};
    bool boxMissingLogged[3] = {};
    bool gone = false;
    crypt::RoomSpawn spawn;   // crypts_host_spawns: the mod fills it (started when a player first stands in its SpawnCharacters box)
};

// Kept across levels: what the hooks learned about this build's functions.
struct Persist {
    std::function<bool(const UObject*)> isModFighter;
    std::unordered_map<UFunction*, uint8_t> hookKind;
    std::unordered_map<UFunction*, int32_t> otherActorOffset;
    bool ruleInstalled = false;
    bool hostSpawns = true;
};
Persist p;

struct State {
    bool shared = false;
    uint32_t armedAt = 0, nextScan = 0, nextPoll = 0, nextLive = 0;
    UObject* director = nullptr;
    int32_t directorIndex = -1;
    std::vector<Room> rooms;
    std::unordered_map<const UObject*, uint16_t> seqOf;
    std::unordered_set<const UObject*> skipped;   // a room the catalog does not know (and its children): logged once
    std::unordered_set<const UObject*> waiting;   // a room whose parent is not sent yet: logged once
    crypt::OnceTracker once;
    bool firing = false;          // our own call of a room's event: the observer does not count it
    bool observed = false;        // the engine's own overlap for a remote body, logged once
    int blocked[kHkCount + 3] = {};
    UFunction* fn[3] = {};        // the BndEvt of SpawnCharacters, SpawnLevel and Box (the master class's)
    UFunction* forceLoad = nullptr;
    bool fnResolved = false;
    size_t rooms0Logged = 0;
    int pruned = 0;               // entries taken out of the director's Spawned Willies
    int boxMissingLogs = 0;       // a room box not read yet (Poll reads it again): the first few logged, then counted
    std::unordered_map<const UObject*, bool> rolled;   // each spawner point's Spawn Chance roll, made once (as the game's one pass)
    int spawnedTotal = 0;
};
State g;

bool Alive(UObject* o, int32_t index) { return o && index >= 0 && ObjectAt(index) == o && !IsGarbage(o); }
std::string V(FVector v) { char b[96]; std::snprintf(b, sizeof b, "(%.0f,%.0f,%.0f)", v.x, v.y, v.z); return b; }

double YawOf(const FQuat& q) {
    constexpr double kDeg = 57.29577951308232;
    return std::atan2(2 * (q.w * q.z + q.x * q.y), 1 - 2 * (q.y * q.y + q.z * q.z)) * kDeg;
}

UObject** BoxField(UObject* room, int k) {
    switch (k) {
        case 0: return Field<UObject*>(room, "Box_Collision_SpawnCharacters");
        case 1: return Field<UObject*>(room, "Box_Collision_SpawnLevel");
        default: return Field<UObject*>(room, "Box");
    }
}
BoxKind KindOfBox(int k) { return k == 0 ? BoxKind::SpawnCharacters : k == 1 ? BoxKind::SpawnLevel : BoxKind::Box; }

// A box component's world pose and half extents (rooms never move once placed: read once).
bool ReadBox(UObject* room, int k, crypt::WorldBox& out) {
    UObject** c = BoxField(room, k);
    if (!c || !*c || IsGarbage(*c)) return false;
    Call l(*c, "K2_GetComponentLocation");
    Call r(*c, "K2_GetComponentRotation");
    Call e(*c, "GetScaledBoxExtent");
    if (!l.invoke() || !r.invoke() || !e.invoke()) return false;
    const FVector at = l.get<FVector>("ReturnValue"), ex = e.get<FVector>("ReturnValue");
    if (!(ex.x > 0 && ex.y > 0 && ex.z > 0) || !std::isfinite(at.x + at.y + at.z)) return false;
    out.centre = {at.x, at.y, at.z};
    out.hx = ex.x; out.hy = ex.y; out.hz = ex.z;
    out.yaw = r.get<FRotator>("ReturnValue").yaw;
    return true;
}

UObject* FindDirector(const Env& env) {
    if (g.director && Alive(g.director, g.directorIndex)) return g.director;
    g.director = nullptr; g.directorIndex = -1;
    UObject* d = nullptr;
    if (env.gameInstance)
        if (UObject** dir = Field<UObject*>(env.gameInstance, "Current Procedural Dungeon Director"); dir && *dir && !IsGarbage(*dir)) d = *dir;
    if (!d) {   // the game instance does not name it yet: one look at the instances, at most every scan
        const std::vector<UObject*> all = FindInstances(kDirectorClass);
        if (!all.empty()) d = all.front();
    }
    if (d) {
        g.director = d; g.directorIndex = ObjectIndex(d);
        HSM_LOG("crypt room: the dungeon director is %s", ObjectName(d).c_str());
    }
    return d;
}

// What the host's room is, as the wire says it. False: not readable yet (try again) or not a room the catalog knows (skipped).
enum class Read { Ok, Later, Skip };
Read ReadRoom(UObject* o, crypt::CryptRoomMsg& m, std::string& cls) {
    cls = ObjectName(reinterpret_cast<UObject*>(ClassOf(o)));
    const int cat = CatalogOfClass(cls);
    if (cat < 0) return Read::Skip;
    const FTransform* origin = static_cast<const FTransform*>(FieldPtr(o, "Origin Point"));
    if (!origin || !(origin->scale.x > 0.5) || !std::isfinite(origin->translation.x + origin->translation.y + origin->translation.z)) return Read::Later;
    m = crypt::CryptRoomMsg{};
    m.catalog = uint8_t(cat);
    m.x = float(origin->translation.x); m.y = float(origin->translation.y); m.z = float(origin->translation.z);
    double yaw = YawOf(origin->rotation);
    yaw = std::fmod(yaw + 180.0, 360.0); if (yaw < 0) yaw += 360.0; yaw -= 180.0;
    m.yaw = float(yaw);
    const int32_t* gen = Field<int32_t>(o, "Generation");
    m.generation = uint8_t(std::clamp<int32_t>(gen ? *gen : 0, 0, crypt::kMaxGeneration));
    const uint8_t* tier = Field<uint8_t>(o, "Room Tier");
    m.tier = uint8_t(std::min<int>(tier ? *tier : 0, crypt::kMaxTier));
    bool b = false;
    if (GetBool(o, "Is Entrance?", b) && b) m.flags |= crypt::kRoomEntrance;
    if (GetBool(o, "Is Boss Arena?", b) && b) m.flags |= crypt::kRoomBoss;
    if ((GetBool(o, "Is Exit?", b) && b) || (GetBool(o, "Leads To Exit", b) && b)) m.flags |= crypt::kRoomExit;
    UObject** parent = Field<UObject*>(o, "My Parent");
    UObject* par = parent && *parent && !IsGarbage(*parent) ? *parent : nullptr;
    if (!par) { m.parentSeq = 0; m.startEntrance = crypt::kNoEntrance; return Read::Ok; }
    if (g.skipped.count(par)) return Read::Skip;
    auto it = g.seqOf.find(par);
    if (it == g.seqOf.end()) return Read::Later;
    m.parentSeq = it->second;
    // Which of this room's entrances joins its parent: the one nearest the doorway the parent placed it at (Start Position).
    const crypts::RoomClass& rc = crypts::kRooms[m.catalog];
    const FTransform* start = static_cast<const FTransform*>(FieldPtr(o, "Start Position"));
    const crypts::Pose org{m.x, m.y, m.z, m.yaw};
    double best = 1e300; uint8_t pick = 0;
    for (uint8_t e = 0; e < rc.entranceCount; ++e) {
        const crypts::Pose w = crypts::ToWorld(org, rc.entrances[e]);
        const double d = start ? std::hypot(w.x - start->translation.x, w.y - start->translation.y) : 0.0;
        if (d < best) { best = d; pick = e; }
    }
    m.startEntrance = pick;
    if (start && best > crypts::kJoinRadius * 4)
        HSM_LOG("crypt room: %s: its nearest entrance is %.0f cm from the doorway the parent placed it at; entrance %u taken", cls.c_str(), best, unsigned(pick));
    return Read::Ok;
}

void Scan(const Env& env) {
    UObject* dir = FindDirector(env);
    if (!dir) return;
    const auto* list = static_cast<const ArrayView<UObject*>*>(FieldPtr(dir, "Active Levels"));
    if (!list || !list->data || list->num <= 0 || list->num > 4096) return;
    for (int32_t i = 0; i < list->num; ++i) {
        UObject* o = list->data[i];
        if (!o || IsGarbage(o) || g.seqOf.count(o) || g.skipped.count(o)) continue;
        crypt::CryptRoomMsg m;
        std::string cls;
        const Read r = ReadRoom(o, m, cls);
        if (r == Read::Skip) {
            g.skipped.insert(o);
            HSM_LOG("crypt room: %s (%s) is not in the room catalog (or its parent is not); it and its children are not sent", ObjectName(o).c_str(), cls.c_str());
            continue;
        }
        if (r == Read::Later) {
            if (g.waiting.insert(o).second) HSM_LOG("crypt room: %s (%s) is not ready to send yet (origin or parent)", ObjectName(o).c_str(), cls.c_str());
            continue;
        }
        const uint16_t seq = env.onRoom ? env.onRoom(m) : 0;
        if (!seq) { g.skipped.insert(o); continue; }
        Room room;
        room.obj = o; room.index = ObjectIndex(o); room.seq = seq; room.cls = cls;
        g.rooms.push_back(room);
        g.seqOf.emplace(o, seq);
    }
}

ArrayView<UObject*>* SpawnedList() {
    if (!g.director || !Alive(g.director, g.directorIndex)) return nullptr;
    auto* list = static_cast<ArrayView<UObject*>*>(FieldPtr(g.director, "Spawned Willies"));
    if (!list || !list->data || list->num < 0 || list->num > 4096) return nullptr;
    return list;
}

// Once per scan: a dead or destroyed fighter leaves the director's list, so its cap of 3 or 4 counts the living.
void PruneSpawned(const Env& env) {
    ArrayView<UObject*>* list = SpawnedList();
    if (!list || list->num == 0) return;
    const int before = list->num;
    const int n = crypt::PruneList(list->data, list->num, [&](UObject* w) {
        return !w || IsGarbage(w) || (env.isDead && env.isDead(w));
    });
    if (!n) return;
    g.pruned += n;
    HSM_LOG("crypt coop: the director's Spawned Willies: %d dead or gone fighter(s) taken out (%d -> %d; %d so far)", n, before, int(list->num), g.pruned);
}

void ResolveFunctions() {
    g.fnResolved = true;
    UClass* master = FindClass(kBlockClass);
    if (!master) return;
    static const char* const kPrefix[3] = {"BndEvt__BP_Procedural_Block_Master_Box_Collision_SpawnCharacters",
                                           "BndEvt__BP_Procedural_Block_Master_Box_Collision_SpawnLevel",
                                           "BndEvt__BP_Procedural_Block_Master_Box_K2Node"};
    for (int k = 0; k < 3; ++k) {
        // A box has a begin and an end overlap event under one prefix: the begin one is the room's "someone came in".
        for (UFunction* f : FunctionsWithPrefix(master, kPrefix[k])) {
            const std::string n = ObjectName(reinterpret_cast<UObject*>(f));
            if (n.find("EndOverlap") != std::string::npos) continue;
            if (!g.fn[k] || n.find("BeginOverlap") != std::string::npos) g.fn[k] = f;
        }
    }
    g.forceLoad = FindFunction(master, "Event Force Load");
    HSM_LOG("crypt room: room events: SpawnCharacters %s, SpawnLevel %s, Box %s, Event Force Load %s", g.fn[0] ? "found" : "MISSING", g.fn[1] ? "found" : "MISSING",
            g.fn[2] ? "found" : "MISSING", g.forceLoad ? "found" : "missing");
}

// The room's own event for a box, called with the host's pawn (what its gate compares with GetPlayerCharacter(0)).
bool Fire(Room& r, int k, const Env& env) {
    if (!env.hostPawn) return false;
    if (!g.fnResolved) ResolveFunctions();
    g.firing = true;
    bool ok = false;
    if (g.fn[k]) {
        Call c(r.obj, g.fn[k]);
        c.set<UObject*>("OtherActor", env.hostPawn);
        ok = c.invoke();
    } else if (k == 1 && g.forceLoad) {
        ok = Call(r.obj, g.forceLoad).invoke();
    }
    g.firing = false;
    return ok;
}

// Every player's body, the host's first.
std::vector<Remote> Players(const Env& env) {
    std::vector<Remote> all;
    if (env.hostPawn && !IsGarbage(env.hostPawn)) all.push_back({0, env.hostName.empty() ? std::string("the host") : env.hostName, env.hostPawn});
    for (const Remote& r : env.remotes) if (r.actor && !IsGarbage(r.actor)) all.push_back(r);
    return all;
}

void SpawnRoom(Room& r, const Env& env, uint32_t now);

void Poll(const Env& env, uint32_t now) {
    if (g.rooms.empty() || (env.remotes.empty() && !p.hostSpawns)) return;
    for (Room& r : g.rooms) {
        if (r.gone) continue;
        if (!Alive(r.obj, r.index)) {
            r.gone = true;
            HSM_LOG("crypt room: #%u %s is gone", unsigned(r.seq), r.cls.c_str());
            continue;
        }
        for (int k = 0; k < 3; ++k) {
            if (!r.boxRead[k]) {
                if (!ReadBox(r.obj, k, r.box[k])) {
                    if (!r.boxMissingLogged[k]) {
                        r.boxMissingLogged[k] = true;
                        if (++g.boxMissingLogs <= 3 || g.boxMissingLogs % 50 == 0)
                            HSM_LOG("crypt room: #%u %s: its %s box could not be read yet; read again each poll (%d so far)", unsigned(r.seq),
                                    r.cls.c_str(), crypt::BoxName(KindOfBox(k)), g.boxMissingLogs);
                    }
                    continue;
                }
                r.boxRead[k] = true;
            }
        }
    }
    for (const Remote& who : Players(env)) {
        const bool host = who.actor == env.hostPawn;
        Call l(who.actor, "K2_GetActorLocation");
        if (!l.invoke()) continue;
        const FVector at = l.get<FVector>("ReturnValue");
        const Point3 pos{at.x, at.y, at.z};
        for (Room& r : g.rooms) {
            if (r.gone) continue;
            for (int k = 0; k < 3; ++k) {
                if (!r.boxRead[k] || !crypt::InBox(r.box[k], pos)) continue;
                const BoxKind kind = KindOfBox(k);
                if (k == 0 && p.hostSpawns) {   // the mod fills the room, for whoever walked in (SpawnRoom)
                    if (r.spawn.started) continue;
                    r.spawn.started = true; r.spawn.next = now;
                    HSM_LOG("crypt coop: #%u %s: %s entered the SpawnCharacters box at %s; the host fills the room", unsigned(r.seq), r.cls.c_str(),
                            who.name.c_str(), V(at).c_str());
                    continue;
                }
                if (host) continue;             // the host's own overlap fires the room's other events itself
                if (g.once.Seen(r.seq, kind, 0)) continue;
                if (k == 0 && !kFireSpawnCharacters) {
                    g.once.First(r.seq, kind, 0);
                    HSM_LOG("crypt room: #%u %s: %s entered the %s box at %s (its fighters are not started)", unsigned(r.seq), r.cls.c_str(), who.name.c_str(),
                            crypt::BoxName(kind), V(at).c_str());
                    continue;
                }
                if (!Fire(r, k, env)) continue;   // no host pawn yet, or the event is missing: asked again next poll
                g.once.First(r.seq, kind, 0);
                HSM_LOG("crypt room: #%u %s: %s entered the %s box at %s; the room's own event called with the host's pawn", unsigned(r.seq), r.cls.c_str(),
                        who.name.c_str(), crypt::BoxName(kind), V(at).c_str());
                if (k == 0) HSM_LOG("crypt coop: #%u %s: its enemies started for %s (director list: %d)", unsigned(r.seq), r.cls.c_str(), who.name.c_str(), SpawnedWillies());
            }
        }
    }
    for (Room& r : g.rooms)
        if (!r.gone && r.spawn.Due(now)) SpawnRoom(r, env, now);
}

// ---- the host fills a room (crypts_host_spawns; session/hsm_crypt_spawn.h)

std::mt19937& Rng() { static std::mt19937 rng{std::random_device{}()}; return rng; }

FVector ActorAt(UObject* a) {
    Call l(a, "K2_GetActorLocation");
    return l.invoke() ? l.get<FVector>("ReturnValue") : FVector{};
}

// The director's Spawn Character, as the room's own event calls it (BP_Procedural_Block_Master.cpp:684/699).
UObject* SpawnAt(const FVector& at, double radius, int team, bool boss) {
    Call c(g.director, "Spawn Character");
    if (!c.valid()) return nullptr;
    c.set<FVector>("Origin Location", at).setReal("Spawn Radius", radius).set<bool>("Zombie", false).set<int32_t>("Team", team)
        .set<bool>("Boss", boss).set<bool>("Go To Near Player", false);
    if (!c.invoke()) return nullptr;
    UObject* w = c.get<UObject*>("Spawned Character");
    return w && !IsGarbage(w) ? w : nullptr;
}

void SpawnRoom(Room& r, const Env& env, uint32_t now) {
    if (!g.director || !Alive(g.director, g.directorIndex) || !Alive(r.obj, r.index)) { r.spawn.next = now + crypt::kSpawnRetryMs; return; }
    bool b = false;
    if (GetBool(r.obj, "Door Barricated", b) && b) {   // the game never fills a barricaded room on entry either
        r.spawn.done = true;
        HSM_LOG("crypt coop: #%u %s: its door is barricaded; not filled on entry (as the game's)", unsigned(r.seq), r.cls.c_str());
        return;
    }
    const auto* list = static_cast<const ArrayView<UObject*>*>(FieldPtr(g.director, "Available Spawners"));
    std::vector<UObject*> all;
    if (list && list->data && list->num > 0 && list->num <= 4096)
        for (int32_t i = 0; i < list->num; ++i) if (UObject* s = list->data[i]; s && !IsGarbage(s)) all.push_back(s);
    // The room's points: those in its boxes; failing that the game's own filter, every point in the room's front half.
    std::vector<UObject*> mine;
    std::vector<Point3> pts;
    std::vector<FVector> at;
    std::vector<FVector> where;
    for (UObject* s : all) where.push_back(ActorAt(s));
    for (size_t i = 0; i < all.size(); ++i) {
        const Point3 q{where[i].x, where[i].y, where[i].z};
        bool in = false;
        for (int k = 0; k < 3 && !in; ++k) in = r.boxRead[k] && crypt::InBox(r.box[k], q, 100);
        if (in) { mine.push_back(all[i]); pts.push_back(q); at.push_back(where[i]); }
    }
    const char* how = "in its boxes";
    if (mine.empty()) {
        how = "in its front half (none in its boxes)";
        const FVector ra = ActorAt(r.obj);
        Call rr(r.obj, "K2_GetActorRotation");
        const double yaw = rr.invoke() ? rr.get<FRotator>("ReturnValue").yaw : 0.0;
        for (size_t i = 0; i < all.size(); ++i) {
            const Point3 q{where[i].x, where[i].y, where[i].z};
            if (crypt::InFront({ra.x, ra.y, ra.z}, yaw, q)) { mine.push_back(all[i]); pts.push_back(q); at.push_back(where[i]); }
        }
    }
    const float* chanceF = Field<float>(r.obj, "Spawn Chance");
    const double chance = chanceF && std::isfinite(*chanceF) ? std::clamp(double(*chanceF), 0.0, 1.0) : 0.5;
    std::vector<bool> passed;
    for (UObject* s : mine) {
        auto it = g.rolled.find(s);
        if (it == g.rolled.end()) it = g.rolled.emplace(s, std::bernoulli_distribution(chance)(Rng())).first;
        passed.push_back(it->second);
    }
    std::vector<Point3> ppos;
    std::vector<FVector> pat;
    std::vector<UObject*> bodies;
    for (const Remote& who : Players(env)) { const FVector v = ActorAt(who.actor); ppos.push_back({v.x, v.y, v.z}); pat.push_back(v); bodies.push_back(who.actor); }
    auto sees = [&](size_t i, size_t j) {
        if (!env.lineClear) return false;
        const FVector from{at[i].x, at[i].y, at[i].z + crypt::kSpawnTraceUpCm};
        return env.lineClear(from, pat[j], bodies);
    };
    bool bossArena = false;
    GetBool(r.obj, "Is Boss Arena?", bossArena);
    UObject** bossField = Field<UObject*>(r.obj, "Spawned Boss");
    auto bossDue = [&] { return bossArena && bossField && (!*bossField || IsGarbage(*bossField)); };
    const bool bossFirst = bossDue();
    const int living = std::max(0, SpawnedWillies());
    const crypt::SpawnPlan plan = crypt::PlanSpawns(pts, passed, ppos, living, sees, crypt::kSpawnClearCm,
                                                    bossFirst ? std::max(crypt::kDirectorCap, living + 1) : crypt::kDirectorCap);   // the boss is past the cap
    // What the room's blocked event would have set: the team its fighters are on (the game's reinforcements read it too).
    const uint8_t* tierB = Field<uint8_t>(g.director, "Current Abyss Tier");
    const int tier = tierB ? *tierB : 0;
    const int team = crypt::RoomTeam(tier, bossArena, [](int n) { return std::uniform_int_distribution<int>(0, n - 1)(Rng()); });
    if (int32_t* t = Field<int32_t>(r.obj, "Spawned Characters Team")) *t = team;
    SetBool(r.obj, "Spawned Reinforcements", false);
    const float* radiusF = Field<float>(r.obj, "Spawn Radius");
    const double radius = radiusF && std::isfinite(*radiusF) ? *radiusF : 0.0;
    int spawned = 0;
    auto one = [&](size_t i) {
        const bool boss = bossDue();
        UObject* w = SpawnAt(at[i], radius, team, boss);
        if (!w) return;
        ++spawned;
        if (boss) *bossField = w;
    };
    for (size_t i : plan.use) one(i);
    const crypt::SpawnStep step = crypt::NextStep(r.spawn, plan, spawned, now);
    if (step == crypt::SpawnStep::Fallback)
        if (const std::optional<size_t> i = crypt::FallbackPoint(pts, plan, ppos)) one(*i);
    g.spawnedTotal += spawned;
    HSM_LOG("crypt coop: #%u %s: %d of %zu point(s) %s spawned (try %d: skipped %d by roll, %d near a player, %d seen, %d over the cap); "
            "team %d, tier %d%s; director list %d -> %d; %s",
            unsigned(r.seq), r.cls.c_str(), spawned, pts.size(), how, r.spawn.tries, plan.chance, plan.nearby, plan.seen, plan.capped, team, tier,
            bossFirst ? ", boss arena" : "", living, SpawnedWillies(), crypt::SpawnStepName(step));
}

// ---- the hooks

uint8_t HookKindOf(UFunction* fn) {
    auto it = p.hookKind.find(fn);
    if (it != p.hookKind.end()) return it->second;
    const std::string n = ObjectName(reinterpret_cast<UObject*>(fn));
    uint8_t k = kHkNone;
    if (n.find("Box_Collision_SpawnLevel") != std::string::npos)
        k = n.find("EndOverlap") != std::string::npos ? kHkSpawnLevelEnd : kHkSpawnLevel;
    else if (n.find("Destroy_Fallen_Actors") != std::string::npos || n.find("Destroy Fallen Actors") != std::string::npos) k = kHkFall;
    else if (n.find("Box_Collision_SpawnCharacters") != std::string::npos) k = kHkSpawnChars;
    else if (n.find("Master_Box_K2Node") != std::string::npos) k = kHkBox;
    p.hookKind.emplace(fn, k);
    return k;
}

UObject* OtherActorOf(UFunction* fn, void* params) {
    if (!params) return nullptr;
    auto it = p.otherActorOffset.find(fn);
    int32_t off = -1;
    if (it != p.otherActorOffset.end()) off = it->second;
    else {
        FProperty* pr = FindProperty(fn, "OtherActor");
        off = pr ? PropertyOffset(pr) : -1;
        p.otherActorOffset.emplace(fn, off);
    }
    if (off < 0) return nullptr;
    return *reinterpret_cast<UObject**>(static_cast<uint8_t*>(params) + off);
}

void NoteBlock(int what, const char* label) {
    const int n = g.blocked[what]++;
    if (n < kBlockLogsFirst || n % kBlockLogsEvery == 0)
        HSM_LOG("crypt room: blocked %s (%d so far; the shared dungeon keeps its rooms and its remote players)", label, n + 1);
}

bool OnBound(UObject*, UFunction* fn, void* params) {
    if (!g.shared || g.firing) return false;
    const uint8_t k = HookKindOf(fn);
    if (k == kHkNone) return false;
    if (k == kHkSpawnLevelEnd) { NoteBlock(k, kHkName[k]); return true; }   // the host walking out of a room's box unloads its parent room
    if (k == kHkSpawnChars && p.hostSpawns) { NoteBlock(k, "a room's own SpawnCharacters event (the host fills the room for every player)"); return true; }
    UObject* other = OtherActorOf(fn, params);
    if (!other) return false;
    const bool mod = p.isModFighter && p.isModFighter(other);
    if (k == kHkFall) {
        if (!mod) return false;
        NoteBlock(k, "the fall box's destroy of a remote fighter");
        return true;
    }
    if (mod && !g.observed) {   // the engine's own overlap reached a remote body: the poll would not be needed
        g.observed = true;
        HSM_LOG("crypt room: overlap fired for stand-in %s (%s)", ObjectName(other).c_str(), kHkName[k]);
    }
    return false;
}

}  // namespace

int CatalogOfClass(const std::string& className) {
    std::string n = className;
    if (n.rfind(kRoomClassPrefix, 0) != 0) return -1;
    n = n.substr(std::strlen(kRoomClassPrefix));
    if (n.size() > 2 && n.compare(n.size() - 2, 2, "_C") == 0) n.resize(n.size() - 2);
    const crypts::RoomClass* rc = crypts::FindRoom(n);
    return rc ? int(rc - crypts::kRooms) : -1;
}

void InstallRule() {
    if (p.ruleInstalled) return;
    p.ruleInstalled = true;
    HookFunctionsMatching([](const std::string& n) {
        return n.rfind("BndEvt__BP_Procedural_Block_", 0) == 0 || n.rfind("BndEvt__NewBlueprint3_Box_Collision_SpawnLevel", 0) == 0;
    }, [](UObject* obj, UFunction* fn, void* params) { return OnBound(obj, fn, params); });
}

void InstallScripts() {
    ClearScriptHooks(kScriptOwner);
    auto block = [](int what, const char* label) {
        return [what, label](ScriptCall&) {
            if (!g.shared) return false;
            NoteBlock(what, label);
            return true;
        };
    };
    // Literal names, one call each: tools/check_reflection.py verifies HookScript names against the game. The dungeon never
    // unloads a room in v1 (a room streaming out crashed the game, REVERSE_ENGINEERING section 14); the director's area clear
    // destroys every Willie but local players 0 and 1, which a remote player's body is not.
    int n = HookScript(kScriptOwner, "BP_Procedural_Block_Master_C", "Event Unload Level", block(kHkCount, "Event Unload Level"));
    n += HookScript(kScriptOwner, "BP_Procedural_Block_Master_C", "Destroy Level", block(kHkCount + 1, "Destroy Level"));
    n += HookScript(kScriptOwner, "BP_Procedural_Dungeon_Director_C", "Destroy Actors in Area", block(kHkCount + 2, "Destroy Actors in Area"));
    HSM_LOG("crypt room: shared-run blocks %s (%d function(s) hooked now)", ScriptHookActive() ? "on" : "NOT ACTIVE (no script VM hook)", n);
}

void RemoveScripts() { ClearScriptHooks(kScriptOwner); }

void SetModFighterTest(std::function<bool(const UObject*)> f) { p.isModFighter = std::move(f); }

void SetHostSpawns(bool on) { p.hostSpawns = on; }

void OnLevel(bool shared, uint32_t nowMs) {
    g = State{};
    g.shared = shared;
    g.armedAt = g.nextScan = g.nextPoll = nowMs;
    g.nextLive = nowMs + kLiveLogMs;
    if (shared) HSM_LOG("crypt room: the host reads its dungeon (rooms from the director's Active Levels, remote bodies walked through them)");
}

bool Armed() { return g.shared; }

uint16_t RoomSeqAt(double x, double y, double z) {
    const Point3 p{x, y, z};
    uint16_t nearest = 0; double nearestD = 1e30;
    for (int pass = 0; pass < 2; ++pass) {
        for (int k : {1, 2, 0}) {
            for (Room& r : g.rooms) {
                if (r.gone || r.seq == 0 || !Alive(r.obj, r.index)) continue;
                if (!r.boxRead[k]) r.boxRead[k] = ReadBox(r.obj, k, r.box[k]);
                if (!r.boxRead[k]) continue;
                if (pass == 0) {
                    if (crypt::InBox(r.box[k], p)) return r.seq;
                } else {
                    const double d = std::sqrt((r.box[k].centre.x - x) * (r.box[k].centre.x - x) + (r.box[k].centre.y - y) * (r.box[k].centre.y - y) +
                                               (r.box[k].centre.z - z) * (r.box[k].centre.z - z));
                    if (d < nearestD) { nearestD = d; nearest = r.seq; }
                }
            }
        }
    }
    return nearest;
}

bool InEntranceRoom(double x, double y, double z) {
    bool any = false;
    for (Room& r : g.rooms) {
        if (r.gone || !Alive(r.obj, r.index)) continue;
        bool entrance = false;
        if (!GetBool(r.obj, "Is Entrance?", entrance) || !entrance) continue;
        for (int k = 1; k <= 2; ++k) {
            if (!r.boxRead[k]) r.boxRead[k] = ReadBox(r.obj, k, r.box[k]);
            if (!r.boxRead[k]) continue;
            any = true;
            if (crypt::InBox(r.box[k], Point3{x, y, z})) return true;
        }
    }
    return !any;
}

int SpawnedWillies() {
    const ArrayView<UObject*>* list = SpawnedList();
    return list ? int(list->num) : -1;
}

int ForgetEnemy(UObject* willie) {
    if (!g.shared || !willie) return 0;
    ArrayView<UObject*>* list = SpawnedList();
    if (!list) return 0;
    const int n = crypt::PruneList(list->data, list->num, [&](UObject* w) { return w == willie; });
    g.pruned += n;
    return n;
}

int Live() {
    int n = 0;
    for (const Room& r : g.rooms) n += !r.gone && Alive(r.obj, r.index);
    return n;
}

void Tick(uint32_t now, const Env& env) {
    if (!g.shared) return;
    if (int32_t(now - g.nextScan) >= 0) { g.nextScan = now + kScanMs; Scan(env); PruneSpawned(env); }
    if (int32_t(now - g.nextPoll) >= 0) { g.nextPoll = now + kPollMs; Poll(env, now); }
    if (int32_t(now - g.nextLive) >= 0) {
        g.nextLive = now + kLiveLogMs;
        HSM_LOG("crypt room: %d live (%zu sent, %zu remote body/bodies)", Live(), g.rooms.size(), env.remotes.size());
        HSM_LOG("crypt coop: director list holds %d fighter(s); %d taken out so far; %d spawned by the host for its players (host spawns %s)",
                SpawnedWillies(), g.pruned, g.spawnedTotal, p.hostSpawns ? "on" : "off");
    }
}

}  // namespace hsm::game::cryptscoop
