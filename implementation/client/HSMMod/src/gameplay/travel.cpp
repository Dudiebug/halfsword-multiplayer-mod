#include "gameplay/travel.h"
#include "gameplay/crypts_layout.h"
#include "gameplay/world_context.h"
#include "log.h"
#include "session/hsm_travel_gate.h"
#include "ue/hook.h"
#include <cstring>

namespace hsm::game {
using namespace hsm::ue;
namespace {

// UGameplayStatics::OpenLevel(const UObject*, FName, bool, FString) for builds 24185754 and 26568321 (same RVA); both Blueprint thunks
// (OpenLevel, OpenLevelBySoftObjectPtr) end here. The FString is passed by hidden pointer and destroyed by the callee.
constexpr uintptr_t kOpenLevelRva = 0x35E3590;
constexpr uint8_t kOpenLevelPrologue[] = {0x48, 0x89, 0x5C, 0x24, 0x20, 0x48, 0x89, 0x54, 0x24, 0x10, 0x55, 0x56, 0x41, 0x56, 0x48, 0x8D};
using OpenLevelFn = void (*)(UObject*, FName, bool, void*);
OpenLevelFn g_original = nullptr;
OpenLevelFilter g_filter;
bool g_bypass = false;

void Detour(UObject* ctx, FName level, bool absolute, void* options) {
    if (!g_bypass) {
        const crypts::Layout* armed = cryptslayout::Armed();
        const bool coop = cryptslayout::CoopArmed();
        cryptslayout::Arm(nullptr);   // the game's own level change: never a shared Crypts map (the filter may arm one)
        std::string name = NameToString(level), opts = ReadFString(options);
        if (g_filter && !g_filter(name, absolute, opts)) {
            cryptslayout::Arm(armed);   // the level stays
            cryptslayout::ArmCoop(coop);
            HSM_LOG("travel: blocked OpenLevel(%s)", name.c_str());
            // Swallowed: the callee would have destroyed the by-value FString. It is owned by the caller's
            // temporary here, which leaks only its small buffer; acceptable for a blocked menu action.
            return;
        }
    }
    g_original(ctx, level, absolute, options);
}

// Fields copied host -> client before travel. Only plain values (bool/byte/enum/int/float/double) are sent.
constexpr const char* kSettings[] = {
    "Free Mode Activated", "Current Game Mode enum", "Current Play Mode", "Current Combat Mode", "Free Mode Foes Amount",
    "Free Mode Tier", "Free Mode Carnage", "Free Mode Brawling", "Free Mode Opponents Height", "Free Mode Opponents Weight",
    "Free Mode Opponents Weapons Type", "Free Mode Rendom Equipment Tier", "Free Mode Blossfechten", "Sparring Partner ID",
    "Progression Multiplayer", "FreeMode Multiplayer",
    // The level scripts read these too (audit item 39): Lose Condition 3 spawns BP_Fence_Bags in Pit, Yard and Cellar;
    // Day Time (Progression) and Current FreeMode Time (Free Mode) pick the lighting sublevel and ambience.
    "Current Lose Condition", "Day Time", "Current FreeMode Time",
    // rev 40: Progression co-op keeps one calendar, the host's (session/hsm_career.h): every client's career takes its day too.
    "Day Count",
};
bool PlainType(const std::string& t) {
    return t == "BoolProperty" || t == "ByteProperty" || t == "EnumProperty" || t == "IntProperty" || t == "FloatProperty" || t == "DoubleProperty";
}
UObject* GameInstance() { auto v = FindInstances("GI_Settings_C"); return v.empty() ? nullptr : v[0]; }

// kOpenLevelRva while the exe's bytes there are OpenLevel's; on an exe that moved it, the one call to a function with
// OpenLevel's prologue in the Blueprint thunk GameplayStatics.OpenLevel (execOpenLevel calls it directly).
void* OpenLevelAddress() {
    if (void* p = CheckedFunction(kOpenLevelRva, kOpenLevelPrologue, sizeof kOpenLevelPrologue)) return p;
    UObject* statics = FindObject("GameplayStatics", "Class");
    UFunction* thunk = statics ? FindFunction(reinterpret_cast<UClass*>(statics), "OpenLevel") : nullptr;
    void* p = CalleeOfNative(thunk, kOpenLevelPrologue, sizeof kOpenLevelPrologue);
    if (p) HSM_LOG("travel: UGameplayStatics::OpenLevel is at exe+0x%zx in this exe (found through GameplayStatics.OpenLevel)",
                   reinterpret_cast<uintptr_t>(p) - ModuleBase());
    return p;
}
}  // namespace

bool InstallTravelHook(OpenLevelFilter filter) {
    g_filter = std::move(filter);
    void* target = OpenLevelAddress();
    if (!target) {
        HSM_LOG("travel: UGameplayStatics::OpenLevel not found in this exe; the game's own level changes are not filtered");
        return false;
    }
    return HookNative(target, reinterpret_cast<void*>(&Detour), reinterpret_cast<void**>(&g_original), "UGameplayStatics::OpenLevel");
}

void OpenLevel(UObject* ctx, const std::string& level, bool absolute, const std::string& options, std::source_location at) {
    // Every caller hands its cached game mode (Session::lastGameMode_): after a level change it may be the old level's,
    // freed (public #855). The live mode takes its place; with none alive the level is not opened.
    ctx = worldctx::For(ctx, at);
    if (!ctx) { HSM_LOG("travel: OpenLevel(%s) not sent: no live world to open it from", level.c_str()); return; }
    static UObject* statics = nullptr;
    if (!statics) if (UObject* c = FindObject("GameplayStatics", "Class")) statics = DefaultObject(reinterpret_cast<UClass*>(c));
    FStringArgHolder opts(options);
    cryptslayout::Arm(nullptr);   // any level opened here is the game's own; a loader layout is armed by whoever opens it, after this
    Call c(statics, "OpenLevel");
    c.set<UObject*>("WorldContextObject", ctx).setName("LevelName", MakeName(level)).set<bool>("bAbsolute", absolute).set("Options", opts.arg);
    g_bypass = true;
    c.invoke();
    g_bypass = false;
}

std::vector<Setting> CaptureGameSettings() {
    std::vector<Setting> out;
    UObject* gi = GameInstance();
    if (!gi) return out;
    for (const char* name : kSettings) {
        FProperty* p = FindProperty(ClassOf(gi), name);
        if (!p || !PlainType(PropertyType(p))) continue;
        Setting s; s.name = name;
        if (PropertyType(p) == "BoolProperty") { bool b = false; GetBool(gi, name, b); s.value = {uint8_t(b)}; }
        else {
            int32_t size = PropertySize(p);
            if (size <= 0 || size_t(size) > kMaxSettingBytes) continue;
            const uint8_t* src = reinterpret_cast<const uint8_t*>(gi) + PropertyOffset(p);
            s.value.assign(src, src + size);
        }
        out.push_back(std::move(s));
    }
    return out;
}

size_t ApplyGameSettings(const std::vector<Setting>& settings) {
    UObject* gi = GameInstance();
    if (!gi) return 0;
    size_t applied = 0;
    for (const Setting& s : settings) {
        bool allowed = false;
        for (const char* n : kSettings) if (s.name == n) allowed = true;
        if (!allowed) continue;                                // only the whitelist, never arbitrary fields
        FProperty* p = FindProperty(ClassOf(gi), s.name);
        if (!p || !PlainType(PropertyType(p))) continue;
        const std::string type = PropertyType(p);
        if (type == "BoolProperty") { if (SettingValueOk(SettingKind::Bool, s.value.data(), s.value.size()) && SetBool(gi, s.name, s.value[0] != 0)) ++applied; continue; }
        if (int32_t(s.value.size()) != PropertySize(p)) continue;
        const SettingKind kind = type == "FloatProperty" ? SettingKind::Float : type == "DoubleProperty" ? SettingKind::Double
                               : type == "IntProperty" || s.value.size() != 1 ? SettingKind::Int : SettingKind::Byte;   // L8: no NaN/Inf, no wild enum bytes
        if (!SettingValueOk(kind, s.value.data(), s.value.size())) continue;
        std::memcpy(reinterpret_cast<uint8_t*>(gi) + PropertyOffset(p), s.value.data(), s.value.size());
        ++applied;
    }
    return applied;
}

std::string ChangedGameSettings(const std::vector<Setting>& want) {
    const std::vector<Setting> now = CaptureGameSettings();
    std::string out;
    for (const Setting& w : want)
        for (const Setting& s : now)
            if (s.name == w.name && s.value != w.value) out += (out.empty() ? "" : ", ") + s.name;
    return out;
}

}  // namespace hsm::game
