#include "gameplay/adapter.h"
#include <windows.h>
#include "gameplay/combat_sync.h"
#include "hsm_owner.h"
#include "log.h"
#include "sync/hsm_gore.h"
#include "ue/hook.h"
#include <algorithm>
#include <climits>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <cwchar>
#include <iterator>

namespace hsm::game {
using namespace hsm::ue;
namespace {

struct FObjArray { UObject** data = nullptr; int32_t num = 0, max = 0; };
constexpr const char* kWillieClass = "Willie_BP_C";
constexpr const char* kGameInstanceClass = "GI_Settings_C";
constexpr const char* kSaveGameClass = "SG_GameProgress_C";
constexpr const char* kSpawnerClass = "BP_SpawnerPoint_Willies_C";
constexpr const char* kPassportStruct = "Str_Passport_Character1";
constexpr const char* kPassportFreeMode = "Free Mode Player Character 1";      // GI_Settings, Free Mode
constexpr const char* kPassportProgression = "Player Character";              // GI_Settings, Progression career fighter
constexpr const char* kPassportDefault = "Free Mode Player Character 2";      // GI_Settings, stock player-2 slot
constexpr const char* kSavePassport = "Free Mode Player Character 1";         // SG_GameProgress_C field carrying the blob

// Input event names as generated for Willie_BP from DefaultInput.ini mappings (order = protocol enums).
constexpr const char* kAxisNames[kAxisCount] = {
    "Move Forward / Backward", "Move Right / Left", "Turn Right / Left Mouse", "Look Up / Down Mouse",
    "Turn Right / Left Gamepad", "Look Up / Down Gamepad", "Right Arm Axis", "Left Arm Axis",
    "Right Guard Axis", "Left Guard Axis",
};
constexpr const char* kActionNames[kActionCount] = {
    "Grab Left", "Grab Right", "Guard Left", "Guard Right", "Thrust", "Thrust Gamepad", "Jump", "Crouch Key",
    "Crouch Hold", "Run", "Swap Hands", "Toggle Camera Lock", "Key Forward", "Key Back", "Key Left", "Key Right",
    "Talk", "RightMouseButton", "MiddleMouseButton",
};
// Simulated body bones (UE mannequin layout used by the Willie skeleton). Missing bones are skipped.
constexpr const char* kBoneCandidates[] = {
    "pelvis", "spine_01", "spine_02", "spine_03", "spine_04", "spine_05", "neck_01", "neck_02", "head",
    "clavicle_l", "upperarm_l", "lowerarm_l", "hand_l", "clavicle_r", "upperarm_r", "lowerarm_r", "hand_r",
    "thigh_l", "calf_l", "foot_l", "thigh_r", "calf_r", "foot_r",
};
constexpr const char* kPartHealth[7] = { "Head Health", "Body Upper Health", "Body Lower Health", "Arm_R Health", "Arm_L Health", "Leg_R Health", "Leg_L Health" };

// Velocity tracking gains: reach the target over ~tau seconds, with caps so a bad sample cannot fling bodies.
constexpr double kTau = 0.05, kMaxLinear = 3000.0, kMaxAngularDeg = 1500.0;   // 50 ms: puppets keep up with fast swings

int TrailingNumber(const std::string& s) {
    size_t i = s.size(); while (i > 0 && std::isdigit(static_cast<unsigned char>(s[i - 1]))) --i;
    return i < s.size() ? std::atoi(s.c_str() + i) : -1;
}
double Clamp(double v, double lim) { return v > lim ? lim : v < -lim ? -lim : v; }

FQuat Mul(const FQuat& a, const FQuat& b) {
    return { a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y, a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
             a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w, a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z };
}
FQuat Conj(const FQuat& q) { return { -q.x, -q.y, -q.z, q.w }; }
}  // namespace

bool Adapter::Resolve(UObject* worldContext) {
    world_ = worldContext;
    if (!statics_) if (UObject* c = FindObject("GameplayStatics", "Class")) statics_ = DefaultObject(reinterpret_cast<UClass*>(c));
    return statics_ != nullptr && world_ != nullptr;
}

UObject* Adapter::PlayerWillie(int localIndex) {
    Call c(statics_, "GetPlayerCharacter");
    c.set<UObject*>("WorldContextObject", world_).set<int32_t>("PlayerIndex", localIndex);
    if (!c.invoke()) return nullptr;
    UObject* ch = c.get<UObject*>("ReturnValue");
    if (!ch) return nullptr;
    if (!willieClass_) {
        UClass* cls = ClassOf(ch);
        if (ObjectName(reinterpret_cast<UObject*>(cls)) == kWillieClass) willieClass_ = cls;
    }
    return willieClass_ && IsA(ch, willieClass_) ? ch : nullptr;
}

// Same calls the game makes when a menu closes (decomp UI/UI_DeathDoor.cpp:118-119): our session travel skips the
// stock menu flow that would otherwise leave the menu's UI input mode and cursor behind.
void Adapter::RestoreGameInput() {
    Call pc(statics_, "GetPlayerController");
    pc.set<UObject*>("WorldContextObject", world_).set<int32_t>("PlayerIndex", 0);
    UObject* ctrl = pc.invoke() ? pc.get<UObject*>("ReturnValue") : nullptr;
    if (!ctrl) return;
    static UObject* wbl = nullptr;
    if (!wbl) if (UObject* cls = FindObject("WidgetBlueprintLibrary", "Class")) wbl = DefaultObject(reinterpret_cast<UClass*>(cls));
    Call mode(wbl, "SetInputMode_GameOnly");
    mode.set<UObject*>("PlayerController", ctrl).set<bool>("bFlushInput", false).invoke();
    SetBool(ctrl, "bShowMouseCursor", false);
}

bool Adapter::FloorZ(const FVector& at, UObject* ignore, double& z) {
    static UObject* ksl = nullptr;
    if (!ksl) if (UObject* cls = FindObject("KismetSystemLibrary", "Class")) ksl = DefaultObject(reinterpret_cast<UClass*>(cls));
    struct ActorArray { UObject** data; int32_t num, max; };   // TArray<AActor*> view over a local; the engine only reads it
    UObject* ignored[1] = {ignore};
    ActorArray skip{ignored, ignore ? 1 : 0, 1};
    Call t(ksl, "LineTraceSingle");
    t.set<UObject*>("WorldContextObject", world_).set("Start", FVector{at.x, at.y, at.z + 150}).set("End", FVector{at.x, at.y, at.z - 600})
     .set<uint8_t>("TraceChannel", 0).set<bool>("bTraceComplex", false).set("ActorsToIgnore", skip).set<uint8_t>("DrawDebugType", 0)
     .set<bool>("bIgnoreSelf", true).set<float>("DrawTime", 0.f);
    if (!t.invoke() || !t.get<bool>("ReturnValue")) return false;
    const uint8_t* hit = static_cast<const uint8_t*>(t.paramPtr("OutHit"));
    if (!hit) return false;
    FVector ip; std::memcpy(&ip, hit + 40, sizeof ip);   // FHitResult::ImpactPoint
    if (!std::isfinite(ip.z)) return false;
    z = ip.z;
    return true;
}

bool Adapter::ControlRotation(int localIndex, float& yaw, float& pitch) {
    Call pc(statics_, "GetPlayerController");
    pc.set<UObject*>("WorldContextObject", world_).set<int32_t>("PlayerIndex", localIndex);
    UObject* ctrl = pc.invoke() ? pc.get<UObject*>("ReturnValue") : nullptr;
    if (!ctrl) return false;
    Call r(ctrl, "GetControlRotation");
    if (!r.invoke()) return false;
    FRotator rot = r.get<FRotator>("ReturnValue");
    yaw = float(std::remainder(rot.yaw, 360.0)); pitch = float(std::remainder(rot.pitch, 360.0));
    return std::isfinite(yaw) && std::isfinite(pitch);
}

UObject* Adapter::GameInstance() {
    std::vector<UObject*> gis = FindInstances(kGameInstanceClass);
    return gis.empty() ? nullptr : gis[0];
}

// HSMMP_tmp_<pid>_<n>.sav: a slot of another running instance (a local host + client test) is still in use.
static bool OwnerAlive(const wchar_t* file) {
    unsigned long pid = 0;
    if (std::swscanf(file, L"HSMMP_tmp_%lu_", &pid) != 1 || !pid) return false;
    HANDLE p = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!p) return false;
    DWORD code = 0;
    bool alive = GetExitCodeProcess(p, &code) && code == STILL_ACTIVE;
    CloseHandle(p);
    return alive;
}

// SaveGameToMemory/LoadGameFromMemory are C++-only in this engine build (not reflected, and inlined into the slot
// functions), so the blob goes through a temporary save slot: SaveGameToSlot writes exactly the bytes
// SaveGameToMemory would produce (GVAS header + object), which are read back from Saved/SaveGames and deleted.
std::wstring Adapter::SaveGamesDir() {
    if (!saveDir_.empty()) return saveDir_;
    UObject* ksl = nullptr;
    if (UObject* cls = FindObject("KismetSystemLibrary", "Class")) ksl = DefaultObject(reinterpret_cast<UClass*>(cls));
    Call dir(ksl, "GetProjectSavedDirectory");
    if (!dir.invoke()) return {};
    FStringArg rel = dir.get<FStringArg>("ReturnValue");
    Call abs(ksl, "ConvertToAbsolutePath");
    abs.set("Filename", rel);
    if (!abs.invoke()) return {};
    FStringArg full = abs.get<FStringArg>("ReturnValue");
    std::string path = ReadFString(&full);
    if (path.empty()) return {};
    saveDir_.assign(path.begin(), path.end());
    if (saveDir_.back() != L'/' && saveDir_.back() != L'\\') saveDir_ += L'/';
    saveDir_ += L"SaveGames/";
    // Sweep temporary slots a crashed session may have left in the player's save folder.
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((saveDir_ + L"HSMMP_tmp_*.sav").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        int removed = 0;
        do { if (!OwnerAlive(fd.cFileName) && DeleteFileW((saveDir_ + fd.cFileName).c_str())) ++removed; } while (FindNextFileW(h, &fd));
        FindClose(h);
        if (removed) HSM_LOG("adapter: removed %d leftover temporary save slot(s)", removed);
    }
    return saveDir_;
}

std::string Adapter::TempSlot() {
    char b[64];
    std::snprintf(b, sizeof b, "HSMMP_tmp_%lu_%u", GetCurrentProcessId(), ++slotCounter_);
    return b;
}

void Adapter::DeleteSlot(const std::string& slot) {
    FStringArgHolder name(slot);
    Call(statics_, "DeleteGameInSlot").set("SlotName", name.arg).set<int32_t>("UserIndex", 0).invoke();
}

bool Adapter::SaveToMemory(UObject* saveGame, std::vector<uint8_t>& out) {
    std::wstring dir = SaveGamesDir();
    if (dir.empty()) { HSM_LOG("adapter: Saved directory unknown"); return false; }
    std::string slot = TempSlot();
    FStringArgHolder name(slot);
    Call s(statics_, "SaveGameToSlot");
    s.set<UObject*>("SaveGameObject", saveGame).set("SlotName", name.arg).set<int32_t>("UserIndex", 0);
    bool ok = s.invoke() && s.get<bool>("ReturnValue");
    std::wstring file = dir + std::wstring(slot.begin(), slot.end()) + L".sav";
    if (ok) {
        if (FILE* f = _wfopen(file.c_str(), L"rb")) {
            std::fseek(f, 0, SEEK_END); long n = std::ftell(f); std::fseek(f, 0, SEEK_SET);
            if (n > 0 && n <= 64L * 1024 * 1024) { out.resize(size_t(n)); ok = std::fread(out.data(), 1, out.size(), f) == out.size(); }
            else ok = false;
            std::fclose(f);
        } else ok = false;
    }
    DeleteSlot(slot);
    if (!ok) HSM_LOG("adapter: SaveGameToSlot(%s) failed", slot.c_str());
    return ok && !out.empty();
}

bool Adapter::ExportPassport(std::vector<uint8_t>& out) {
    UObject* gi = GameInstance();
    if (!gi) { HSM_LOG("adapter: export needs %s", kGameInstanceClass); return false; }
    bool freeMode = true;
    GetBool(gi, "Free Mode Activated", freeMode);
    const char* source = freeMode ? kPassportFreeMode : kPassportProgression;
    bool ok = SavePassportFrom(gi, FindProperty(ClassOf(gi), source), out);
    HSM_LOG("adapter: exported %s passport (%zu bytes)%s", freeMode ? "Free Mode" : "Progression", out.size(), ok ? "" : " FAILED");
    return ok;
}

bool Adapter::ExportPassportOf(UObject* willie, std::vector<uint8_t>& out) {
    return willie && SavePassportFrom(willie, FindProperty(ClassOf(willie), "Character Passport"), out);
}

// Copies one passport-typed property of `owner` into a fresh SG_GameProgress_C and serializes it.
bool Adapter::SavePassportFrom(UObject* owner, FProperty* src, std::vector<uint8_t>& out) {
    UClass* sgClass = FindClass(kSaveGameClass);
    if (!sgClass || !src) return false;
    Call c(statics_, "CreateSaveGameObject");
    c.set<UClass*>("SaveGameClass", sgClass);
    UObject* sg = c.invoke() ? c.get<UObject*>("ReturnValue") : nullptr;
    FProperty* dst = sg ? FindProperty(ClassOf(sg), kSavePassport) : nullptr;
    if (!dst || PropertySize(src) != PropertySize(dst) || PropertyType(src) != PropertyType(dst)) {
        HSM_LOG("adapter: passport properties missing or mismatched (%s)", PropertyName(src).c_str()); return false;
    }
    CopyPropertyValue(dst, reinterpret_cast<uint8_t*>(sg) + PropertyOffset(dst), reinterpret_cast<uint8_t*>(owner) + PropertyOffset(src));
    return SaveToMemory(sg, out);
}

UObject* Adapter::LoadPassportSave(const std::vector<uint8_t>& blob) {
    if (blob.empty() || blob.size() > 64u * 1024 * 1024) return nullptr;
    UClass* sgClass = FindClass(kSaveGameClass);
    std::wstring dir = SaveGamesDir();
    if (dir.empty()) return nullptr;
    std::string slot = TempSlot();
    std::wstring file = dir + std::wstring(slot.begin(), slot.end()) + L".sav";
    FILE* f = _wfopen(file.c_str(), L"wb");
    if (!f) { HSM_LOG("adapter: cannot write temporary save slot"); return nullptr; }
    bool wrote = std::fwrite(blob.data(), 1, blob.size(), f) == blob.size();
    std::fclose(f);
    UObject* sg = nullptr;
    if (wrote) {
        FStringArgHolder name(slot);
        Call c(statics_, "LoadGameFromSlot");
        c.set("SlotName", name.arg).set<int32_t>("UserIndex", 0);
        sg = c.invoke() ? c.get<UObject*>("ReturnValue") : nullptr;
    }
    DeleteSlot(slot);
    _wremove(file.c_str());                              // in case DeleteGameInSlot did not (bad data)
    if (!sg || !sgClass || !IsA(sg, sgClass)) { HSM_LOG("adapter: loadout blob is not a %s", kSaveGameClass); return nullptr; }
    return sg;
}

// Copies the passport into a deferred-spawned Willie: from the blob's SaveGame, or the stock player-2 slot.
bool Adapter::CopyPassport(UObject* willie, const SpawnOptions& o, double& height) {
    FProperty* dst = FindProperty(ClassOf(willie), "Character Passport");
    const uint8_t* from = static_cast<const uint8_t*>(o.passport);
    if (!from) {
        UObject* owner = nullptr; FProperty* src = nullptr;
        if (o.blob) { owner = LoadPassportSave(*o.blob); src = owner ? FindProperty(ClassOf(owner), kSavePassport) : nullptr; }
        if (!src) { owner = GameInstance(); src = owner ? FindProperty(ClassOf(owner), kPassportDefault) : nullptr; }
        if (!src || (dst && PropertySize(src) != PropertySize(dst))) { HSM_LOG("adapter: cannot copy passport"); return false; }
        from = reinterpret_cast<uint8_t*>(owner) + PropertyOffset(src);
    }
    if (!dst) { HSM_LOG("adapter: Willie has no Character Passport"); return false; }
    CopyPropertyValue(dst, reinterpret_cast<uint8_t*>(willie) + PropertyOffset(dst), from);
    height = 0.5;
    if (UObject* st = FindObject(kPassportStruct, "UserDefinedStruct"))
        if (FProperty* h = FindPropertyPrefix(st, "Height_"); h && PropertySize(h) == sizeof(double)) std::memcpy(&height, from + PropertyOffset(h), sizeof height);
    if (!std::isfinite(height)) height = 0.5;
    return true;
}

UObject* Adapter::SpawnFighter(const SpawnOptions& o) {
    UClass* willie = FindClass(kWillieClass);
    UClass* aiCtrl = o.brain && !o.stockBrain ? FindClass("AIController") : nullptr;
    if (!willie || (o.brain && !o.stockBrain && !aiCtrl)) { HSM_LOG("adapter: spawn classes missing"); return nullptr; }
    double rad = o.yawDeg * 0.017453292519943295;
    FTransform t; t.rotation = {0, 0, std::sin(rad / 2), std::cos(rad / 2)}; t.translation = o.at;
    Call begin(statics_, "BeginDeferredActorSpawnFromClass");
    begin.set<UObject*>("WorldContextObject", world_).set<UClass*>("ActorClass", willie).set("SpawnTransform", t)
         .set<uint8_t>("CollisionHandlingOverride", 2).set<UObject*>("Owner", nullptr).set<uint8_t>("TransformScaleMethod", 1);
    UObject* w = begin.invoke() ? begin.get<UObject*>("ReturnValue") : nullptr;
    if (!w) { HSM_LOG("adapter: BeginDeferredActorSpawnFromClass failed"); return nullptr; }
    double height = 0.5;
    CopyPassport(w, o, height);
    if (int32_t* team = Field<int32_t>(w, "Team Int")) *team = o.teamInt;
    if (!o.stockBrain) {
        if (UClass** cls = Field<UClass*>(w, "AIControllerClass")) *cls = aiCtrl;
        if (uint8_t* ap = Field<uint8_t>(w, "AutoPossessAI")) *ap = o.brain ? 3 : 0;   // EAutoPossessAI PlacedInWorldOrSpawned / Disabled
    }
    // Same height mapping as the stock spawner: lift 0..10 cm and scale 0.875..1.
    height = std::clamp(height, 0.0, 1.0);
    t.translation.z += 10.0 * height;
    double sc = 0.875 + 0.125 * height; t.scale = {sc, sc, sc};
    Call finish(statics_, "FinishSpawningActor");
    finish.set<UObject*>("Actor", w).set("SpawnTransform", t).set<uint8_t>("TransformScaleMethod", 1);
    UObject* done = finish.invoke() ? finish.get<UObject*>("ReturnValue") : nullptr;
    if (!done) { HSM_LOG("adapter: FinishSpawningActor failed"); return nullptr; }
    if (o.brain && !o.stockBrain) {
        UObject* ctrl = ControllerOf(done);
        if (!ctrl) HSM_LOG("adapter: spawned fighter has no controller");
        else SetBool(ctrl, "bSetControlRotationFromPawnOrientation", false);   // aim comes from the remote player
    }
    HSM_LOG("adapter: spawned %s at (%.0f, %.0f, %.0f)%s", o.stockBrain ? "bot" : o.brain ? "fighter" : "puppet", o.at.x, o.at.y, o.at.z, o.blob || o.passport ? "" : " with default loadout");
    return done;
}

UObject* Adapter::ControllerOf(UObject* pawn) {
    Call c(pawn, "GetController");
    return c.invoke() ? c.get<UObject*>("ReturnValue") : nullptr;
}

void Adapter::KeepRemoteFighter(UObject* willie, float yawDeg, float pitchDeg) {
    SetBool(willie, "Player", true);   // Willie computes Player once from local player 0/1; mod fighters are players too
    if (UObject* ctrl = ControllerOf(willie)) Call(ctrl, "SetControlRotation").set("NewRotation", FRotator{pitchDeg, yawDeg, 0}).invoke();
}

void Adapter::DestroyActor(UObject* actor) { if (actor) Call(actor, "K2_DestroyActor").invoke(); }

bool Adapter::BeingDestroyed(UObject* actor) {
    static UFunction* fn = nullptr;
    if (!fn) if (UClass* actorClass = FindClass("Actor")) fn = FindFunction(actorClass, "IsActorBeingDestroyed");
    if (!actor || !fn) return false;
    Call c(actor, fn);
    return c.invoke() && c.get<bool>("ReturnValue");
}

bool Adapter::StillValid(UObject* o, int32_t index) const {
    if (!o || index < 0 || ObjectAt(index) != o) return false;
    return !destroyAware_ || !BeingDestroyed(o);
}

void Adapter::DestroyFighter(UObject* willie, int32_t index) {
    if (!StillValid(willie, index)) return;
    std::vector<UObject*> carried;
    for (const char* f : {"Weapon R", "Weapon L", "Attached Bolt Quiver"})
        if (UObject** p = Field<UObject*>(willie, f); p && *p) carried.push_back(*p);
    Call att(willie, "GetAttachedActors");
    att.set<bool>("bResetArray", true).set<bool>("bRecursivelyIncludeAttachedActors", true);
    if (att.invoke())
        if (FObjArray* arr = static_cast<FObjArray*>(att.paramPtr("OutActors")); arr && arr->num > 0 && arr->num <= 256)
            for (int32_t i = 0; i < arr->num; ++i) carried.push_back(arr->data[i]);
    std::sort(carried.begin(), carried.end());
    carried.erase(std::unique(carried.begin(), carried.end()), carried.end());
    int destroyed = 0;
    for (UObject* a : carried) {
        if (!a || a == willie || IsA(a, FindClass(kWillieClass))) continue;
        DestroyActor(a);
        ++destroyed;
    }
    DestroyActor(willie);
    HSM_LOG("adapter: destroyed %s and %d carried actor(s)", ObjectName(willie).c_str(), destroyed);
}

FVector Adapter::ActorLocation(UObject* actor) {
    Call c(actor, "K2_GetActorLocation");
    return c.invoke() ? c.get<FVector>("ReturnValue") : FVector{};
}

double Adapter::ActorYaw(UObject* actor) {
    Call c(actor, "K2_GetActorRotation");
    return c.invoke() ? c.get<FRotator>("ReturnValue").yaw : 0.0;
}

std::vector<FVector> Adapter::SpawnCandidates() {
    std::vector<FVector> out;
    for (UObject* a : FindInstances(kSpawnerClass)) out.push_back(ActorLocation(a));
    return out;
}

void Adapter::Teleport(UObject* actor, const FVector& to) {
    Call c(actor, "K2_SetActorLocation");
    c.set("NewLocation", to).set<bool>("bSweep", false).set<bool>("bTeleport", true).invoke();
}

bool Adapter::BindInputs(UObject* willie) {
    if (!bindings_.empty()) return true;
    UClass* cls = ClassOf(willie);
    for (size_t i = 0; i < kAxisCount; ++i) {
        auto fns = FunctionsWithPrefix(cls, std::string("InpAxisEvt_") + kAxisNames[i] + "_K2Node");
        if (fns.empty()) { HSM_LOG("adapter: no axis event for %s", kAxisNames[i]); continue; }
        axisFn_[i] = fns[0];
        bindings_[fns[0]] = {true, i, false};
    }
    for (size_t i = 0; i < kActionCount; ++i) {
        auto fns = FunctionsWithPrefix(cls, std::string("InpActEvt_") + kActionNames[i] + "_K2Node");
        if (fns.empty()) continue;   // e.g. Guard Left/Right are mapped in config but unused by Willie
        // Each action node compiles to a pressed event and (optionally) a released event with a higher node index.
        std::sort(fns.begin(), fns.end(), [](UFunction* a, UFunction* b) {
            return TrailingNumber(ObjectName(reinterpret_cast<UObject*>(a))) < TrailingNumber(ObjectName(reinterpret_cast<UObject*>(b))); });
        pressFn_[i] = fns[0]; bindings_[fns[0]] = {false, i, true};
        if (fns.size() > 1) { releaseFn_[i] = fns[1]; bindings_[fns[1]] = {false, i, false}; }
    }
    HSM_LOG("adapter: bound %zu input events", bindings_.size());
    return !bindings_.empty();
}

float Adapter::ReadAxisParam(UFunction* fn, void* params) {
    FProperty* p = FindProperty(fn, "AxisValue");
    if (!p || !params) return 0.f;
    float v; std::memcpy(&v, static_cast<uint8_t*>(params) + PropertyOffset(p), sizeof v);
    return std::isfinite(v) ? v : 0.f;
}

void Adapter::ApplyAxis(UObject* willie, Axis a, float value) {
    if (UFunction* fn = axisFn_[size_t(a)]) Call(willie, fn).set<float>("AxisValue", value).invoke();
}

void Adapter::FireAction(UObject* willie, Action a, bool pressed) {
    UFunction* fn = pressed ? pressFn_[size_t(a)] : releaseFn_[size_t(a)];
    if (fn) Call(willie, fn).invoke();   // FKey parameter left zeroed; Willie's handlers do not read it
}

UObject* Adapter::MeshOf(UObject* willie) {
    UObject** mesh = Field<UObject*>(willie, "Mesh");
    return mesh ? *mesh : nullptr;
}

bool Adapter::ResolveBones(UObject* mesh) {
    if (bonesResolved_) return true;
    for (const char* b : kBoneCandidates) {
        FName n = MakeName(b);
        Call c(mesh, "GetBoneIndex");
        c.setName("BoneName", n).invoke();
        if (c.get<int32_t>("ReturnValue") >= 0) { bones_.push_back(n); boneNames_.push_back(b); }
    }
    bonesResolved_ = true;
    HSM_LOG("adapter: %zu/%zu body bones present", bones_.size(), sizeof kBoneCandidates / sizeof kBoneCandidates[0]);
    return !bones_.empty();
}

bool Adapter::ReadState(UObject* willie, uint8_t entity, WillieState& out) {
    UObject* mesh = MeshOf(willie);
    if (!mesh || !ResolveBones(mesh)) return false;
    out = WillieState{}; out.entity = entity;
    bool b = false;
    if (GetBool(willie, "DED", b) && b) out.flags |= 1;
    if (GetBool(willie, "Fallen", b) && b) out.flags |= 2;
    if (GetBool(willie, "Headless", b) && b) out.flags |= 4;
    auto d = [&](const char* name) { double* p = Field<double>(willie, name); return p ? float(*p) : 0.f; };
    out.health = d("Health"); out.consciousness = d("Consciousness"); out.stamina = d("Stamina"); out.bleeding = d("Bleeding");
    for (size_t i = 0; i < 7; ++i) out.partHealth[i] = d(kPartHealth[i]);
    out.bodies.reserve(bones_.size());
    for (const FName& bone : bones_) {
        Call c(mesh, "GetSocketTransform");
        c.setName("InSocketName", bone).set<uint8_t>("TransformSpace", 0).invoke();   // RTS_World
        FTransform t = c.get<FTransform>("ReturnValue");
        BodyState s; s.px = float(t.translation.x); s.py = float(t.translation.y); s.pz = float(t.translation.z);
        s.rot = PackQuat(float(t.rotation.x), float(t.rotation.y), float(t.rotation.z), float(t.rotation.w));
        out.bodies.push_back(s);
    }
    return true;
}

bool Adapter::ReadVitals(UObject* willie, uint8_t entity, VitalsMsg& out) {
    double* health = willie ? Field<double>(willie, "Health") : nullptr;
    if (!health) return false;
    out = VitalsMsg{}; out.entity = entity;
    auto d = [&](const char* name) { double* p = Field<double>(willie, name); return p && std::isfinite(*p) ? float(*p) : 0.f; };
    out.health = d("Health"); out.consciousness = d("Consciousness"); out.stamina = d("Stamina"); out.bleeding = d("Bleeding");
    for (size_t i = 0; i < 7; ++i) out.partHealth[i] = d(kPartHealth[i]);
    bool b = false;
    if (GetBool(willie, "DED", b) && b) out.flags |= 1;
    if (GetBool(willie, "Fallen", b) && b) out.flags |= 2;
    if (Field<double>(willie, "Consciousness") && out.consciousness <= 0.f) out.flags |= 4;
    out.severed = ReadSeveredMask(willie);
    // Bit per dislocatable joint, in sync/hsm_gore.h's order (Willie_BP.cpp:1250-1255): the stand-ins free that joint.
    constexpr const char* kDislocatedBoolFields[] = { "Arm R Dislocated", "Arm L Dislocated", "Leg R Dislocated",
                                                      "Leg L Dislocated", "Neck Dislocated", "Spine Dislocated" };
    static_assert(std::size(kDislocatedBoolFields) == sync::kGoreJoints, "one name per gore joint bit");
    for (uint32_t j = 0; j < sync::kGoreJoints; ++j)
        if (GetBool(willie, kDislocatedBoolFields[j], b) && b) out.dislocated |= 1u << j;
    return true;
}

void Adapter::DriveTowards(UObject* willie, const WillieState& target, float dt) {
    UObject* mesh = MeshOf(willie);
    if (!mesh || !ResolveBones(mesh) || dt <= 0.f) return;
    size_t n = std::min(bones_.size(), target.bodies.size());
    for (size_t i = 0; i < n; ++i) {
        Call get(mesh, "GetSocketTransform");
        get.setName("InSocketName", bones_[i]).set<uint8_t>("TransformSpace", 0).invoke();
        FTransform cur = get.get<FTransform>("ReturnValue");
        const BodyState& tb = target.bodies[i];
        FVector lin{ Clamp((tb.px - cur.translation.x) / kTau, kMaxLinear), Clamp((tb.py - cur.translation.y) / kTau, kMaxLinear),
                     Clamp((tb.pz - cur.translation.z) / kTau, kMaxLinear) };
        float qx, qy, qz, qw; UnpackQuat(tb.rot, qx, qy, qz, qw);
        FQuat err = Mul(FQuat{qx, qy, qz, qw}, Conj(cur.rotation));
        if (err.w < 0) err = { -err.x, -err.y, -err.z, -err.w };
        double s = std::sqrt(err.x * err.x + err.y * err.y + err.z * err.z);
        FVector ang{};
        if (s > 1e-6) {
            double angleDeg = 2.0 * std::atan2(s, err.w) * 57.29577951308232;
            double k = angleDeg / kTau / s;
            ang = { Clamp(err.x * k, kMaxAngularDeg), Clamp(err.y * k, kMaxAngularDeg), Clamp(err.z * k, kMaxAngularDeg) };
        }
        Call(mesh, "SetPhysicsLinearVelocity").set("NewVel", lin).set<bool>("bAddToCurrent", false).setName("BoneName", bones_[i]).invoke();
        Call(mesh, "SetPhysicsAngularVelocityInDegrees").set("NewAngVel", ang).set<bool>("bAddToCurrent", false).setName("BoneName", bones_[i]).invoke();
    }
}

// Each bone's velocity moves `blend` of the way toward the one DriveTowards would set; angular motion is left to the
// local simulation, so an input-simulated fighter is pulled back gradually instead of snapping.
void Adapter::NudgeTowards(UObject* willie, const WillieState& target, float blend) {
    UObject* mesh = MeshOf(willie);
    if (!mesh || !ResolveBones(mesh)) return;
    size_t n = std::min(bones_.size(), target.bodies.size());
    for (size_t i = 0; i < n; ++i) {
        Call get(mesh, "GetSocketTransform");
        get.setName("InSocketName", bones_[i]).set<uint8_t>("TransformSpace", 0).invoke();
        FTransform cur = get.get<FTransform>("ReturnValue");
        Call vel(mesh, "GetPhysicsLinearVelocity");
        vel.setName("BoneName", bones_[i]).invoke();
        FVector v = vel.get<FVector>("ReturnValue");
        const BodyState& tb = target.bodies[i];
        FVector want{ Clamp((tb.px - cur.translation.x) / kTau, kMaxLinear), Clamp((tb.py - cur.translation.y) / kTau, kMaxLinear),
                      Clamp((tb.pz - cur.translation.z) / kTau, kMaxLinear) };
        FVector out{ v.x + (want.x - v.x) * blend, v.y + (want.y - v.y) * blend, v.z + (want.z - v.z) * blend };
        Call(mesh, "SetPhysicsLinearVelocity").set("NewVel", out).set<bool>("bAddToCurrent", false).setName("BoneName", bones_[i]).invoke();
    }
}

bool Adapter::FirstBodyPosition(UObject* willie, FVector& out) {
    UObject* mesh = MeshOf(willie);
    if (!mesh || !ResolveBones(mesh) || bones_.empty()) return false;
    Call get(mesh, "GetSocketTransform");
    get.setName("InSocketName", bones_[0]).set<uint8_t>("TransformSpace", 0);
    if (!get.invoke()) return false;
    out = get.get<FTransform>("ReturnValue").translation;
    return true;
}

void Adapter::ApplyVitals(UObject* willie, const WillieState& s) {
    auto put = [&](const char* name, float v) { if (double* p = Field<double>(willie, name)) *p = v; };
    put("Health", s.health); put("Consciousness", s.consciousness); put("Stamina", s.stamina); put("Bleeding", s.bleeding);
    for (size_t i = 0; i < 7; ++i) put(kPartHealth[i], s.partHealth[i]);
}

void Adapter::TriggerDeath(UObject* willie) { Call(willie, "Death").invoke(); }

}  // namespace hsm::game

namespace hsm::game {
namespace {
constexpr const char* kWeaponClass = "ModularWeaponBP_C";
constexpr const char* kArmorClass = "BP_Armor_Master_C";
constexpr const char* kArmorStruct = "Str_Passport_Armor1";
constexpr const char* kStockScreens[] = {"UI_DED_C", "UI_DeathDoor_C", "UI_WinScreen_C", "UI_WIN_C"};
// Owner sync only: these pause the game (UI_Lose, UI_GiveUp, UI_NextFIght all call SetGamePaused(true)), which stops
// that player's stream, and offer "return to tavern / another fight". Host sync keeps its old list.
constexpr const char* kOwnerMatchScreens[] = {"UI_Lose_C", "UI_GiveUp_C", "UI_NextFIght_C"};
bool g_suppressScreens = false;
bool g_suppressOwnerScreens = false;
bool g_screenHooks = false;
std::vector<std::pair<UObject*, int32_t>> g_suppressed;   // widgets whose Construct was skipped; removed next tick
bool ClassNamed(UObject* o, const char* name) { return o && ObjectName(reinterpret_cast<UObject*>(ClassOf(o))) == name; }
}  // namespace

bool Adapter::IsWillie(UObject* o) {
    if (!willieClass_) willieClass_ = FindClass(kWillieClass);
    return o && willieClass_ && IsA(o, willieClass_);
}

bool Adapter::FreeModeActive() { bool f = true; UObject* gi = GameInstance(); if (gi) GetBool(gi, "Free Mode Activated", f); return f; }

void Adapter::PrepareFreeModeFight(int foes) {
    UObject* gi = GameInstance();
    if (!gi) return;
    SetBool(gi, "Free Mode Activated", true);
    if (foes >= 0) if (int32_t* f = Field<int32_t>(gi, "Free Mode Foes Amount")) *f = foes;
    bool carnage = false;   // the level manager's own Free Mode combat mode (decomp BP_LevelManager.cpp:67)
    GetBool(gi, "Free Mode Carnage", carnage);
    if (uint8_t* cm = Field<uint8_t>(gi, "Current Combat Mode")) *cm = carnage ? 3 : 1;
    if (uint8_t* p = Field<uint8_t>(gi, "Current Play Mode")) *p = 1;
    if (uint8_t* m = Field<uint8_t>(gi, "Current Game Mode enum")) *m = 1;
}

int Adapter::FreeModeFoes() {
    UObject* gi = GameInstance();
    int32_t* f = gi ? Field<int32_t>(gi, "Free Mode Foes Amount") : nullptr;
    return f ? *f : -1;
}
void Adapter::SetFreeModeFoes(int foes) {
    if (UObject* gi = GameInstance()) if (int32_t* f = Field<int32_t>(gi, "Free Mode Foes Amount")) *f = foes;
}

bool Adapter::GamePaused() {
    Call c(statics_, "IsGamePaused");
    c.set<UObject*>("WorldContextObject", world_);
    return c.invoke() && c.get<bool>("ReturnValue");
}

void Adapter::SetGamePaused(bool paused) {
    Call(statics_, "SetGamePaused").set<UObject*>("WorldContextObject", world_).set<bool>("bPaused", paused).invoke();
}

void Adapter::QuitGame() {
    UObject* ksl = nullptr;
    if (UObject* cls = FindObject("KismetSystemLibrary", "Class")) ksl = DefaultObject(reinterpret_cast<UClass*>(cls));
    Call q(ksl, "QuitGame");
    q.set<UObject*>("WorldContextObject", world_).set<UObject*>("SpecificPlayer", nullptr).set<uint8_t>("QuitPreference", 0).set<bool>("bIgnorePlatformRestrictions", true);
    if (!q.invoke()) ExitProcess(0);
}

void Adapter::ConsoleCommand(const std::string& command) {
    UObject* ksl = nullptr;
    if (UObject* cls = FindObject("KismetSystemLibrary", "Class")) ksl = DefaultObject(reinterpret_cast<UClass*>(cls));
    FStringArgHolder cmd(command);
    Call(ksl, "ExecuteConsoleCommand").set<UObject*>("WorldContextObject", world_).set("Command", cmd.arg).set<UObject*>("SpecificPlayer", nullptr).invoke();
}

bool Adapter::IsDead(UObject* willie) { bool d = false; return GetBool(willie, "DED", d) && d; }

bool Adapter::IsWeapon(UObject* actor) { return ClassNamed(actor, kWeaponClass); }

UObject* Adapter::FighterOf(UObject* actor) {
    for (int depth = 0; actor && depth < 4; ++depth) {
        if (IsWillie(actor)) return actor;
        if (IsWeapon(actor)) {
            UObject** p = Field<UObject*>(actor, "Parent Actor");
            if (p && *p) return *p;
            UObject** last = Field<UObject*>(actor, "Last Parent");
            return last ? *last : nullptr;              // thrown weapons credit their last holder
        }
        Call c(actor, "GetAttachParentActor");
        actor = c.invoke() ? c.get<UObject*>("ReturnValue") : nullptr;
    }
    return nullptr;
}

std::vector<UObject*> Adapter::AllWillies() {
    std::vector<UObject*> out;
    for (UObject* w : FindInstances(kWillieClass)) if (!destroyAware_ || !BeingDestroyed(w)) out.push_back(w);
    return out;
}

int Adapter::TintArmor(UObject* willie, float r, float g, float b) {
    Call att(willie, "GetAttachedActors");
    att.set<bool>("bResetArray", true).set<bool>("bRecursivelyIncludeAttachedActors", true);
    if (!att.invoke()) return 0;
    FObjArray* arr = static_cast<FObjArray*>(att.paramPtr("OutActors"));
    UObject* st = FindObject(kArmorStruct, "UserDefinedStruct");
    if (!arr || !st || arr->num <= 0 || arr->num > 256) return 0;
    struct LinearColor { float r, g, b, a; } want{r, g, b, 1};
    FProperty* colors[] = {FindPropertyPrefix(st, "FabricColor1_"), FindPropertyPrefix(st, "FabricColor2_"), FindPropertyPrefix(st, "FabricColor3_")};
    int changed = 0;
    for (int32_t i = 0; i < arr->num; ++i) {
        UObject* armor = arr->data[i];
        if (!ClassNamed(armor, kArmorClass) && !(armor && FindProperty(ClassOf(armor), "Armor Passport"))) continue;
        uint8_t* passport = Field<uint8_t>(armor, "Armor Passport");
        UObject** meshPtr = Field<UObject*>(armor, "Armor Mesh Skeletal");
        if (!passport || !meshPtr || !*meshPtr) continue;
        bool differs = false;
        for (FProperty* c : colors) if (c && std::memcmp(passport + PropertyOffset(c), &want, sizeof want) != 0) { std::memcpy(passport + PropertyOffset(c), &want, sizeof want); differs = true; }
        if (!differs) continue;
        // Re-run the armor's own material setup with the edited passport for every dynamic material.
        Call n(*meshPtr, "GetNumMaterials"); n.invoke();
        int32_t count = n.get<int32_t>("ReturnValue");
        for (int32_t m = 0; m < count && m < 32; ++m) {
            Call gm(*meshPtr, "GetMaterial"); gm.set<int32_t>("ElementIndex", m).invoke();
            UObject* mat = gm.get<UObject*>("ReturnValue");
            if (!ClassNamed(mat, "MaterialInstanceDynamic")) continue;
            Call setup(armor, "Setup Armor Material");
            setup.set<UObject*>("AddedArmorMesh", *meshPtr).setFrom("Passport", passport).set<UObject*>("SetupArmorDynamicMaterial", mat).invoke();
        }
        ++changed;
    }
    return changed;
}

UObject* Adapter::RespawnLocalPlayer(const SpawnOptions& o) {
    UObject* old = PlayerWillie(0);
    SpawnOptions puppet = o; puppet.brain = false; puppet.stockBrain = false;
    UObject* w = SpawnFighter(puppet);
    if (!w) return nullptr;
    Call pc(statics_, "GetPlayerController");
    pc.set<UObject*>("WorldContextObject", world_).set<int32_t>("PlayerIndex", 0);
    UObject* ctrl = pc.invoke() ? pc.get<UObject*>("ReturnValue") : nullptr;
    if (!ctrl) { DestroyActor(w); return nullptr; }
    Call(ctrl, "Possess").set<UObject*>("InPawn", w).invoke();
    SetBool(w, "Player", true);
    if (old && old != w) DestroyActor(old);
    Call(ctrl, "ResetIgnoreInputFlags").invoke();   // the new body must take input whatever the death left behind
    RestoreGameInput();
    HSM_LOG("adapter: local player respawned");
    return w;
}

// Abyss_Map_Open_EA_C::Spawn_NPC builds a procedural Abyss enemy (decomp Maps/Abyss_Map_Open_EA.cpp:175).
UObject* Adapter::SpawnAbyssNpc(const FVector& at) {
    std::vector<UObject*> level = FindInstances("Abyss_Map_Open_EA_C");
    if (level.empty()) return nullptr;
    FTransform t; t.translation = at;
    Call c(level[0], "Spawn NPC");
    c.set("SpawnTransform", t);
    return c.invoke() ? c.get<UObject*>("ReturnValue") : nullptr;
}

UObject* Adapter::SpawnBot(int rank, const FVector& at, double yawDeg, int teamInt) {
    UClass* gen = FindClass("BP_Generator_Characters_Random_C");
    UClass* willie = FindClass(kWillieClass);
    if (!gen || !willie) { HSM_LOG("adapter: bot generator not loaded"); return nullptr; }
    FTransform t; t.translation = at;
    Call begin(statics_, "BeginDeferredActorSpawnFromClass");
    begin.set<UObject*>("WorldContextObject", world_).set<UClass*>("ActorClass", gen).set("SpawnTransform", t)
         .set<uint8_t>("CollisionHandlingOverride", 1).set<UObject*>("Owner", nullptr).set<uint8_t>("TransformScaleMethod", 1);
    UObject* g = begin.invoke() ? begin.get<UObject*>("ReturnValue") : nullptr;
    if (!g) return nullptr;
    if (double* budget = Field<double>(g, "Equipment Budget")) *budget = 0;
    Call fin(statics_, "FinishSpawningActor");
    fin.set<UObject*>("Actor", g).set("SpawnTransform", t).set<uint8_t>("TransformScaleMethod", 1);
    g = fin.invoke() ? fin.get<UObject*>("ReturnValue") : nullptr;
    if (!g) return nullptr;
    Call make(g, "Generate Character");
    make.set<UClass*>("Actor Class", willie).set<uint8_t>("Nationality", 0).set<uint8_t>("Tier", uint8_t(std::clamp(rank, 0, 7)))
        .set<bool>("Is Mercenary", false).set<bool>("Ignore Budget", false);
    UObject* bot = nullptr;
    if (make.invoke()) {
        SpawnOptions o; o.at = at; o.yawDeg = yawDeg; o.teamInt = teamInt; o.brain = true; o.stockBrain = true;
        o.passport = make.paramPtr("Character Passport");
        bot = o.passport ? SpawnFighter(o) : nullptr;
    }
    DestroyActor(g);
    return bot;
}

void Adapter::SuppressStockScreens(bool on, bool ownerMatch) {
    // Remove widgets suppressed since the last tick (never from inside their own construction).
    for (auto& [w, index] : g_suppressed) if (ObjectAt(index) == w) Call(w, "RemoveFromParent").invoke();
    g_suppressed.clear();
    g_suppressScreens = on;
    g_suppressOwnerScreens = on && ownerMatch;
    if (g_screenHooks) return;
    g_screenHooks = true;
    HookFunctionsMatching([](const std::string& n) { return n == "Construct"; }, [this](UObject* obj, UFunction*, void*) {
        // The main menu replays the intro video after 30 s idle (UI_Startup_Menu RetriggerableDelay -> UI_Video_Intro).
        // Skipping its Construct means no video, no sound and no input-mode change; the first-launch intro still plays.
        if (ClassNamed(obj, "UI_Video_Intro_C")) {
            bool played = false;
            if (UObject* gi = GameInstance()) GetBool(gi, "First Start Intro Played", played);
            if (!played) return false;
            g_suppressed.push_back({obj, ObjectIndex(obj)});
            return true;
        }
        if (!g_suppressScreens) return false;
        for (const char* cls : kStockScreens) if (ClassNamed(obj, cls)) {
            HSM_LOG("adapter: suppressed stock %s during a session match", cls);
            g_suppressed.push_back({obj, ObjectIndex(obj)});
            return true;                                 // skip Construct: no slow motion, no input lock, no career reset
        }
        if (g_suppressOwnerScreens)
            for (const char* cls : kOwnerMatchScreens) if (ClassNamed(obj, cls)) {
                HSM_LOG("adapter: suppressed stock %s during an owner-sync match", cls);
                g_suppressed.push_back({obj, ObjectIndex(obj)});
                return true;                             // skip Construct: no pause, no tavern / next-fight menu
            }
        return false;
    });
}

}  // namespace hsm::game
