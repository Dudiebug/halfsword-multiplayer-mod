#include "gameplay/shared_dice.h"
#include "log.h"
#include "session/hsm_dice.h"
#include "ue/engine.h"
#include "ue/hook.h"
#include "util/hsm_names.h"
#include <windows.h>
#include <cstdio>
#include <cstring>
#include <unordered_map>
#include <vector>

namespace hsm::game::shareddice {
using namespace hsm::ue;
namespace {

// FFrame of this build (docs/REVERSE_ENGINEERING.md section 9): Object 0x18, Code 0x20. A native called from bytecode has
// the caller's frame, Code at its argument expressions; one called by ProcessEvent has Code null and is left alone.
constexpr size_t kFrameObject = 0x18, kFrameCode = 0x20;
constexpr int kLogRolls = 24;   // rolls logged after each seed, so two machines' logs can be compared line by line
constexpr size_t kMaxActors = 4096;

template <class T> T Read(const void* base, size_t off) { T v; std::memcpy(&v, static_cast<const uint8_t*>(base) + off, sizeof(T)); return v; }

using NativeFn = void (*)(UObject*, void*, void*);
using StepFn = void (*)(void* stack, UObject* context, void* result);   // FFrame::Step

enum Kind { kBool, kBoolWeight, kInteger, kIntegerRange, kFloat, kFloatRange, kUnitVector, kRotator, kPointInBox, kKinds };
struct ParamSpec { const char* name; int size; };
struct Spec {
    const char* function;
    int steps;               // how many parameter expressions the stock thunk steps through
    int count;               // parameters in all, the return value included
    ParamSpec all[4];
};
// The stock thunks, read from the exe (build 24185754): parameters are stepped in declaration order, the result stored last.
constexpr Spec kSpecs[kKinds] = {
    {"RandomBool", 0, 1, {{"ReturnValue", 1}}},
    {"RandomBoolWithWeight", 1, 2, {{"Weight", 4}, {"ReturnValue", 1}}},
    {"RandomInteger", 1, 2, {{"Max", 4}, {"ReturnValue", 4}}},
    {"RandomIntegerInRange", 2, 3, {{"Min", 4}, {"Max", 4}, {"ReturnValue", 4}}},
    {"RandomFloat", 0, 1, {{"ReturnValue", 8}}},
    {"RandomFloatInRange", 2, 3, {{"Min", 8}, {"Max", 8}, {"ReturnValue", 8}}},
    {"RandomUnitVector", 0, 1, {{"ReturnValue", 24}}},
    {"RandomRotator", 1, 2, {{"bRoll", 1}, {"ReturnValue", 24}}},
    {"RandomPointInBoundingBox", 2, 3, {{"Center", 24}, {"HalfSize", 24}, {"ReturnValue", 24}}},
};

NativeFn g_orig[kKinds] = {};
StepFn g_step = nullptr;
bool g_installed = false;

bool g_active = false;
uint64_t g_seed = 0;
// One entry per actor seen since the seed was set: whether it rolls shared, its identity, and how often it has rolled.
struct Actor { FName name; bool shared = false; uint64_t key = 0; uint64_t rolls = 0; };
std::unordered_map<const UObject*, Actor> g_actors;   // node-based: an Actor stays put while a nested call adds others
uint64_t g_answered = 0, g_passed = 0;
int g_logged = 0;

bool IsFighter(const UObject* o) {
    for (const UClass* c = ClassOf(o); c; c = SuperOf(c)) if (ObjectName(reinterpret_cast<const UObject*>(c)) == "Willie_BP_C") return true;
    return false;
}

// A placed actor of a level, not a fighter. Its name is part of the map package, so it is the same on every machine.
Actor* ActorOf(const UObject* obj) {
    static Actor none;
    const FName name = ObjectFName(obj);
    auto it = g_actors.find(obj);
    if (it != g_actors.end() && it->second.name.index == name.index && it->second.name.number == name.number) return &it->second;
    if (it == g_actors.end() && g_actors.size() >= kMaxActors) return &none;
    Actor a; a.name = name;
    const UObject* level = OuterOf(obj);
    const UClass* lc = ClassOf(level);
    if (!IsDefaultObject(obj) && !IsGarbage(obj) && WasLoaded(obj) && lc && ObjectName(reinterpret_cast<const UObject*>(lc)) == "Level" && !IsFighter(obj)) {
        a.shared = true;
        a.key = dice::IdentityKey(ObjectName(OuterOf(level)) + "/" + ObjectName(level) + "/" + ObjectName(obj));
    }
    return &(g_actors[obj] = a);
}

// The actor whose Blueprint is making this call, if its roll is shared; null = run the stock thunk.
Actor* Caller(void* stack, UObject** objOut) {
    if (!g_active || !Read<void*>(stack, kFrameCode)) return nullptr;
    if (GetCurrentThreadId() != GameThreadId()) return nullptr;
    UObject* obj = Read<UObject*>(stack, kFrameObject);
    if (!obj) return nullptr;
    Actor* a = ActorOf(obj);
    if (!a->shared) { ++g_passed; return nullptr; }
    *objOut = obj;
    return a;
}

// The tail of every stock thunk: step past EX_EndFunctionParms when running bytecode.
void Finish(void* stack) {
    uint8_t* code = Read<uint8_t*>(stack, kFrameCode);
    if (code) { code += 1; std::memcpy(static_cast<uint8_t*>(stack) + kFrameCode, &code, sizeof code); }
}
template <class T> void Eval(void* stack, UObject* obj, T& out) { g_step(stack, obj, &out); }

void LogRoll(Kind k, const UObject* obj, uint64_t n, const char* value) {
    if (g_logged >= kLogRolls) return;
    ++g_logged;
    HSM_LOG("dice: %s #%llu %s -> %s", ObjectName(obj).c_str(), (unsigned long long)n, kSpecs[k].function, value);
}

template <Kind K> void Thunk(UObject* ctx, void* stack, void* result) {
    UObject* obj = nullptr;
    Actor* a = Caller(stack, &obj);
    if (!a) { g_orig[K](ctx, stack, result); return; }
    // Parameters first: an expression may roll too (its own call is counted before this one, the same on every machine).
    int32_t i1 = 0, i2 = 0; double d1 = 0, d2 = 0; float w = 0; uint32_t flag = 0; dice::Vec3 v1, v2;
    switch (K) {
        case kBoolWeight: Eval(stack, obj, w); break;
        case kInteger: Eval(stack, obj, i1); break;
        case kIntegerRange: Eval(stack, obj, i1); Eval(stack, obj, i2); break;
        case kFloatRange: Eval(stack, obj, d1); Eval(stack, obj, d2); break;
        case kRotator: Eval(stack, obj, flag); break;
        case kPointInBox: Eval(stack, obj, v1); Eval(stack, obj, v2); break;
        default: break;
    }
    Finish(stack);
    const uint64_t n = a->rolls++;
    ++g_answered;
    auto u = [&](uint32_t sub) { return dice::Unit(dice::Draw(g_seed, a->key, n, sub)); };
    char text[96] = {};
    switch (K) {
        case kBool: { const bool r = dice::RandomBool(u(0)); *static_cast<uint8_t*>(result) = r; std::snprintf(text, sizeof text, "%d", r); break; }
        case kBoolWeight: { const bool r = dice::RandomBoolWithWeight(u(0), w); *static_cast<uint8_t*>(result) = r; std::snprintf(text, sizeof text, "%d (weight %g)", r, double(w)); break; }
        case kInteger: { const int32_t r = dice::RandomInteger(u(0), i1); *static_cast<int32_t*>(result) = r; std::snprintf(text, sizeof text, "%d (max %d)", r, i1); break; }
        case kIntegerRange: { const int32_t r = dice::RandomIntegerInRange(u(0), i1, i2); *static_cast<int32_t*>(result) = r; std::snprintf(text, sizeof text, "%d (%d..%d)", r, i1, i2); break; }
        case kFloat: { const double r = u(0); *static_cast<double*>(result) = r; std::snprintf(text, sizeof text, "%.6f", r); break; }
        case kFloatRange: { const double r = dice::RandomFloatInRange(u(0), d1, d2); *static_cast<double*>(result) = r; std::snprintf(text, sizeof text, "%.6f (%g..%g)", r, d1, d2); break; }
        case kUnitVector: { const dice::Vec3 r = dice::RandomUnitVector(u(0), u(1)); std::memcpy(result, &r, sizeof r); std::snprintf(text, sizeof text, "%.4f %.4f %.4f", r.x, r.y, r.z); break; }
        case kRotator: { const dice::Vec3 r = dice::RandomRotator(u(0), u(1), u(2), (flag & 0xFF) != 0); std::memcpy(result, &r, sizeof r); std::snprintf(text, sizeof text, "%.2f %.2f %.2f", r.x, r.y, r.z); break; }
        case kPointInBox: { const dice::Vec3 r = dice::RandomPointInBox(u(0), u(1), u(2), v1, v2); std::memcpy(result, &r, sizeof r); std::snprintf(text, sizeof text, "%.1f %.1f %.1f", r.x, r.y, r.z); break; }
        default: break;
    }
    LogRoll(K, obj, n, text);
}

constexpr NativeFn kThunks[kKinds] = {
    &Thunk<kBool>, &Thunk<kBoolWeight>, &Thunk<kInteger>, &Thunk<kIntegerRange>, &Thunk<kFloat>,
    &Thunk<kFloatRange>, &Thunk<kUnitVector>, &Thunk<kRotator>, &Thunk<kPointInBox>,
};

// The function is the stock one: a native thunk (not a Blueprint body, not another mod's jump), with the parameters we
// will read in the sizes the thunk was disassembled with, and one that steps through FFrame::Step.
bool Plausible(Kind k, UFunction* fn) {
    const Spec& s = kSpecs[k];
    if (!fn) { HSM_LOG("dice: %s not found; left as the game has it", s.function); return false; }
    const auto* thunk = static_cast<const uint8_t*>(FunctionNative(fn));
    if (!thunk || IsScriptFunction(fn)) { HSM_LOG("dice: %s is not a native function; left alone", s.function); return false; }
    const std::vector<FProperty*> params = Params(fn);
    bool shape = int(params.size()) == s.count;
    for (int i = 0; shape && i < s.count; ++i) {
        bool found = false;
        for (FProperty* p : params)
            if (SameNameAscii(PropertyName(p), s.all[i].name) && PropertyArrayDim(p) == 1 && PropertySize(p) == s.all[i].size) found = true;
        shape = found;
    }
    if (!shape) { HSM_LOG("dice: %s has other parameters than expected; left alone", s.function); return false; }
    if (s.steps > 0) {   // a stock thunk with parameters calls FFrame::Step for each one
        bool steps = false;
        for (size_t i = 0; i + 5 <= 0x180 && !steps; ++i)
            steps = thunk[i] == 0xE8 && thunk + i + 5 + Read<int32_t>(thunk, i + 1) == reinterpret_cast<const uint8_t*>(g_step);
        if (!steps) { HSM_LOG("dice: %s: the thunk does not step through FFrame::Step (another hook on it?); left alone", s.function); return false; }
    }
    return true;
}
}  // namespace

bool Install() {
    if (g_installed) return true;
    void* step = FrameStepAddress();
    UObject* math = FindObject("KismetMathLibrary", "Class");
    if (!step || !math) { HSM_LOG("dice: shared dice unavailable (%s)", step ? "KismetMathLibrary not loaded" : "FFrame::Step not verified"); return false; }
    g_step = reinterpret_cast<StepFn>(step);
    UClass* cls = reinterpret_cast<UClass*>(math);
    UFunction* found[kKinds] = {
        FindFunction(cls, "RandomBool"), FindFunction(cls, "RandomBoolWithWeight"), FindFunction(cls, "RandomInteger"),
        FindFunction(cls, "RandomIntegerInRange"), FindFunction(cls, "RandomFloat"), FindFunction(cls, "RandomFloatInRange"),
        FindFunction(cls, "RandomUnitVector"), FindFunction(cls, "RandomRotator"), FindFunction(cls, "RandomPointInBoundingBox"),
    };
    int swapped = 0;
    for (int k = 0; k < kKinds; ++k) {
        if (!Plausible(Kind(k), found[k])) continue;
        g_orig[k] = reinterpret_cast<NativeFn>(FunctionNative(found[k]));
        SetFunctionNative(found[k], reinterpret_cast<void*>(kThunks[k]));
        ++swapped;
    }
    g_installed = swapped > 0;
    HSM_LOG("dice: shared dice installed on %d of %d random functions", swapped, int(kKinds));
    return g_installed;
}

void SetSeed(uint64_t seed) {
    if (g_active && seed == g_seed) return;
    g_seed = seed; g_active = true;
    g_actors.clear(); g_answered = g_passed = 0; g_logged = 0;
    HSM_LOG("dice: level seed %016llx%s", (unsigned long long)seed, g_installed ? "" : " (not installed: the game keeps its own randomness)");
}

void Clear() {
    if (!g_active) return;
    g_active = false;
    g_actors.clear();
    HSM_LOG("dice: session over; the game's own randomness again (%llu rolls answered)", (unsigned long long)g_answered);
}

bool Active() { return g_active; }
uint64_t Seed() { return g_seed; }

std::string HealthLine() {
    if (!g_active || !g_installed) return {};
    char b[160];
    std::snprintf(b, sizeof b, "dice: seed %016llx, %llu rolls on %zu actors, %llu passed",
                  (unsigned long long)g_seed, (unsigned long long)g_answered, g_actors.size(), (unsigned long long)g_passed);
    return b;
}

}  // namespace hsm::game::shareddice
