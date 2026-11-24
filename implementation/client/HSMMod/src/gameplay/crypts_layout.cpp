#include "gameplay/crypts_layout.h"
#include "gameplay/adapter.h"
#include "log.h"
#include "ue/hook.h"
#include <cmath>
#include <cstdio>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

using namespace hsm::ue;

namespace hsm::game::cryptslayout {
namespace {

constexpr const char* kLevelScriptClass = "Map_Procedural_Block_Test_Main_C";
constexpr const char* kBlockClass = "BP_Procedural_Block_Master_C";
constexpr const char* kDirectorClass = "BP_Procedural_Dungeon_Director_C";
constexpr const char* kSpawnerClass = "BP_SpawnerPoint_Willies_C";
constexpr const char* kGateClass = "BP_Door_Gate_A_001_C";
constexpr const char* kStreamingClass = "LevelStreamingDynamic";
constexpr const char* kBarricadePath = "/Game/Blueprints/Procedural/Structures/BP_Door_Barricade_Unbreakable.BP_Door_Barricade_Unbreakable_C";
constexpr const char* kCoverMesh = "StaticMeshActor_1";   // the level script destroys it 10 s after BeginPlay
constexpr const char* kCryptsWorld = "Map_Procedural_Block_Test_Main";
constexpr uint8_t kCryptsGameMode = 6;                      // the level script's own Set Game Mode

constexpr uint32_t kScanGapMs = 200;
constexpr uint32_t kCoverAfterMs = 10000;
constexpr uint32_t kTripwireAfterMs = 30000;
constexpr uint32_t kTimeoutMs = 45000;   // session/hsm_level_gate.h kLayoutMaxWaitMs
constexpr uint32_t kProgressGapMs = 5000;
constexpr int kProgressLogs = 12;        // 60 s of progress lines at most
constexpr int kBlockLogsPerKind = 3;
constexpr int kNameMismatchLogs = 3;
constexpr int kCoopStartsPerTick = 6;    // rooms whose levels one tick asks for (3 levels each)

// The instance name's form (SetNameForm). Auto settles on the first level of each build.
enum NameForm : uint8_t { kAuto, kShort, kLong, kEngine, kForms };
const char* const kFormName[kForms] = {"auto", "short", "long", "engine"};
NameForm g_form = kAuto;

enum Kind : uint8_t { kOther = 0, kScript = 1, kRoom = 2, kDirector = 3, kKinds = 4 };
const char* const kKindName[kKinds] = {"other", "level script", "room", "director"};

struct Stream {
    std::string name, path;   // instance name, package path
    int row = -1;             // -1: the Ambient level; in a co-op build the room's seq
    crypts::Part part = crypts::Part::Module;
    UObject* stream = nullptr;
    int32_t index = -1;
    UObject* level = nullptr;   // the shown ULevel
    bool failed = false;
};

// One room of a co-op build (the host's CryptRoom, its three levels in `streams` under row == seq).
struct CoopRoom {
    crypt::CryptRoomMsg msg;
    bool jointDone = false, built = false;
};

struct State {
    // the rule's (kept from Arm to the next Arm)
    const crypts::Layout* armed = nullptr;
    bool coop = false;        // a client in the host's shared Crypts: same rule, the rooms come from the host
    std::unordered_map<UClass*, uint8_t> kind;
    int blocked[kKinds] = {};
    // this level's build
    const crypts::Layout* layout = nullptr;
    UObject* context = nullptr;
    uint32_t startMs = 0, nextScan = 0, nextProgress = 0;
    NameForm names = kAuto;   // the form this build loads under
    int nameMismatches = 0, progressLogs = 0;
    std::vector<crypts::Pose> origins;
    std::vector<crypts::Join> joins;
    std::vector<bool> joinDone;
    std::vector<crypts::OpenEntrance> open;
    std::vector<bool> rowBarred;
    std::vector<Stream> streams;
    std::unordered_set<const UObject*> shown;
    UClass* barricadeCls = nullptr;
    bool barricadeTried = false, built = false, coverDone = false, tripwireDone = false, timeoutLogged = false;
    int barricades = 0, gatesCleared = 0;
    // a co-op build
    bool coopLive = false, coopEntrance = false, coopTimeoutLogged = false;
    std::vector<CoopRoom> coopRooms;   // index seq - 1
};
State g;

std::string V(FVector v) { char b[96]; std::snprintf(b, sizeof b, "(%.0f,%.0f,%.0f)", v.x, v.y, v.z); return b; }
FVector At(const crypts::Pose& p) { return {p.x, p.y, p.z}; }
double Dist(FVector a, FVector b) { return std::sqrt((a.x - b.x) * (a.x - b.x) + (a.y - b.y) * (a.y - b.y) + (a.z - b.z) * (a.z - b.z)); }
bool Alive(UObject* o, int32_t index) { return o && index >= 0 && ObjectAt(index) == o && !IsGarbage(o); }
UObject* GameInstance() { auto v = FindInstances("GI_Settings_C"); return v.empty() ? nullptr : v[0]; }
const char* PartName(crypts::Part p) { return p == crypts::Part::Module ? "module" : p == crypts::Part::Lighting ? "lighting" : "logic"; }

uint8_t KindOf(UClass* c) {
    if (!c) return kOther;
    auto it = g.kind.find(c);
    if (it != g.kind.end()) return it->second;
    uint8_t k = kOther;
    for (UClass* s = c; s && k == kOther; s = SuperOf(s)) {
        const std::string n = ObjectName(reinterpret_cast<UObject*>(s));
        if (n == kLevelScriptClass) k = kScript;
        else if (n == kBlockClass) k = kRoom;
        else if (n == kDirectorClass) k = kDirector;
    }
    g.kind.emplace(c, k);
    return k;
}

void NoteBlock(uint8_t k, UObject* obj, UFunction* fn) {
    if (g.blocked[k]++ < kBlockLogsPerKind)
        HSM_LOG("crypts layout: blocked %s.%s (%s; dungeon off)", ObjectName(reinterpret_cast<UObject*>(ClassOf(obj))).c_str(),
                ObjectName(reinterpret_cast<UObject*>(fn)).c_str(), kKindName[k]);
}

// A streaming level that is loaded and shown: its ULevel, else nullptr (crypts_probe.cpp's).
UObject* ShownLevel(UObject* stream) {
    if (!stream || IsGarbage(stream)) return nullptr;
    Call v(stream, "IsLevelVisible");
    if (!v.invoke() || !v.get<bool>("ReturnValue")) return nullptr;
    Call l(stream, "GetLoadedLevel");
    return l.invoke() ? l.get<UObject*>("ReturnValue") : nullptr;
}

// LevelStreamingDynamic.LoadLevelInstance under `instance` (empty: the engine's own name). nullptr when the engine refused it.
UObject* LoadInstance(UObject* streamingCdo, const std::string& path, const crypts::Pose& at, const std::string& instance) {
    FStringArgHolder level(path), name(instance);
    Call c(streamingCdo, "LoadLevelInstance");
    c.set<UObject*>("WorldContextObject", g.context).set("LevelName", level.arg).set("Location", At(at))
        .set("Rotation", FRotator{0, at.yaw, 0}).set<bool>("bLoadAsTempPackage", false);
    if (!instance.empty()) c.set("OptionalLevelNameOverride", name.arg);
    if (!c.invoke()) return nullptr;
    return c.get<bool>("bOutSuccess") ? c.get<UObject*>("ReturnValue") : nullptr;
}

std::string LongInstanceName(const std::string& path, const std::string& instance) {
    const size_t slash = path.rfind('/');
    return (slash == std::string::npos ? std::string() : path.substr(0, slash + 1)) + instance;
}

// The package name a streaming level loads its level as ("?": unread).
std::string PackageOf(UObject* stream) {
    Call c(stream, "GetWorldAssetPackageFName");
    return c.invoke() ? NameToString(c.get<FName>("ReturnValue")) : std::string("?");
}

bool Ask(UObject* stream, const char* fn) {
    Call c(stream, fn);
    return c.invoke() && c.get<bool>("ReturnValue");
}

// A streaming level LoadLevelInstance added in this tick: the world's next streaming update drops it without loading it.
void Discard(UObject* stream) {
    Call(stream, "SetShouldBeVisible").set<bool>("bInShouldBeVisible", false).invoke();
    Call(stream, "SetShouldBeLoaded").set<bool>("bInShouldBeLoaded", false).invoke();
    Call(stream, "SetIsRequestingUnloadAndRemoval").set<bool>("bInIsRequestingUnloadAndRemoval", true).invoke();
}

std::string OverrideFor(NameForm form, const Stream& s) {
    return form == kShort ? s.name : form == kLong ? LongInstanceName(s.path, s.name) : std::string();
}

// Loads `s` under its instance name. Every machine must give a room's level the same package name, the source level's
// folder plus the instance name (match/hsm_crypts_layout.h; its shared dice key on it). Whether LoadLevelInstance wants the
// override short (the engine puts it in the source's folder) or long (taken whole) is not known for this build: an
// owner's Descent (2026-11-15) loaded none of its 34 levels in 13 s under long names, where a probe's level with no
// override showed in 420 ms. So the first level of a build (auto) tries short, then long, and keeps the form whose read-back
// package name is the one wanted; a level loaded under a wrong name is dropped before the engine asks for it. Neither:
// the engine's own name for the build.
void Load(UObject* streamingCdo, Stream& s, const crypts::Pose& at) {
    const std::string want = LongInstanceName(s.path, s.name);
    std::string package;
    if (g.names == kAuto) {
        std::string tried;
        bool loaded = false;   // both refused (a path the engine cannot find, not a name): the next level tries again
        for (NameForm form : {kShort, kLong}) {
            UObject* st = LoadInstance(streamingCdo, s.path, at, OverrideFor(form, s));
            package = st ? PackageOf(st) : std::string();
            tried += std::string(tried.empty() ? "" : "; ") + kFormName[form] + " " + (st ? "as " + package : std::string("refused"));
            if (!st) continue;
            loaded = true;
            if (package == want) { s.stream = st; g.names = form; break; }
            Discard(st);
        }
        if (g.names == kAuto && loaded) g.names = kEngine;
        HSM_LOG("crypts layout: instance names %s (%s; wanted %s)%s", g.names == kAuto ? "not settled" : kFormName[g.names], tried.c_str(),
                want.c_str(), g.names == kEngine ? ": NEITHER FORM GAVE THE NAME WANTED; the engine's own names, the rooms' shared dice "
                "fall back to the world's name (loot may differ between machines)" : g.names == kAuto ? ": both refused; the next level tries again" : "");
    }
    if (!s.stream && g.names != kAuto) {
        s.stream = LoadInstance(streamingCdo, s.path, at, OverrideFor(g.names, s));
        if (s.stream && g.names != kEngine && (package = PackageOf(s.stream)) != want && g.nameMismatches++ < kNameMismatchLogs)
            HSM_LOG("crypts layout: %s: loading as %s, not %s", s.name.c_str(), package.c_str(), want.c_str());
    }
    if (!s.stream && g.names != kEngine) {   // the name refused (auto: both forms)
        s.stream = LoadInstance(streamingCdo, s.path, at, "");
        HSM_LOG("crypts layout: %s: the instance name was refused; %s", s.name.c_str(),
                s.stream ? "loaded under the engine's own name (its shared dice fall back to the world's name)" : "the level did not load");
    }
    s.index = s.stream ? ObjectIndex(s.stream) : -1;
    s.failed = !s.stream;
}

const char* BuildName() { return g.layout ? g.layout->name : "coop"; }

// Every 5 s of a build: how far its levels got. Loaded but not shown: the engine is making them visible (or cannot); not
// loaded and not pending: nothing asked for them.
void LogProgress(uint32_t since) {
    int shown = 0, loaded = 0, pending = 0, failed = 0, idle = 0;
    const Stream* first = nullptr;
    for (const Stream& s : g.streams) {
        if (s.level) { ++shown; continue; }
        if (s.failed || !Alive(s.stream, s.index)) { ++failed; continue; }
        const bool isLoaded = Ask(s.stream, "IsLevelLoaded"), isPending = Ask(s.stream, "IsStreamingStatePending");
        loaded += isLoaded;
        pending += isPending;
        idle += !isLoaded && !isPending;
        if (!first) first = &s;
    }
    HSM_LOG("crypts layout: %s %u s: %d of %zu levels shown, %d more loaded, %d streaming, %d idle, %d failed%s%s%s%s", BuildName(),
            since / 1000, shown, g.streams.size(), loaded, pending, idle, failed, first ? "; first waiting " : "",
            first ? first->name.c_str() : "", first ? " as " : "", first ? PackageOf(first->stream).c_str() : "");
}

UObject* ModuleOf(int row) {
    for (const Stream& s : g.streams) if (s.row == row && s.part == crypts::Part::Module) return s.level;
    return nullptr;
}

int CountInstances(const char* className, bool inLayout) {
    UClass* c = FindClass(className);
    if (!c) return 0;
    int n = 0;
    for (int32_t i = 0, count = ObjectCount(); i < count; ++i) {
        UObject* o = ObjectAt(i);
        if (!o || IsDefaultObject(o) || IsGarbage(o) || !IsA(o, c)) continue;
        if (inLayout && !g.shown.count(OuterOf(o))) continue;
        ++n;
    }
    return n;
}

// Where two rooms meet, each room's level has its own gate ~10 uu from the joint (the far-loaded Chamber_1 showed one at
// every entrance). A gate's BeginPlay destroys itself when another stands within 25 uu, and its Event Clear Door destroys
// every other gate within 50 uu (BP_Door_Gate_A_001.cpp:122, 170), so one is left; the game then swings its door open
// with physics. Here the gate left is taken out of play instead: no physics, no collision, hidden. An open archway, with
// nothing to sync.
// Returns the gates found near the joint and how many were left standing (taken out of play).
std::pair<size_t, int> ClearGatesAt(const FVector at) {
    std::vector<UObject*> near;
    for (UObject* gate : FindInstances(kGateClass)) {
        if (IsGarbage(gate)) continue;
        Call l(gate, "K2_GetActorLocation");
        if (l.invoke() && Dist(l.get<FVector>("ReturnValue"), at) <= crypts::kJoinRadius) near.push_back(gate);
    }
    for (UObject* gate : near) if (!IsGarbage(gate)) Call(gate, "Event Clear Door").invoke();
    int standing = 0;
    for (UObject* gate : near) {
        if (IsGarbage(gate)) continue;
        ++standing;
        if (UObject** door = Field<UObject*>(gate, "Door"); door && *door && !IsGarbage(*door))
            Call(*door, "SetSimulatePhysics").set<bool>("bSimulate", false).invoke();
        Call(gate, "SetActorEnableCollision").set<bool>("bNewActorEnableCollision", false).invoke();
        Call(gate, "SetActorHiddenInGame").set<bool>("bNewHidden", true).invoke();
    }
    g.gatesCleared += standing;
    return {near.size(), standing};
}

void ClearJoint(size_t j) {
    const crypts::Join& jn = g.joins[j];
    const FVector at = At(jn.at);
    const auto [found, standing] = ClearGatesAt(at);
    HSM_LOG("crypts layout: joint row %u entrance %u - row %u entrance %u at %s: %zu gates, %d left by Event Clear Door, taken out of play",
            jn.parent, jn.parentEntrance, jn.child, jn.childEntrance, V(at).c_str(), found, standing);
}

void BarRow(int row, Adapter& adapter) {
    if (!g.barricadeTried) {
        g.barricadeTried = true;
        g.barricadeCls = adapter.LoadClassBlocking(kBarricadePath);
        if (!g.barricadeCls) HSM_LOG("crypts layout: the barricade class did not load; open entrances stay open");
    }
    if (!g.barricadeCls) return;
    for (const crypts::OpenEntrance& e : g.open) {
        if (e.row != row) continue;
        UObject* b = adapter.SpawnActorAt(g.barricadeCls, At(e.at), FRotator{0, e.at.yaw, 0});
        if (b) ++g.barricades;
        HSM_LOG("crypts layout: row %d entrance %u barred at %s yaw %.0f%s", row, e.entrance, V(At(e.at)).c_str(), e.at.yaw, b ? "" : " - FAILED");
    }
}

void RemoveCover() {
    g.coverDone = true;
    UObject* persistent = OuterOf(g.context);
    for (int32_t i = 0, n = ObjectCount(); i < n; ++i) {
        UObject* o = ObjectAt(i);
        if (!o || IsDefaultObject(o) || IsGarbage(o) || OuterOf(o) != persistent || ObjectName(o) != kCoverMesh) continue;
        Call l(o, "K2_GetActorLocation");
        Call b(o, "GetActorBounds");
        b.set<bool>("bOnlyCollidingComponents", false).set<bool>("bIncludeFromChildActors", false);
        const FVector at = l.invoke() ? l.get<FVector>("ReturnValue") : FVector{};
        const bool bounded = b.invoke();
        HSM_LOG("crypts layout: %s at %s, extent %s: removed (the level script removes it after 10 s)", kCoverMesh, V(at).c_str(),
                bounded ? V(b.get<FVector>("BoxExtent")).c_str() : "?");
        Call(o, "K2_DestroyActor").invoke();
        return;
    }
    HSM_LOG("crypts layout: no %s in the level", kCoverMesh);
}

void LogTripwire(const char* when) {
    HSM_LOG("crypts layout: %s: blocked level script %d, room %d, director %d; rooms %d, directors %d in the world", when,
            g.blocked[kScript], g.blocked[kRoom], g.blocked[kDirector], CountInstances(kBlockClass, false), CountInstances(kDirectorClass, false));
}

int Shown() { int n = 0; for (const Stream& s : g.streams) n += s.level != nullptr; return n; }
int Failed() { int n = 0; for (const Stream& s : g.streams) n += s.failed; return n; }

// The streams that have become visible since the last scan are marked shown; one whose streaming level is gone has failed.
void ScanStreams(uint32_t since) {
    for (Stream& s : g.streams) {
        if (s.level || s.failed) continue;
        if (!Alive(s.stream, s.index)) { s.failed = true; HSM_LOG("crypts layout: %s: its streaming level is gone", s.name.c_str()); continue; }
        UObject* level = ShownLevel(s.stream);
        if (!level) continue;
        s.level = level;
        g.shown.insert(level);
        const UObject* worldObj = OuterOf(level);
        HSM_LOG("crypts layout: %s shown after %u ms (world %s, package %s)", s.name.c_str(), since, ObjectName(worldObj).c_str(),
                ObjectName(OuterOf(worldObj)).c_str());
    }
}

// A room's streams, for the co-op build: how many levels it has and how many are settled (shown or failed).
void CoopStreamsOf(uint16_t seq, int& total, int& settled, int& failed) {
    total = settled = failed = 0;
    for (const Stream& s : g.streams) {
        if (s.row != int(seq)) continue;
        ++total;
        settled += s.level != nullptr || s.failed;
        failed += s.failed;
    }
}
UObject* StreamLevel(int row, crypts::Part part) {
    for (const Stream& s : g.streams) if (s.row == row && s.part == part) return s.level;
    return nullptr;
}
bool StreamFailed(int row, crypts::Part part) {
    for (const Stream& s : g.streams) if (s.row == row && s.part == part) return s.failed;
    return false;
}

// Asks for the three levels of a room the host placed, at its Origin Point.
void StartCoopRoom(UObject* streamingCdo, const crypt::CryptRoomMsg& m, uint32_t since) {
    const crypts::RoomClass& rc = crypts::kRooms[m.catalog];
    const crypts::Pose at{m.x, m.y, m.z, m.yaw};
    CoopRoom room;
    room.msg = m;
    room.jointDone = m.parentSeq == 0;
    g.coopRooms.push_back(room);
    for (crypts::Part p : {crypts::Part::Module, crypts::Part::Lighting, crypts::Part::Logic}) {
        const char* path = crypts::PartPath(rc, p);
        if (!path || !*path) continue;
        Stream st;
        st.name = crypts::CoopInstanceName(m.seq, p);
        st.path = path;
        st.row = int(m.seq);
        st.part = p;
        g.streams.push_back(st);
        Load(streamingCdo, g.streams.back(), at);
        HSM_LOG("crypts layout: room #%u %s %s at %s yaw %.0f: %s (%u ms)", unsigned(m.seq), rc.name, PartName(p), V(At(at)).c_str(), at.yaw,
                g.streams.back().failed ? "NOT LOADED" : "loading", since);
    }
}

}  // namespace

void InstallRule() {
    HookFunctionsMatching([](const std::string& n) { return n == "ReceiveBeginPlay" || n == "Reinforcements_Timer__FinishedFunc"; },
                          [](UObject* obj, UFunction* fn, void*) {
        if (!g.armed && !g.coop) return false;
        const uint8_t k = KindOf(ClassOf(obj));
        if (k == kOther) return false;
        if (k == kScript)
            if (UObject* gi = GameInstance())
                if (uint8_t* m = Field<uint8_t>(gi, "Current Game Mode enum")) *m = kCryptsGameMode;
        NoteBlock(k, obj, fn);
        return true;
    });
    HookFunctionsMatching([](const std::string& n) { return n.rfind("BndEvt__BP_Procedural_Block_", 0) == 0; },
                          [](UObject* obj, UFunction* fn, void*) {
        if ((!g.armed && !g.coop) || KindOf(ClassOf(obj)) != kRoom) return false;
        NoteBlock(kRoom, obj, fn);
        return true;
    });
}

void Arm(const crypts::Layout* layout) {
    if (layout != g.armed) HSM_LOG("crypts layout: %s", layout ? ("armed: " + std::string(layout->name)).c_str() : "disarmed");
    if (!layout && g.coop) HSM_LOG("crypts layout: co-op disarmed");
    g.armed = layout;
    g.coop = false;
    g.kind.clear();   // the next level's classes
    for (int& b : g.blocked) b = 0;
}

const crypts::Layout* Armed() { return g.armed; }

void ArmCoop(bool on) {
    if (on != g.coop) HSM_LOG("crypts layout: co-op %s", on ? "armed: the next Crypts level builds the host's rooms" : "disarmed");
    g.coop = on;
    if (on) { g.armed = nullptr; g.kind.clear(); for (int& b : g.blocked) b = 0; }
}
bool CoopArmed() { return g.coop; }

void SetNameForm(const std::string& form) {
    NameForm f = kForms;
    for (int i = 0; i < kForms; ++i) if (form == kFormName[i]) f = NameForm(i);
    if (f == kForms) {
        HSM_LOG("config: crypts_layout_names=%s is not auto, short, long or engine; auto", form.c_str());
        f = kAuto;
    } else if (f != kAuto) {
        HSM_LOG("config: crypts_layout_names=%s: the shared Crypts maps' levels load under %s instance names (dev)", form.c_str(), form.c_str());
    }
    g_form = f;
}

// The level's start-up shared by a table and a co-op build: the dungeon off, the player's input back, and a log of what is there.
static void Prepare(UObject* context, Adapter& adapter, uint32_t nowMs, const char* name, unsigned rooms) {
    g.context = context;
    g.startMs = g.nextScan = nowMs;
    g.nextProgress = nowMs + kProgressGapMs;
    g.names = g_form;
    // Before any barricade: its BeginPlay calls the GI's dungeon director's Destroy Actors in Area 200 m ahead of it, which
    // destroys every fighter but local players 0 and 1. With no director (its BeginPlay is blocked) the call does nothing.
    UObject* gi = adapter.GameInstance();
    if (UObject** dir = gi ? Field<UObject*>(gi, "Current Procedural Dungeon Director") : nullptr; dir && *dir) {
        HSM_LOG("crypts layout: the game instance still named a dungeon director (%s); cleared", ObjectName(*dir).c_str());
        *dir = nullptr;
    }
    adapter.RestoreGameInput();   // the blocked level script would have set game-only input after 3 s
    const uint8_t* gameMode = gi ? Field<uint8_t>(gi, "Current Game Mode enum") : nullptr;
    const uint8_t* playMode = gi ? Field<uint8_t>(gi, "Current Play Mode") : nullptr;
    HSM_LOG("crypts layout: building %s (%u rooms): Game Mode %d, Play Mode %d, level manager %s, player pawn %s", name, rooms,
            gameMode ? int(*gameMode) : -1, playMode ? int(*playMode) : -1,
            FindInstances("BP_LevelManager_C").empty() ? "none" : "present", adapter.PlayerWillie(0) ? "present" : "none");
    LogTripwire("at the build");
}

// The host's shared Crypts, on a client: the Ambient level now; each room as the host's CryptRoom arrives (CoopTick).
static void StartCoop(UObject* context, Adapter& adapter, uint32_t nowMs) {
    UObject* streamingCdo = DefaultObject(FindClass(kStreamingClass));
    if (!streamingCdo) { HSM_LOG("crypts layout: co-op not built: no LevelStreamingDynamic class"); return; }
    g.coopLive = true;
    Prepare(context, adapter, nowMs, "the host's rooms", 0);
    Stream ambient;
    ambient.name = crypts::CoopAmbientInstanceName();
    ambient.path = crypts::kAmbientLevel;
    g.streams.push_back(ambient);
    Load(streamingCdo, g.streams.back(), crypts::Pose{});
}

const crypts::Layout* OnLevel(UObject* context, Adapter& adapter, uint32_t nowMs) {
    const crypts::Layout* armed = g.armed;
    const bool coop = g.coop;
    std::unordered_map<UClass*, uint8_t> kind = std::move(g.kind);
    int blocked[kKinds];
    for (int k = 0; k < kKinds; ++k) blocked[k] = g.blocked[k];
    g = State{};
    g.armed = armed; g.coop = coop; g.kind = std::move(kind);
    for (int k = 0; k < kKinds; ++k) g.blocked[k] = blocked[k];
    const std::string world = context ? ObjectName(OuterOf(OuterOf(context))) : std::string();
    if (!armed && !coop) return nullptr;
    if (world != kCryptsWorld) {
        HSM_LOG("crypts layout: %s is armed but %s loaded; nothing built, disarmed", armed ? armed->name : "co-op", world.c_str());
        g.armed = nullptr;   // a stale arm: never carried into a later Crypts run of the game's own
        g.coop = false;
        return nullptr;
    }
    if (!armed) { StartCoop(context, adapter, nowMs); return nullptr; }
    const crypts::LayoutCheck check = crypts::Check(*armed);
    UObject* streamingCdo = DefaultObject(FindClass(kStreamingClass));
    if (!check.ok() || !streamingCdo) {
        HSM_LOG("crypts layout: %s not built: %s", armed->name, !check.ok() ? "its table does not check out" : "no LevelStreamingDynamic class");
        return nullptr;
    }
    g.layout = armed;
    Prepare(context, adapter, nowMs, armed->name, unsigned(armed->count));
    g.origins = crypts::Place(*armed);
    g.joins = crypts::Joins(*armed, g.origins);
    g.joinDone.assign(g.joins.size(), false);
    g.open = crypts::OpenEntrances(*armed, g.origins);
    g.rowBarred.assign(armed->count, false);
    Stream ambient;
    ambient.name = crypts::AmbientInstanceName(*armed);
    ambient.path = crypts::kAmbientLevel;
    g.streams.push_back(ambient);
    Load(streamingCdo, g.streams.back(), crypts::Pose{});
    for (size_t r = 0; r < armed->count; ++r) {
        const crypts::RoomClass* room = crypts::FindRoom(armed->rows[r].room);
        for (crypts::Part p : {crypts::Part::Module, crypts::Part::Lighting, crypts::Part::Logic}) {
            const char* path = crypts::PartPath(*room, p);
            if (!path || !*path) continue;
            Stream s;
            s.name = crypts::InstanceName(*armed, r, p);
            s.path = path;
            s.row = int(r);
            s.part = p;
            g.streams.push_back(s);
            Load(streamingCdo, g.streams.back(), g.origins[r]);
            HSM_LOG("crypts layout: row %zu %s %s at %s yaw %.0f: %s", r, room->name, PartName(p), V(At(g.origins[r])).c_str(),
                    g.origins[r].yaw, g.streams.back().failed ? "NOT LOADED" : "loading");
        }
    }
    return armed;
}

void Tick(Adapter& adapter, uint32_t now) {
    if (!g.layout) return;
    const uint32_t since = now - g.startMs;
    if (!g.built && int32_t(now - g.nextScan) >= 0) {
        g.nextScan = now + kScanGapMs;
        ScanStreams(since);
        for (size_t r = 0; r < g.rowBarred.size(); ++r)
            if (!g.rowBarred[r] && ModuleOf(int(r))) { g.rowBarred[r] = true; BarRow(int(r), adapter); }
        for (size_t j = 0; j < g.joins.size(); ++j)
            if (!g.joinDone[j] && ModuleOf(g.joins[j].parent) && ModuleOf(g.joins[j].child)) { g.joinDone[j] = true; ClearJoint(j); }
        if (Pending() == 0) {
            g.built = true;
            HSM_LOG("crypts layout: %s built in %u ms: %d of %zu levels shown (%d failed), spawn points %d (the table says %d), %d barricades, %d joint gates",
                    g.layout->name, since, Shown(), g.streams.size(), Failed(), CountInstances(kSpawnerClass, true), crypts::SpawnCount(*g.layout),
                    g.barricades, g.gatesCleared);
        }
    }
    if (!g.built && g.progressLogs < kProgressLogs && int32_t(now - g.nextProgress) >= 0) {
        g.nextProgress = now + kProgressGapMs;
        ++g.progressLogs;
        LogProgress(since);
    }
    if (!g.coverDone && (g.built || since >= kCoverAfterMs)) RemoveCover();
    if (!g.tripwireDone && since >= kTripwireAfterMs) { g.tripwireDone = true; LogTripwire("30 s in"); }
    if (!g.built && !g.timeoutLogged && since >= kTimeoutMs) {
        g.timeoutLogged = true;
        std::string pending;
        for (const Stream& s : g.streams) if (!s.level && !s.failed) pending += (pending.empty() ? "" : ", ") + s.name;
        HSM_LOG("crypts layout: %s: %u s and still loading %s; the fighters go in without them", g.layout->name, kTimeoutMs / 1000, pending.c_str());
    }
}

void CoopTick(Adapter& adapter, uint32_t now, const crypt::RoomList& rooms, const std::function<void(uint16_t)>& onBuilt) {
    (void)adapter;
    if (!g.coopLive) return;
    const uint32_t since = now - g.startMs;
    if (UObject* cdo = DefaultObject(FindClass(kStreamingClass))) {
        const uint16_t known = rooms.Known();
        for (int started = 0; g.coopRooms.size() < known && started < kCoopStartsPerTick; ++started)
            if (const crypt::CryptRoomMsg* m = rooms.Find(uint16_t(g.coopRooms.size() + 1))) StartCoopRoom(cdo, *m, since);
            else break;
    }
    if (int32_t(now - g.nextScan) >= 0) {
        g.nextScan = now + kScanGapMs;
        ScanStreams(since);
        for (CoopRoom& r : g.coopRooms) {
            const uint16_t seq = r.msg.seq;
            if (!r.jointDone && StreamLevel(int(seq), crypts::Part::Module) && r.msg.parentSeq && r.msg.parentSeq <= g.coopRooms.size()
                && StreamLevel(int(r.msg.parentSeq), crypts::Part::Module)) {
                r.jointDone = true;
                const crypts::Pose at = crypt::JointOf(r.msg);
                const auto [found, standing] = ClearGatesAt(At(at));
                HSM_LOG("crypts layout: room #%u entrance %u joins room #%u at %s: %zu gates, %d left by Event Clear Door, taken out of play",
                        unsigned(seq), unsigned(r.msg.startEntrance), unsigned(r.msg.parentSeq), V(At(at)).c_str(), found, standing);
            } else if (!r.jointDone && (StreamFailed(int(seq), crypts::Part::Module) ||
                                        (r.msg.parentSeq && r.msg.parentSeq <= g.coopRooms.size() && StreamFailed(int(r.msg.parentSeq), crypts::Part::Module)))) {
                r.jointDone = true;   // a module that did not load has no gate to clear
                HSM_LOG("crypts layout: room #%u: a module did not load; its joint is not cleared", unsigned(seq));
            }
            if (r.built || !r.jointDone) continue;
            int total = 0, settled = 0, failed = 0;
            CoopStreamsOf(seq, total, settled, failed);
            if (settled < total) continue;
            r.built = true;
            HSM_LOG("crypts layout: room #%u %s built after %u ms: %d of %d levels shown (%d failed)", unsigned(seq), crypts::kRooms[r.msg.catalog].name,
                    since, total - failed, total, failed);
            if (onBuilt) onBuilt(seq);
        }
        if (!g.coopEntrance) {
            const uint16_t cover = rooms.EntranceCover();
            bool all = cover != 0 && cover <= g.coopRooms.size();
            for (uint16_t q = 1; all && q <= cover; ++q) all = g.coopRooms[q - 1].built;
            if (all) {
                g.coopEntrance = true;
                HSM_LOG("crypts layout: the entrance rooms (1..%u) are built after %u ms", unsigned(cover), since);
            }
        }
    }
    if (!g.coopEntrance && g.progressLogs < kProgressLogs && int32_t(now - g.nextProgress) >= 0) {
        g.nextProgress = now + kProgressGapMs;
        ++g.progressLogs;
        LogProgress(since);
    }
    if (!g.coverDone && (g.coopEntrance || since >= kCoverAfterMs)) RemoveCover();
    if (!g.tripwireDone && since >= kTripwireAfterMs) { g.tripwireDone = true; LogTripwire("30 s in"); }
    if (!g.coopEntrance && !g.coopTimeoutLogged && since >= kTimeoutMs) {
        g.coopTimeoutLogged = true;
        HSM_LOG("crypts layout: co-op: %u s and the entrance rooms are not built (%zu of %u rooms known); the fighters go in without them",
                kTimeoutMs / 1000, g.coopRooms.size(), unsigned(rooms.Known()));
    }
}

bool CoopLive() { return g.coopLive; }

bool Active() { return g.layout != nullptr || g.coopLive; }

int Pending() {
    if (!g.layout && !g.coopLive) return 0;
    int n = 0;
    for (const Stream& s : g.streams) n += !s.level && !s.failed;
    if (g.coopLive && !g.coopEntrance) ++n;   // the entrance rooms: not known yet, or not built
    return n;
}

bool Loading() { return (g.layout || g.coopLive) && !g.built && !g.timeoutLogged && !g.coopTimeoutLogged && Pending() > 0; }

bool InLayout(const UObject* level) { return (g.layout || g.coopLive) && level && g.shown.count(level) != 0; }

const crypts::Layout* Current() { return g.layout; }

int RowOfLevel(const UObject* level) {
    if ((!g.layout && !g.coopLive) || !level) return -1;
    for (const Stream& s : g.streams) if (s.level == level) return s.row;   // any part: spawners stand in all three
    return -1;
}

}  // namespace hsm::game::cryptslayout
