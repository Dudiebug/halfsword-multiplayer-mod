#include "gameplay/crypts_loot.h"
#include "gameplay/adapter.h"
#include "gameplay/crypts_coop.h"
#include "gameplay/crypts_layout.h"
#include "log.h"
#include "ue/hook.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <map>
#include <string>
#include <unordered_map>
#include <utility>

using namespace hsm::ue;

namespace hsm::game::cryptsloot {
namespace {

using crypt::CryptDoorMsg;
using crypt::CryptLootMsg;

enum Kind : uint8_t { kNone = 0, kPoint = 1, kContainer = 2, kItem = 3 };

constexpr const char* kDoorClass[3] = {"BP_Door_Gate_A_001_C", "BP_TearDown_Barricade_Door_C", "BP_Door_Barricade_Unbreakable_C"};
constexpr const char* kDoorPath[3] = {
    "/Game/Assets/Environments/Modules/Doors/Blueprint/BP_Door_Gate_A_001.BP_Door_Gate_A_001_C",
    "/Game/Blueprints/Procedural/Structures/BP_TearDown_Barricade_Door.BP_TearDown_Barricade_Door_C",
    "/Game/Blueprints/Procedural/Structures/BP_Door_Barricade_Unbreakable.BP_Door_Barricade_Unbreakable_C"};
constexpr const char* kDoorName[3] = {"gate", "tear-down barricade", "unbreakable barricade"};
constexpr const char* kDoorStateName[3] = {"closed", "open", "gone"};

constexpr double kPointMatchCm = 200.0;      // a product begins play this close to the loot point that made it
constexpr int64_t kPointKeepMs = 5000;
constexpr double kContainerMatchCm = 350.0;  // a container's contents spawn at its spawn points (PR D2): items this close, within kPointKeepMs of its BeginPlay
constexpr int kExportsPerTick = 2;           // each weapon or armour export is a save to disk (PR D2)
constexpr int kExportTries = 3;
constexpr size_t kMaxPoints = 64;
constexpr uint32_t kCandidateGiveUpMs = 30000;
constexpr uint32_t kTakenPollMs = 300;
constexpr uint32_t kDoorScanMs = 1500;
constexpr int kSpawnsPerTick = 3;
constexpr int kBlockLogs = 4;
constexpr double kArmBarricadeCm = 500.0;    // a remote body this close arms a tear-down barricade (the game arms it for player 0's overlap)
constexpr int32_t kPlanksToBreak = 3;
constexpr double kClaimNearCm = 400.0;       // a client's claim that it unlocked a gate: a remote body this close to it

int64_t SteadyMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

struct Ref {
    UObject* obj = nullptr;
    int32_t index = -1;
};
Ref MakeRef(UObject* o) { return {o, o ? ObjectIndex(o) : -1}; }
bool Alive(const Ref& r) { return r.obj && r.index >= 0 && ObjectAt(r.index) == r.obj && !IsGarbage(r.obj); }

// A loot point makes one product; a container makes several (its contents), within `reach` of it.
struct Point { FVector at; int64_t ms; double reach = kPointMatchCm; bool multi = false; };
struct Candidate { Ref obj; std::string cls; uint32_t sinceMs; uint8_t itemKind = crypt::kItemNone; int tries = 0; };
struct Tracked { Ref obj; FVector origin; bool spawnedHere = false; double homeCm = crypt::kLootTakenCm; };

struct State {
    crypt::LootBook loot;
    crypt::DoorBook doors;
    // A class's kind, with its name: a level or room unload frees a class, and a new one at the same address (the pre56 test's
    // Cabinet loot point, read as a container) must not take the old one's kind.
    struct Kind { uint8_t kind; std::string name; };
    std::unordered_map<UClass*, Kind> kindOf;
    std::vector<Point> points;                 // host: loot points that began play lately
    std::vector<Candidate> candidates;         // host: products waiting for their room
    std::map<uint32_t, Tracked> track;         // (room << 16 | index): the actor of a record on this machine
    std::map<uint32_t, int> spawnFails;
    std::vector<Ref> destroyQ;                 // client: loot points to destroy
    std::map<uint16_t, Ref> doorActor;         // seq: the actor on this machine
    std::map<uint16_t, int> doorFails;
    uint32_t nowMs = 0, nextTaken = 0, nextDoorScan = 0;
    int blocked[2] = {};
    bool armedBarricadeLogged = false;
    Env env;
    bool haveEnv = false;
};
State g;

uint32_t Key(uint16_t room, uint16_t index) { return (uint32_t(room) << 16) | index; }
std::string V(FVector v) { char b[96]; std::snprintf(b, sizeof b, "(%.0f,%.0f,%.0f)", v.x, v.y, v.z); return b; }
double Dist(FVector a, FVector b) { return std::sqrt((a.x - b.x) * (a.x - b.x) + (a.y - b.y) * (a.y - b.y) + (a.z - b.z) * (a.z - b.z)); }
std::string Leaf(const std::string& path) { const size_t d = path.rfind('.'); return d == std::string::npos ? path : path.substr(d + 1); }

bool ReadAt(UObject* o, FVector& at) {
    Call l(o, "K2_GetActorLocation");
    if (!l.invoke()) return false;
    at = l.get<FVector>("ReturnValue");
    return std::isfinite(at.x + at.y + at.z);
}
FRotator ReadRot(UObject* o) {
    Call r(o, "K2_GetActorRotation");
    return r.invoke() ? r.get<FRotator>("ReturnValue") : FRotator{};
}
void Destroy(UObject* o) { if (o && !IsGarbage(o)) Call(o, "K2_DestroyActor").invoke(); }

// The class path of an actor's class, without the "Class " the full name starts with.
std::string ClassPathOf(UObject* o) {
    const std::string full = FullName(reinterpret_cast<UObject*>(ClassOf(o)));
    const size_t sp = full.find(' ');
    return sp == std::string::npos ? full : full.substr(sp + 1);
}

uint8_t KindOfClass(UClass* c) {
    if (!c) return kNone;
    const std::string own = ObjectName(reinterpret_cast<UObject*>(c));
    auto it = g.kindOf.find(c);
    if (it != g.kindOf.end() && it->second.name == own) return it->second.kind;
    uint8_t k = kNone;
    for (UClass* s = c; s && k == kNone; s = SuperOf(s)) {
        const std::string n = ObjectName(reinterpret_cast<UObject*>(s));
        if (crypt::IsLootPointClass(n)) k = kPoint;
        else if (crypt::IsRollingContainerClass(n)) k = kContainer;
        else if (crypt::IsItemClass(n)) k = kItem;
    }
    g.kindOf[c] = {k, own};
    return k;
}

enum class Mode { Off, Host, Client };
Mode ModeNow() {
    if (cryptscoop::Armed()) return Mode::Host;
    if (cryptslayout::CoopArmed() || cryptslayout::CoopLive()) return Mode::Client;
    return Mode::Off;
}

// A BeginPlay on any actor: the host notes a loot point and the product that follows it; the client blocks the dice.
bool OnBeginPlay(UObject* obj) {
    const Mode mode = ModeNow();
    if (mode == Mode::Off) return false;
    UClass* cls = ClassOf(obj);
    const uint8_t k = KindOfClass(cls);
    if (k == kNone) return false;
    if (mode == Mode::Client) {
        if (k == kItem) return false;
        const int slot = k == kPoint ? 0 : 1;
        if (g.blocked[slot]++ < kBlockLogs)
            HSM_LOG("crypt coop: loot: blocked %s.ReceiveBeginPlay (%s; the host's loot is spawned instead)", ObjectName(reinterpret_cast<UObject*>(cls)).c_str(),
                    k == kPoint ? "loot point" : "container: empty here");
        if (k == kPoint) g.destroyQ.push_back(MakeRef(obj));
        return true;
    }
    const int64_t now = SteadyMs();
    if (k == kItem) {                                // every weapon and armour piece in the game begins play here: nothing to match, nothing to read
        g.points.erase(std::remove_if(g.points.begin(), g.points.end(), [now](const Point& p) { return now - p.ms > kPointKeepMs; }), g.points.end());
        if (g.points.empty()) return false;
    }
    FVector at;
    if (!ReadAt(obj, at)) return false;
    if (k == kPoint) {
        g.points.push_back({at, now});
        if (g.points.size() > kMaxPoints) g.points.erase(g.points.begin());
        return false;
    }
    // A product: the loot point that just began play and stands next to it made it (or the container whose contents it is).
    int best = -1; double bestD = 1e18;
    for (size_t i = 0; i < g.points.size(); ++i) {
        if (k == kContainer && g.points[i].multi) continue;   // a container is made by a loot point, never by another container
        const double d = Dist(g.points[i].at, at);
        if (d <= g.points[i].reach && d <= bestD) { bestD = d; best = int(i); }
    }
    if (k == kContainer) {                           // its contents roll a frame after its BeginPlay (Delay(0)): they are its products too
        if (best >= 0 && !g.points[size_t(best)].multi) g.points.erase(g.points.begin() + best);
        g.points.push_back({at, now, kContainerMatchCm, true});
        if (g.points.size() > kMaxPoints) g.points.erase(g.points.begin());
        if (best < 0) return false;                  // a container placed in the level: both machines have it, only its contents are sent
    } else {
        if (best < 0) return false;                  // placed in the level, or spawned by something else: both machines have it
        if (!g.points[size_t(best)].multi) g.points.erase(g.points.begin() + best);
    }
    const std::string path = ClassPathOf(obj);
    uint8_t itemKind = crypt::kItemNone;
    if (!crypt::LootClassOk(path)) {
        if (crypt::ItemClassOk(path, crypt::kItemArmor)) itemKind = crypt::kItemArmor;
        else if (crypt::ItemClassOk(path, crypt::kItemWeapon)) itemKind = crypt::kItemWeapon;
        else {
            HSM_LOG("crypt coop: loot: %s begins play at a loot point but its class path is not one the wire carries (%s)", ObjectName(obj).c_str(), path.c_str());
            return false;
        }
    }
    g.candidates.push_back({MakeRef(obj), path, g.nowMs, itemKind, 0});
    return false;
}

// ---------------------------------------------------------------------------------------------------------------- loot

void SendLoot(const CryptLootMsg& m) { if (g.haveEnv && g.env.sendLoot) g.env.sendLoot(m); }
void SendDoor(const CryptDoorMsg& m) { if (g.haveEnv && g.env.sendDoor) g.env.sendDoor(m); }

// Host: gives each product a room and an index, and sends it.
void HostResolveCandidates(const Env& env) {
    int exports = 0;
    for (size_t i = 0; i < g.candidates.size();) {
        Candidate& c = g.candidates[i];
        if (!Alive(c.obj)) { g.candidates.erase(g.candidates.begin() + long(i)); continue; }
        FVector at;
        if (!ReadAt(c.obj.obj, at)) { g.candidates.erase(g.candidates.begin() + long(i)); continue; }
        const uint16_t room = env.roomAt ? env.roomAt(at.x, at.y, at.z) : 0;
        if (room == 0) {
            if (g.nowMs - c.sinceMs > kCandidateGiveUpMs) {
                HSM_LOG("crypt coop: loot: %s at %s is in no room the host sent after %u s; not sent", Leaf(c.cls).c_str(), V(at).c_str(), kCandidateGiveUpMs / 1000);
                g.candidates.erase(g.candidates.begin() + long(i));
            } else ++i;
            continue;
        }
        const FRotator r = ReadRot(c.obj.obj);
        // A weapon or armour piece goes with its passport struct as a save file (docs/PLAN_CRYPTS_COOP.md, PR D2); that is a save to disk, so a few a tick.
        std::vector<uint8_t> item;
        if (c.itemKind != crypt::kItemNone) {
            if (exports >= kExportsPerTick) { ++i; continue; }
            ++exports;
            if (!env.adapter || !env.adapter->ExportItemState(c.obj.obj, c.itemKind, item)) {
                if (++c.tries < kExportTries) { ++i; continue; }
                HSM_LOG("crypt coop: loot room #%u: %s at %s: its state could not be saved; not sent (a client will not have it)", unsigned(room), Leaf(c.cls).c_str(), V(at).c_str());
                g.candidates.erase(g.candidates.begin() + long(i));
                continue;
            }
        }
        CryptLootMsg m;
        if (g.loot.Add(room, c.cls, float(at.x), float(at.y), float(at.z), float(r.pitch), float(r.yaw), float(r.roll), m, c.itemKind, c.itemKind != crypt::kItemNone ? &item : nullptr)) {
            g.track[Key(m.room, m.index)] = {c.obj, at, false, c.itemKind != crypt::kItemNone ? crypt::kGearTakenCm : crypt::kLootTakenCm};
            if (c.itemKind != crypt::kItemNone)
                HSM_LOG("crypt coop: loot room #%u point %u: %s (%zu bytes of item state) at %s: sent", unsigned(m.room), unsigned(m.index), Leaf(m.cls).c_str(), m.item.size(), V(at).c_str());
            else
                HSM_LOG("crypt coop: loot room #%u point %u: %s at %s: sent", unsigned(m.room), unsigned(m.index), Leaf(m.cls).c_str(), V(at).c_str());
            SendLoot(m);
        }
        g.candidates.erase(g.candidates.begin() + long(i));
    }
}

// An item sitting where it was placed (not the one a player carries or kicked away).
bool StillHome(const Tracked& t) {
    if (!Alive(t.obj)) return false;
    FVector at;
    return ReadAt(t.obj.obj, at) && Dist(at, t.origin) <= t.homeCm;
}

// Both sides: an item of ours that is gone or moved away from its place was taken. The host sends it to everyone; a client to the host.
void WatchTaken(const char* who) {
    for (auto& [key, t] : g.track) {
        const uint16_t room = uint16_t(key >> 16), index = uint16_t(key & 0xFFFF);
        const CryptLootMsg* rec = g.loot.Find(room, index);
        if (!rec || rec->state == crypt::kLootTaken || !crypt::LootTakeable(rec->cls)) continue;
        if (StillHome(t)) continue;
        if (!g.loot.MarkTaken(room, index)) continue;
        CryptLootMsg m = *rec;
        HSM_LOG("crypt coop: loot room #%u point %u: %s taken %s; sent", unsigned(room), unsigned(index), Leaf(rec->cls).c_str(), who);
        SendLoot(m);
    }
}

// Client: spawns the host's things whose room is built.
void ClientSpawnLoot(const Env& env) {
    int spawned = 0;
    for (const CryptLootMsg& rec : g.loot.All()) {
        if (spawned >= kSpawnsPerTick) break;
        const uint32_t key = Key(rec.room, rec.index);
        if (rec.state == crypt::kLootTaken || g.track.count(key) || g.spawnFails[key] >= 3) continue;
        if (!env.roomBuilt || !env.roomBuilt(rec.room)) continue;
        ++spawned;
        const bool gear = rec.itemKind != crypt::kItemNone;
        UClass* cls = env.adapter->LoadClassBlocking(rec.cls);
        std::string why = cls ? "spawn failed" : "class not loaded";
        const FVector at{rec.x, rec.y, rec.z};
        const FRotator rot{rec.pitch, rec.yaw, rec.roll};
        UObject* o = !cls ? nullptr : gear ? env.adapter->SpawnItemFromState(rec.item, rec.itemKind, cls, at, rot, why) : env.adapter->SpawnActorAt(cls, at, rot);
        if (!o) {
            if (++g.spawnFails[key] == 3) HSM_LOG("crypt coop: loot room #%u point %u: %s NOT spawned (%s)", unsigned(rec.room), unsigned(rec.index), Leaf(rec.cls).c_str(), why.c_str());
            continue;
        }
        g.track[key] = {MakeRef(o), at, true, gear ? crypt::kGearTakenCm : crypt::kLootTakenCm};
        if (gear) HSM_LOG("crypt coop: loot room #%u point %u: %s (%zu bytes of item state) at (%.0f,%.0f,%.0f)", unsigned(rec.room), unsigned(rec.index), Leaf(rec.cls).c_str(), rec.item.size(), rec.x, rec.y, rec.z);
        else HSM_LOG("crypt coop: loot room #%u point %u: %s at (%.0f,%.0f,%.0f)", unsigned(rec.room), unsigned(rec.index), Leaf(rec.cls).c_str(), rec.x, rec.y, rec.z);
    }
}

// ---------------------------------------------------------------------------------------------------------------- doors

bool GateLocked(UObject* gate) { bool b = true; return GetBool(gate, "Locked", b) ? b : true; }

uint8_t StateOf(uint8_t kind, UObject* o) {
    if (kind == crypt::kDoorGate) return GateLocked(o) ? crypt::kDoorClosed : crypt::kDoorOpen;
    if (kind == crypt::kDoorTearDown) {
        const int32_t* broken = Field<int32_t>(o, "Number of planks broken");
        return broken && *broken >= kPlanksToBreak ? crypt::kDoorGone : crypt::kDoorClosed;
    }
    return crypt::kDoorClosed;
}

// Sets the door actor of `seq` to its book state, here.
void ApplyDoor(uint16_t seq) {
    const CryptDoorMsg* d = g.doors.Get(seq);
    auto it = g.doorActor.find(seq);
    if (!d || it == g.doorActor.end() || !Alive(it->second)) return;
    UObject* o = it->second.obj;
    if (d->state == crypt::kDoorGone) {
        Destroy(o);
    } else if (d->state == crypt::kDoorOpen && d->kind == crypt::kDoorGate && GateLocked(o)) {
        Call(o, "Event Unlock Door").invoke();
    }
}

// Both sides, every ~1.5 s: finds the door actors, numbers them (host), and sends a change of state (a client only an unlocked gate).
void ScanDoors(const Env& env) {
    for (uint8_t kind = 0; kind < 3; ++kind) {
        for (UObject* o : FindInstances(kDoorClass[kind])) {
            if (IsGarbage(o)) continue;
            FVector at;
            if (!ReadAt(o, at)) continue;
            uint16_t seq = g.doors.Find(kind, float(at.x), float(at.y), float(at.z));
            if (seq == 0 && env.host) {
                CryptDoorMsg m;
                if (g.doors.Add(kind, float(at.x), float(at.y), float(at.z), float(ReadRot(o).yaw), StateOf(kind, o), m)) {
                    seq = m.seq;
                    HSM_LOG("crypt coop: door #%u (%s) at %s: %s; sent", unsigned(m.seq), kDoorName[kind], V(at).c_str(), kDoorStateName[m.state]);
                    SendDoor(m);
                }
            }
            if (seq == 0) continue;
            g.doorActor[seq] = MakeRef(o);
            // The game arms a barricade when player 0 overlaps its box; a remote player's body is not player 0.
            if (env.host && kind == crypt::kDoorTearDown) {
                bool init = true;
                if (GetBool(o, "Initialized", init) && !init)
                    for (UObject* r : env.remotes) {
                        FVector p;
                        if (r && ReadAt(r, p) && Dist(p, at) <= kArmBarricadeCm) {
                            SetBool(o, "Initialized", true);
                            if (!g.armedBarricadeLogged) { g.armedBarricadeLogged = true; HSM_LOG("crypt coop: door #%u (tear-down barricade) armed for a remote player", unsigned(seq)); }
                            break;
                        }
                    }
            }
        }
    }
    // State changes.
    for (auto& [seq, ref] : g.doorActor) {
        const CryptDoorMsg* d = g.doors.Get(seq);
        if (!d) continue;
        const bool alive = Alive(ref);
        // A client's own loader clears the gates where two rooms join; that is not a door the host opened.
        if (!env.host && (d->kind != crypt::kDoorGate || !alive)) continue;
        const uint8_t st = alive ? StateOf(d->kind, ref.obj) : crypt::kDoorGone;
        if (!g.doors.Advance(seq, st)) continue;
        CryptDoorMsg m = *g.doors.Get(seq);
        HSM_LOG("crypt coop: door #%u (%s) at (%.0f,%.0f,%.0f): %s; sent", unsigned(seq), kDoorName[m.kind], m.x, m.y, m.z, kDoorStateName[m.state]);
        SendDoor(m);
    }
}

// Client: the barricades the host's rooms placed. Its own rooms never run their logic, so it spawns them, inert (no breaking of its own).
void ClientSpawnDoors(const Env& env) {
    if (!env.allBuilt || !env.allBuilt()) return;
    int spawned = 0;
    for (const CryptDoorMsg& d : g.doors.All()) {
        if (spawned >= kSpawnsPerTick) break;
        if (d.kind == crypt::kDoorGate || d.state == crypt::kDoorGone || g.doorFails[d.seq] >= 3) continue;
        auto it = g.doorActor.find(d.seq);
        if (it != g.doorActor.end() && Alive(it->second)) continue;
        ++spawned;
        UClass* cls = env.adapter->LoadClassBlocking(kDoorPath[d.kind]);
        UObject* o = cls ? env.adapter->SpawnActorAt(cls, FVector{d.x, d.y, d.z}, FRotator{0, d.yaw, 0}) : nullptr;
        if (!o) {
            if (++g.doorFails[d.seq] == 3) HSM_LOG("crypt coop: door #%u (%s) NOT spawned (%s)", unsigned(d.seq), kDoorName[d.kind], cls ? "spawn failed" : "class not loaded");
            continue;
        }
        if (d.kind == crypt::kDoorTearDown)
            SetBool(o, "Break Cooldown", true);   // its Tick returns while this is set: the host breaks it
        g.doorActor[d.seq] = MakeRef(o);
        HSM_LOG("crypt coop: door #%u (%s) spawned here at (%.0f,%.0f,%.0f)", unsigned(d.seq), kDoorName[d.kind], d.x, d.y, d.z);
    }
}

}  // namespace

void InstallRule() {
    HookFunctionsMatching([](const std::string& n) { return n == "ReceiveBeginPlay"; },
                          [](UObject* obj, UFunction*, void*) { return OnBeginPlay(obj); });
}

void Reset() {
    g.loot.Clear(); g.doors.Clear();
    g.points.clear(); g.candidates.clear(); g.track.clear(); g.spawnFails.clear(); g.destroyQ.clear();
    g.doorActor.clear(); g.doorFails.clear(); g.kindOf.clear();
    g.blocked[0] = g.blocked[1] = 0;
    g.armedBarricadeLogged = false;
    g.nextTaken = g.nextDoorScan = 0;
}

void Tick(uint32_t nowMs, const Env& env) {
    g.nowMs = nowMs;
    g.env = env; g.haveEnv = true;
    for (const Ref& r : g.destroyQ) if (Alive(r)) Destroy(r.obj);
    g.destroyQ.clear();
    if (env.host) HostResolveCandidates(env);
    else if (env.adapter) { ClientSpawnLoot(env); ClientSpawnDoors(env); }
    if (nowMs >= g.nextTaken) {
        g.nextTaken = nowMs + kTakenPollMs;
        WatchTaken(env.host ? "on the host" : "here");
    }
    if (nowMs >= g.nextDoorScan) {
        g.nextDoorScan = nowMs + kDoorScanMs;
        ScanDoors(env);
    }
}

void OnLoot(const CryptLootMsg& m, bool fromClient) {
    if (fromClient) {
        // Host: a client took an item. Only a take of a record the host sent, once; the host removes its copy and tells everyone.
        if (m.state != crypt::kLootTaken || !g.loot.Find(m.room, m.index)) return;
        if (g.loot.Find(m.room, m.index)->cls != m.cls || !g.loot.MarkTaken(m.room, m.index)) return;
        auto it = g.track.find(Key(m.room, m.index));
        const bool removed = it != g.track.end() && StillHome(it->second);
        if (removed) Destroy(it->second.obj.obj);
        HSM_LOG("crypt coop: loot room #%u point %u: %s taken by a client; %s", unsigned(m.room), unsigned(m.index), Leaf(m.cls).c_str(),
                removed ? "removed here, sent to everyone" : "sent to everyone");
        CryptLootMsg out = *g.loot.Find(m.room, m.index);
        SendLoot(out);
        return;
    }
    const auto got = g.loot.Receive(m);
    if (got == crypt::LootBook::Got::NowTaken) {
        auto it = g.track.find(Key(m.room, m.index));
        const bool removed = it != g.track.end() && StillHome(it->second);
        if (removed) Destroy(it->second.obj.obj);
        HSM_LOG("crypt coop: loot room #%u point %u: %s taken elsewhere; %s", unsigned(m.room), unsigned(m.index), Leaf(m.cls).c_str(),
                removed ? "removed here" : "not here (or the one carried here)");
    } else if (got == crypt::LootBook::Got::Bad) {
        HSM_LOG("crypt coop: loot room #%u point %u: %s refused (another thing already at that address)", unsigned(m.room), unsigned(m.index), Leaf(m.cls).c_str());
    }
}

void OnDoor(const CryptDoorMsg& m, bool fromClient) {
    if (fromClient) {
        // Host: a client unlocked a gate (a key it carries). Only an opening of a known gate; the host opens its own and tells everyone.
        const CryptDoorMsg* d = g.doors.Get(m.seq);
        if (!d || d->kind != m.kind || m.kind != crypt::kDoorGate || m.state != crypt::kDoorOpen || d->state != crypt::kDoorClosed) return;
        // The host checks the claim: some remote player's body must be at the gate (the key's collider is within a few metres of it).
        bool near = false;
        for (UObject* r : g.env.remotes) {
            FVector p;
            if (r && !IsGarbage(r) && ReadAt(r, p) && Dist(p, FVector{d->x, d->y, d->z}) <= kClaimNearCm) { near = true; break; }
        }
        if (!near) {
            HSM_LOG("crypt coop: door #%u (gate): a client says it is open but no remote player is within %.0f cm of it; refused", unsigned(m.seq), kClaimNearCm);
            return;
        }
        g.doors.Advance(m.seq, m.state);
        ApplyDoor(m.seq);
        HSM_LOG("crypt coop: door #%u (%s): %s by a client; applied here, sent to everyone", unsigned(m.seq), kDoorName[m.kind], kDoorStateName[m.state]);
        SendDoor(*g.doors.Get(m.seq));
        return;
    }
    const auto got = g.doors.Receive(m);
    if (got == crypt::DoorBook::Got::Advanced) {
        ApplyDoor(m.seq);
        HSM_LOG("crypt coop: door #%u (%s): %s elsewhere; applied here", unsigned(m.seq), kDoorName[m.kind], kDoorStateName[m.state]);
    } else if (got == crypt::DoorBook::Got::New) {
        HSM_LOG("crypt coop: door #%u (%s) at (%.0f,%.0f,%.0f): %s", unsigned(m.seq), kDoorName[m.kind], m.x, m.y, m.z, kDoorStateName[m.state]);
    } else if (got == crypt::DoorBook::Got::Bad) {
        HSM_LOG("crypt coop: door #%u refused (out of order or another kind)", unsigned(m.seq));
    }
}

void ForEachRecord(const std::function<void(const std::vector<uint8_t>&)>& send) {
    for (const CryptLootMsg& m : g.loot.All()) send(Encode(m));
    for (const CryptDoorMsg& m : g.doors.All()) send(Encode(m));
}

}  // namespace hsm::game::cryptsloot
