#include "ue/engine.h"
#include "log.h"
#include "util/hsm_names.h"
#include "hsm_chat.h"
#include <windows.h>
#include <cstring>
#include <mutex>
#include <unordered_map>

namespace hsm::ue {
namespace {

// ---- per-build constants (RVA), verified against the shipping exe (see docs/REVERSE_ENGINEERING.md)
struct ScriptVm {
    uintptr_t processInternal;     // UObject::ProcessInternal, Func of every script UFunction (UFunction::Bind)
    uintptr_t executor;            // ProcessLocalScriptFunction(UObject*, FFrame&, RESULT_DECL)
    uint32_t executorCallSite;     // offset of ProcessInternal's E8 call to the executor
    uintptr_t execAddIntInt;       // UKismetMathLibrary::execAdd_IntInt, checks UFunction::Func's offset
};
struct BuildInfo { uint64_t key; const char* label; uintptr_t gobjects, appendString; uint32_t processEventIndex; ScriptVm vm; };
// key = PE TimeDateStamp << 32 | SizeOfImage. Unknown key => mod disables itself.
constexpr BuildInfo kBuilds[] = {
    {0x8EFA122909712000ull, "steam-24185754 (UE 5.4.4 CL-2705)", 0x08BFD240, 0x012C1CB0, 0x4D,
     {0x014A4850, 0x014A4950, 0x6C, 0x036AF990}},
};
// Prologues of the two VM functions in the builds above (MSVC, /GS): checked before the executor is detoured.
constexpr uint8_t kExecutorPrologue[] = {0x48, 0x89, 0x5C, 0x24, 0x08, 0x55, 0x56, 0x57, 0x48, 0x83, 0xEC, 0x70};
constexpr uint8_t kProcessInternalPrologue[] = {0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x6C, 0x24, 0x10};
// ---- UE 5.4 core layouts for this build
constexpr size_t kObjFlags = 0x08, kObjIndex = 0x0C, kObjClass = 0x10, kObjName = 0x18, kObjOuter = 0x20;
constexpr size_t kFieldNext = 0x28;
constexpr size_t kStructSuper = 0x40, kStructChildren = 0x48, kStructChildProps = 0x50, kStructSize = 0x58;
constexpr size_t kFuncFlags = 0xB0, kFuncNative = 0xD8;
constexpr size_t kClassCDO = 0x110;
constexpr size_t kFFieldClass = 0x08, kFFieldNext = 0x18, kFFieldName = 0x20;
constexpr size_t kPropArrayDim = 0x30, kPropElementSize = 0x34, kPropFlags = 0x38, kPropOffset = 0x44;
constexpr size_t kBoolByteOffset = 0x71, kBoolByteMask = 0x72;
constexpr size_t kPropVtCopyValues = 0xF0;   // FProperty::CopyValuesInternal(Dest, Src, Count)
constexpr size_t kPropVtClearValue = 0x130;   // FProperty::ClearValueInternal(Data), as ProcessLocalScriptFunction's abort path calls it
constexpr uint32_t kRF_ClassDefaultObject = 0x10;
constexpr uint32_t kRF_MirroredGarbage = 0x40000000;   // UE5: set by MarkAsGarbage (an actor's destroy) until GC frees it
constexpr uint64_t kCPF_Parm = 0x80, kCPF_OutParm = 0x100, kCPF_ZeroConstructor = 0x200, kCPF_ReturnParm = 0x400,
                   kCPF_NoDestructor = 0x1000000000ull;
constexpr int32_t kItemsPerChunk = 0x10000, kItemSize = 0x18;

struct TUObjectArray { uint8_t** objects; uint8_t* pre; int32_t maxElements, numElements, maxChunks, numChunks; };
struct FString { wchar_t* data; int32_t num, max; };

const BuildInfo* g_build = nullptr;
uintptr_t g_base = 0;
TUObjectArray* g_objects = nullptr;
using AppendStringFn = void (*)(const FName*, FString*);
AppendStringFn g_appendString = nullptr;
uint64_t g_key = 0;

std::mutex g_nameMu;
std::unordered_map<uint64_t, std::string> g_nameCache;

// FindFunction / FindProperty answers per struct (engine.h). The slot and name tell a reused address apart.
struct LookupBucket {
    int32_t index = -1;
    FName name{};
    std::unordered_map<std::string, void*> answers;   // nullptr: looked up, not there
};
std::mutex g_lookupMu;
std::unordered_map<const void*, LookupBucket> g_functionCache, g_propertyCache;
uint64_t g_lookupHits = 0, g_lookupMisses = 0, g_lookupDropped = 0;

template <class T> T Read(const void* base, size_t off) { T v; std::memcpy(&v, static_cast<const uint8_t*>(base) + off, sizeof(T)); return v; }

uint64_t ComputeBuildKey() {
    auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(g_base);
    auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(g_base + dos->e_lfanew);
    return uint64_t(nt->FileHeader.TimeDateStamp) << 32 | nt->OptionalHeader.SizeOfImage;
}

bool IsName(const UObject* o, const char* n) { return o && SameNameAscii(ObjectName(o), n); }
std::string ClassName(const UObject* o) { UClass* c = ClassOf(o); return c ? ObjectName(reinterpret_cast<UObject*>(c)) : std::string(); }
}  // namespace

uint64_t BuildKey() { return g_key; }

bool Init() {
    g_base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    g_key = ComputeBuildKey();
    for (const BuildInfo& b : kBuilds) if (b.key == g_key) { g_build = &b; break; }
    if (!g_build) { HSM_LOG("engine: unsupported build key %016llx", (unsigned long long)g_key); return false; }
    g_objects = reinterpret_cast<TUObjectArray*>(g_base + g_build->gobjects);
    g_appendString = reinterpret_cast<AppendStringFn>(g_base + g_build->appendString);
    // Wait for the object array to fill (engine init) before anyone touches it.
    for (int i = 0; i < 600 && (g_objects->numElements < 10000 || !g_objects->objects); ++i) Sleep(100);
    if (g_objects->numElements < 10000 || g_objects->numElements > g_objects->maxElements) {
        HSM_LOG("engine: GObjects looks wrong (num=%d max=%d)", g_objects->numElements, g_objects->maxElements); return false;
    }
    UObject* first = ObjectAt(0);
    std::string n = first ? FullName(first) : "<null>";
    HSM_LOG("engine: %s, %d objects, object[0]=%s", g_build->label, g_objects->numElements, n.c_str());
    return first && n.find("CoreUObject") != std::string::npos;
}

void* ProcessEventAddress() {
    UObject* o = ObjectAt(0);
    if (!o) return nullptr;
    void** vtable = Read<void**>(o, 0);
    return vtable[g_build->processEventIndex];
}

// ---------------------------------------------------------------- names / objects
std::string NameToString(FName n) {
    uint64_t key = uint64_t(uint32_t(n.index)) << 32 | uint32_t(n.number);
    {
        std::lock_guard<std::mutex> lock(g_nameMu);
        auto it = g_nameCache.find(key);
        if (it != g_nameCache.end()) return it->second;
    }
    // AppendString writes into our FString; the buffer is large enough that the engine never reallocates it.
    wchar_t buf[1024]; FString s{buf, 0, 1024};
    g_appendString(&n, &s);
    std::string out;
    for (int i = 0; i < s.num && buf[i]; ++i) out.push_back(buf[i] < 128 ? char(buf[i]) : '?');
    std::lock_guard<std::mutex> lock(g_nameMu);
    g_nameCache.emplace(key, out);
    return out;
}

FName ObjectFName(const UObject* o) { return o ? Read<FName>(o, kObjName) : FName{}; }
std::string ObjectName(const UObject* o) { return o ? NameToString(Read<FName>(o, kObjName)) : std::string(); }
UClass* ClassOf(const UObject* o) { return o ? Read<UClass*>(o, kObjClass) : nullptr; }
UObject* OuterOf(const UObject* o) { return o ? Read<UObject*>(o, kObjOuter) : nullptr; }
bool IsDefaultObject(const UObject* o) { return o && (Read<uint32_t>(o, kObjFlags) & kRF_ClassDefaultObject); }
bool IsGarbage(const UObject* o) { return o && (Read<uint32_t>(o, kObjFlags) & kRF_MirroredGarbage); }

std::string FullName(const UObject* o) {
    if (!o) return "None";
    std::string path = ObjectName(o);
    for (UObject* outer = OuterOf(o); outer; outer = OuterOf(outer)) path = ObjectName(outer) + "." + path;
    return ClassName(o) + " " + path;
}

bool IsA(const UObject* o, const UClass* c) { return IsChildOf(ClassOf(o), c); }

bool IsChildOf(const UClass* c, const UClass* base) {
    for (const void* s = c; s; s = Read<void*>(s, kStructSuper)) if (s == base) return true;
    return false;
}
UClass* SuperOf(const UClass* c) { return c ? Read<UClass*>(c, kStructSuper) : nullptr; }

int32_t ObjectIndex(const UObject* o) { return o ? Read<int32_t>(o, kObjIndex) : -1; }
int32_t ObjectCount() { return g_objects ? g_objects->numElements : 0; }

UObject* ObjectAt(int32_t index) {
    if (!g_objects || index < 0 || index >= g_objects->numElements) return nullptr;
    uint8_t* chunk = g_objects->objects[index / kItemsPerChunk];
    if (!chunk) return nullptr;
    return *reinterpret_cast<UObject**>(chunk + size_t(index % kItemsPerChunk) * kItemSize);
}

// Once a name has matched, its FName is remembered and later scans compare only that (a shipping build keeps one name
// entry per case-insensitive string, trailing number split off, so no other FName reads as the same text). Before,
// every scan built each of about 80k object names as a string, and FindClass of a blueprint class scans twice. An object
// once found is checked at its object array slot (same object, name and class) before any scan: the callers look up types
// (classes, structs), several of them on every spawn, removal or loadout.
UObject* FindObject(const std::string& name, const char* className) {
    static std::mutex mu;
    static std::unordered_map<std::string, uint64_t> known;   // name -> its FName key
    struct Found { UObject* o; int32_t index; FName name; UClass* cls; };
    static std::unordered_map<std::string, Found> found;      // name and class name -> the object last returned
    const std::string where = name + '\0' + (className ? className : "");
    uint64_t want = 0;
    bool haveKey = false;
    {
        std::lock_guard<std::mutex> lock(mu);
        if (auto it = found.find(where); it != found.end()) {
            const Found& f = it->second;
            if (ObjectAt(f.index) == f.o) {
                const FName now = Read<FName>(f.o, kObjName);
                if (now.index == f.name.index && now.number == f.name.number && ClassOf(f.o) == f.cls) return f.o;
            }
            found.erase(it);
        }
        if (auto it = known.find(name); it != known.end()) { want = it->second; haveKey = true; }
    }
    for (int32_t i = 0, n = ObjectCount(); i < n; ++i) {
        UObject* o = ObjectAt(i);
        if (!o) continue;
        const FName on = Read<FName>(o, kObjName);
        const uint64_t key = uint64_t(uint32_t(on.index)) << 32 | uint32_t(on.number);
        if (haveKey ? key != want : !SameNameAscii(NameToString(on), name)) continue;
        if (!haveKey) {
            std::lock_guard<std::mutex> lock(mu);
            known.emplace(name, key);
            want = key; haveKey = true;
        }
        if (className && !SameNameAscii(ClassName(o), className)) continue;
        std::lock_guard<std::mutex> lock(mu);
        found[where] = {o, i, on, ClassOf(o)};
        return o;
    }
    return nullptr;
}

// Per object only its class's FName is read; whether a name matches is decided once per name and remembered (a name's
// text never changes: the name pool only grows). Building each object's class name as a string took 7-9 ms per scan
// of about 80k objects (logs 8-9: the host's once-a-second stock-fighter scan, the 5 s census).
std::vector<UObject*> FindInstances(const char* className) {
    static std::mutex mu;
    static std::unordered_map<std::string, std::unordered_map<uint64_t, bool>> matches;   // className -> class FName -> match
    std::vector<UObject*> out;
    std::lock_guard<std::mutex> lock(mu);
    auto& seen = matches[className];
    UClass* lastClass = nullptr;
    bool lastMatch = false;
    for (int32_t i = 0, n = ObjectCount(); i < n; ++i) {
        UObject* o = ObjectAt(i);
        if (!o || IsDefaultObject(o)) continue;
        UClass* c = ClassOf(o);
        if (!c) continue;
        if (c != lastClass) {
            const FName name = Read<FName>(c, kObjName);
            const uint64_t key = uint64_t(uint32_t(name.index)) << 32 | uint32_t(name.number);
            auto it = seen.find(key);
            if (it == seen.end()) it = seen.emplace(key, SameNameAscii(NameToString(name), className)).first;
            lastClass = c;
            lastMatch = it->second;
        }
        if (lastMatch) out.push_back(o);
    }
    return out;
}

UObject* DefaultObject(const UClass* c) { return c ? Read<UObject*>(c, kClassCDO) : nullptr; }

// ---------------------------------------------------------------- reflection
namespace {
bool SameFName(FName a, FName b) { return a.index == b.index && a.number == b.number; }

// The struct is still the one the bucket was filled for: same slot, same name, and the object array holds it there.
bool BucketCurrent(LookupBucket& b, const UObject* s, int32_t index, FName name) {
    if (b.index == index && SameFName(b.name, name) && ObjectAt(index) == s) return true;
    if (!b.answers.empty()) ++g_lookupDropped;
    b.answers.clear(); b.index = index; b.name = name;
    return false;
}

template <class Walk>
void* CachedLookup(std::unordered_map<const void*, LookupBucket>& cache, const void* ustruct, const std::string& name, Walk walk) {
    if (!ustruct) return nullptr;
    const UObject* s = static_cast<const UObject*>(ustruct);
    const int32_t index = ObjectIndex(s);
    const FName sname = ObjectFName(s);
    {
        std::lock_guard<std::mutex> lock(g_lookupMu);
        auto b = cache.find(ustruct);
        if (b != cache.end() && BucketCurrent(b->second, s, index, sname)) {
            auto a = b->second.answers.find(name);
            if (a != b->second.answers.end()) { ++g_lookupHits; return a->second; }
        }
    }
    void* found = walk();                                // unlocked: the walk reads names (their own lock)
    std::lock_guard<std::mutex> lock(g_lookupMu);
    ++g_lookupMisses;
    if (ObjectAt(index) != s) return found;              // not in the object array: answered, not remembered
    LookupBucket& b = cache[ustruct];
    BucketCurrent(b, s, index, sname);
    b.answers.emplace(name, found);
    return found;
}
}  // namespace

void FlushReflectionCache() {
    std::lock_guard<std::mutex> lock(g_lookupMu);
    g_lookupDropped += g_functionCache.size() + g_propertyCache.size();
    g_functionCache.clear(); g_propertyCache.clear();
}

ReflectionCacheStats GetReflectionCacheStats() {
    std::lock_guard<std::mutex> lock(g_lookupMu);
    ReflectionCacheStats st;
    st.hits = g_lookupHits; st.misses = g_lookupMisses; st.dropped = g_lookupDropped;
    st.structs = g_functionCache.size() + g_propertyCache.size();
    return st;
}

UFunction* FindFunction(const UClass* c, const std::string& name) {
    return static_cast<UFunction*>(CachedLookup(g_functionCache, c, name, [&]() -> void* {
        for (const void* s = c; s; s = Read<void*>(s, kStructSuper))
            for (UObject* f = Read<UObject*>(s, kStructChildren); f; f = Read<UObject*>(f, kFieldNext))
                if (SameNameAscii(ObjectName(f), name)) return f;
        return nullptr;
    }));
}

std::vector<UFunction*> FunctionsWithPrefix(const UClass* c, const std::string& prefix) {
    std::vector<UFunction*> out;
    for (UObject* f = Read<UObject*>(c, kStructChildren); f; f = Read<UObject*>(f, kFieldNext))
        if (StartsWithNameAscii(ObjectName(f), prefix)) out.push_back(reinterpret_cast<UFunction*>(f));
    return out;
}

FProperty* FindProperty(const void* ustruct, const std::string& name) {
    return static_cast<FProperty*>(CachedLookup(g_propertyCache, ustruct, name, [&]() -> void* {
        for (const void* s = ustruct; s; s = Read<void*>(s, kStructSuper))
            for (const void* p = Read<void*>(s, kStructChildProps); p; p = Read<void*>(p, kFFieldNext))
                if (SameNameAscii(NameToString(Read<FName>(p, kFFieldName)), name)) return const_cast<void*>(p);
        return nullptr;
    }));
}

FProperty* FindPropertyPrefix(const void* ustruct, const std::string& prefix) {
    for (const void* s = ustruct; s; s = Read<void*>(s, kStructSuper))
        for (const void* p = Read<void*>(s, kStructChildProps); p; p = Read<void*>(p, kFFieldNext))
            if (StartsWithNameAscii(NameToString(Read<FName>(p, kFFieldName)), prefix)) return static_cast<FProperty*>(const_cast<void*>(p));
    return nullptr;
}

std::vector<FProperty*> StructProperties(const void* ustruct) {
    std::vector<FProperty*> out;
    if (ustruct)
        for (const void* p = Read<void*>(ustruct, kStructChildProps); p; p = Read<void*>(p, kFFieldNext)) out.push_back(static_cast<FProperty*>(const_cast<void*>(p)));
    return out;
}

UObject* FindStruct(const std::string& name) { return FindObject(name, "ScriptStruct"); }
FProperty* StructMember(const UObject* scriptStruct, const std::string& member) { return scriptStruct ? FindProperty(scriptStruct, member) : nullptr; }

int32_t PropertyOffset(const FProperty* p) { return Read<int32_t>(p, kPropOffset); }
int32_t PropertySize(const FProperty* p) { return Read<int32_t>(p, kPropElementSize); }
int32_t PropertyArrayDim(const FProperty* p) { return Read<int32_t>(p, kPropArrayDim); }
uint64_t PropertyFlags(const FProperty* p) { return Read<uint64_t>(p, kPropFlags); }
std::string PropertyName(const FProperty* p) { return NameToString(Read<FName>(p, kFFieldName)); }
std::string PropertyType(const FProperty* p) { const void* fc = Read<void*>(p, kFFieldClass); return fc ? NameToString(Read<FName>(fc, 0)) : std::string(); }
int32_t ParamsSize(const UFunction* f) { return Read<int32_t>(f, kStructSize); }
uint32_t FunctionFlags(const UFunction* f) { return Read<uint32_t>(f, kFuncFlags); }

void* FunctionNative(const UFunction* f) { return f ? Read<void*>(f, kFuncNative) : nullptr; }
bool IsScriptFunction(const UFunction* f) { return g_build && FunctionNative(f) == reinterpret_cast<void*>(g_base + g_build->vm.processInternal); }

void* ScriptExecutorAddress() {
    if (!g_build) return nullptr;
    const ScriptVm& vm = g_build->vm;
    auto* pi = reinterpret_cast<const uint8_t*>(g_base + vm.processInternal);
    auto* exec = reinterpret_cast<const uint8_t*>(g_base + vm.executor);
    UObject* math = FindObject("KismetMathLibrary", "Class");
    UFunction* add = math ? FindFunction(reinterpret_cast<UClass*>(math), "Add_IntInt") : nullptr;
    bool funcOk = add && FunctionNative(add) == reinterpret_cast<void*>(g_base + vm.execAddIntInt);
    bool prologues = std::memcmp(pi, kProcessInternalPrologue, sizeof kProcessInternalPrologue) == 0 &&
                     std::memcmp(exec, kExecutorPrologue, sizeof kExecutorPrologue) == 0;
    const uint8_t* call = pi + vm.executorCallSite;
    bool callOk = call[0] == 0xE8 && call + 5 + Read<int32_t>(call, 1) == exec;
    HSM_LOG("engine: script VM Func@0x%zx %s, prologues %s, ProcessInternal->executor call %s", kFuncNative,
            funcOk ? "ok" : "MISMATCH", prologues ? "ok" : "MISMATCH", callOk ? "ok" : "MISMATCH");
    return funcOk && prologues && callOk ? const_cast<uint8_t*>(exec) : nullptr;
}

std::vector<FProperty*> Params(const UFunction* f) {
    std::vector<FProperty*> out;
    for (const void* p = Read<void*>(f, kStructChildProps); p; p = Read<void*>(p, kFFieldNext))
        if (Read<uint64_t>(p, kPropFlags) & kCPF_Parm) out.push_back(static_cast<FProperty*>(const_cast<void*>(p)));
    return out;
}

// ---------------------------------------------------------------- calls / fields
namespace {
// Whether address p lies in committed memory backed by a loaded image (a DLL or the exe): where every vtable lives.
bool InImage(const void* p) {
    MEMORY_BASIC_INFORMATION mbi;
    return VirtualQuery(p, &mbi, sizeof mbi) && mbi.State == MEM_COMMIT && mbi.Type == MEM_IMAGE;
}

// A pointer that is not (or no longer) an object has no vtable in an image: obj+0 is whatever data sits there, and the
// slot read from it is a jump to anything (0x300000002 in the 0.7.0-pre1 test). Vtables that passed are remembered.
bool LooksLikeObject(const UObject* obj) {
    const uintptr_t a = reinterpret_cast<uintptr_t>(obj);
    if (a < 0x10000 || (a & 7) || a >= 0x800000000000ull || IsBadReadPtr(obj, 8)) return false;
    const void* vtable = Read<const void*>(obj, 0);
    const uintptr_t v = reinterpret_cast<uintptr_t>(vtable);
    if (v < 0x10000 || (v & 7) || v >= 0x800000000000ull) return false;
    static thread_local uintptr_t known[64] = {};
    uintptr_t& slot = known[(v >> 4) & 63];
    if (slot == v) return true;
    // The slot's target is not checked: another mod may point a vtable slot at its own hook outside any image.
    const void* entry = static_cast<const uint8_t*>(vtable) + size_t(g_build->processEventIndex) * 8;
    if (!InImage(vtable) || !InImage(entry)) return false;
    slot = v;
    return true;
}
}  // namespace

bool ProcessEvent(UObject* obj, UFunction* fn, void* params) {
    using Fn = void (*)(UObject*, UFunction*, void*);
    if (!LooksLikeObject(obj)) {
        static int logged = 0;
        if (++logged <= 5) {
            HSM_LOG("engine: call of %s refused: %p is not an object (a stale pointer)", fn ? ObjectName(reinterpret_cast<UObject*>(fn)).c_str() : "?",
                    static_cast<const void*>(obj));
            LogCallStack("refused call");
        }
        return false;
    }
    void** vtable = Read<void**>(obj, 0);
    reinterpret_cast<Fn>(vtable[g_build->processEventIndex])(obj, fn, params);
    return true;
}

void* FieldPtr(UObject* obj, const std::string& property) {
    if (!obj) return nullptr;
    FProperty* p = FindProperty(ClassOf(obj), property);
    return p ? reinterpret_cast<uint8_t*>(obj) + PropertyOffset(p) : nullptr;
}

bool GetBoolValue(const FProperty* p, const void* value, bool& out) {
    if (!p || !value || PropertyType(p) != "BoolProperty") return false;
    const uint8_t* byte = static_cast<const uint8_t*>(value) + Read<uint8_t>(p, kBoolByteOffset);
    out = (*byte & Read<uint8_t>(p, kBoolByteMask)) != 0;
    return true;
}

bool SetBoolValue(const FProperty* p, void* value, bool v) {
    if (!p || !value || PropertyType(p) != "BoolProperty") return false;
    uint8_t mask = Read<uint8_t>(p, kBoolByteMask);
    uint8_t* byte = static_cast<uint8_t*>(value) + Read<uint8_t>(p, kBoolByteOffset);
    *byte = v ? uint8_t(*byte | mask) : uint8_t(*byte & ~mask);
    return true;
}

bool GetBool(UObject* obj, const std::string& property, bool& out) {
    FProperty* p = obj ? FindProperty(ClassOf(obj), property) : nullptr;
    return p && GetBoolValue(p, reinterpret_cast<uint8_t*>(obj) + PropertyOffset(p), out);
}

bool SetBool(UObject* obj, const std::string& property, bool value) {
    FProperty* p = obj ? FindProperty(ClassOf(obj), property) : nullptr;
    return p && SetBoolValue(p, reinterpret_cast<uint8_t*>(obj) + PropertyOffset(p), value);
}

Call::Call(UObject* target, UFunction* fn) : target_(target), fn_(fn) {
    if (fn_) buf_.assign(size_t(ParamsSize(fn_)) + 16, 0);
}
Call::Call(UObject* target, const std::string& name) : Call(target, target ? FindFunction(ClassOf(target), name) : nullptr) {}

void* Call::slot(const std::string& param, size_t size) {
    if (!fn_) return nullptr;
    FProperty* p = FindProperty(fn_, param);
    if (!p) { HSM_LOG("call: %s has no param %s", ObjectName(reinterpret_cast<UObject*>(fn_)).c_str(), param.c_str()); return nullptr; }
    if (size_t(PropertyOffset(p)) + size > buf_.size() || size_t(PropertySize(p)) < size) {
        HSM_LOG("call: param %s size mismatch (%d vs %zu)", param.c_str(), PropertySize(p), size); return nullptr;
    }
    return buf_.data() + PropertyOffset(p);
}

Call& Call::setReal(const std::string& param, double v) {
    FProperty* p = fn_ ? FindProperty(fn_, param) : nullptr;
    if (p && PropertySize(p) == int32_t(sizeof(double))) return set<double>(param, v);
    return set<float>(param, float(v));             // a missing or odd-sized param is logged there
}

Call& Call::setFrom(const std::string& param, const void* src) {
    FProperty* p = fn_ ? FindProperty(fn_, param) : nullptr;
    if (!p || !src) { HSM_LOG("call: setFrom %s failed", param.c_str()); return *this; }
    CopyPropertyValue(p, buf_.data() + PropertyOffset(p), src);
    return *this;
}

void* Call::paramPtr(const std::string& param) {
    FProperty* p = fn_ ? FindProperty(fn_, param) : nullptr;
    return p ? buf_.data() + PropertyOffset(p) : nullptr;
}

bool Call::invoke() {
    if (!valid()) return false;
    return ProcessEvent(target_, fn_, buf_.data());
}

PreparedCall::PreparedCall(const UClass* cls, const std::string& name, std::initializer_list<const char*> params) {
    UFunction* fn = cls ? FindFunction(cls, name) : nullptr;
    if (!fn) { HSM_LOG("prepare: %s has no function %s", cls ? ObjectName(reinterpret_cast<const UObject*>(cls)).c_str() : "(null class)", name.c_str()); return; }
    for (FProperty* p : Params(fn))
        if ((PropertyFlags(p) & (kCPF_OutParm | kCPF_ReturnParm)) && !(PropertyFlags(p) & kCPF_NoDestructor)) owning_.push_back(p);
    for (const char* param : params) {
        FProperty* p = FindProperty(fn, param);
        if (!p || !(PropertyFlags(p) & kCPF_Parm)) { HSM_LOG("prepare: %s has no param %s", name.c_str(), param); return; }
        off_.push_back(PropertyOffset(p));
        size_.push_back(PropertySize(p) * PropertyArrayDim(p));
    }
    cls_ = cls;
    fn_ = fn;
    buf_.assign(size_t(ParamsSize(fn)) + 16, 0);
}

PreparedCall& PreparedCall::operator=(PreparedCall&& o) noexcept {
    if (this == &o) return *this;
    cls_ = o.cls_; fn_ = o.fn_; ran_ = o.ran_;
    off_.swap(o.off_); size_.swap(o.size_); owning_.swap(o.owning_); buf_.swap(o.buf_);
    o.cls_ = nullptr; o.fn_ = nullptr; o.ran_ = false;
    o.off_.clear(); o.size_.clear(); o.owning_.clear(); o.buf_.clear();
    return *this;
}

void* PreparedCall::slot(size_t i, size_t size) {
    return const_cast<void*>(static_cast<const PreparedCall*>(this)->slot(i, size));
}

const void* PreparedCall::slot(size_t i, size_t size) const {
    if (!fn_ || i >= off_.size()) return nullptr;
    if (size_t(size_[i]) != size || size_t(off_[i]) + size > buf_.size()) {
        static int logged = 0;
        if (logged++ < 8) HSM_LOG("prepare: %s param #%zu is %d bytes, accessed as %zu", ObjectName(reinterpret_cast<UObject*>(fn_)).c_str(), i, size_[i], size);
        return nullptr;
    }
    return buf_.data() + off_[i];
}

bool PreparedCall::invoke(UObject* target) {
    if (!fn_ || !target || !IsA(target, cls_)) return false;
    if (ran_) for (FProperty* p : owning_) ClearPropertyValue(p, buf_.data() + PropertyOffset(p));
    if (!ProcessEvent(target, fn_, buf_.data())) return false;
    ran_ = true;
    return true;
}

FName MakeName(const std::string& s) {
    static UObject* lib = nullptr;
    if (!lib) if (UObject* cls = FindObject("KismetStringLibrary", "Class")) lib = DefaultObject(reinterpret_cast<UClass*>(cls));
    std::wstring w(s.begin(), s.end());
    FString str{w.data(), int32_t(w.size() + 1), int32_t(w.size() + 1)};
    Call c(lib, "Conv_StringToName");
    c.set("InString", str);
    c.invoke();
    return c.get<FName>("ReturnValue");
}

uintptr_t ModuleBase() { return g_base; }

// A class once found is checked at its object array slot (same object, name and class) before scanning all objects
// again: spawns, removals and loadouts ask for the same few classes many times, a blueprint class at two passes each.
UClass* FindClass(const std::string& name) {
    struct Found { UObject* o; int32_t index; FName name; UClass* cls; };
    static std::mutex mu;
    static std::unordered_map<std::string, Found> found;
    {
        std::lock_guard<std::mutex> lock(mu);
        if (auto it = found.find(name); it != found.end()) {
            const Found& f = it->second;
            if (ObjectAt(f.index) == f.o) {
                const FName now = Read<FName>(f.o, kObjName);
                if (now.index == f.name.index && now.number == f.name.number && ClassOf(f.o) == f.cls)
                    return reinterpret_cast<UClass*>(f.o);
            }
            found.erase(it);
        }
    }
    for (const char* kind : {"Class", "BlueprintGeneratedClass", "WidgetBlueprintGeneratedClass", "AnimBlueprintGeneratedClass"})
        if (UObject* o = FindObject(name, kind)) {
            std::lock_guard<std::mutex> lock(mu);
            found[name] = {o, ObjectIndex(o), Read<FName>(o, kObjName), ClassOf(o)};
            return reinterpret_cast<UClass*>(o);
        }
    return nullptr;
}

UObject* SpawnObject(UClass* cls, UObject* outer) {
    static UObject* statics = nullptr;
    if (!statics) if (UObject* c = FindObject("GameplayStatics", "Class")) statics = DefaultObject(reinterpret_cast<UClass*>(c));
    if (!cls || !outer || !statics) return nullptr;
    Call call(statics, "SpawnObject");
    call.set<UClass*>("ObjectClass", cls).set<UObject*>("Outer", outer);
    if (!call.invoke()) return nullptr;
    return call.get<UObject*>("ReturnValue");
}

void CopyPropertyValue(const FProperty* prop, void* dest, const void* src) {
    using CopyFn = void (*)(const FProperty*, void*, const void*, int32_t);
    void** vtable = Read<void**>(prop, 0);
    reinterpret_cast<CopyFn>(vtable[kPropVtCopyValues / sizeof(void*)])(prop, dest, src, 1);
}

void ClearPropertyValue(const FProperty* prop, void* value) {
    using ClearFn = void (*)(const FProperty*, void*);
    bool pod = (PropertyFlags(prop) & (kCPF_ZeroConstructor | kCPF_NoDestructor)) == (kCPF_ZeroConstructor | kCPF_NoDestructor);
    void** vtable = Read<void**>(prop, 0);
    auto* v = static_cast<uint8_t*>(value);
    for (int32_t i = 0, n = PropertyArrayDim(prop), size = PropertySize(prop); i < n; ++i, v += size) {
        if (pod) std::memset(v, 0, size_t(size));
        else reinterpret_cast<ClearFn>(vtable[kPropVtClearValue / sizeof(void*)])(prop, v);
    }
}

FText MakeText(const std::string& s) {
    static UObject* lib = nullptr;
    if (!lib) if (UObject* cls = FindObject("KismetTextLibrary", "Class")) lib = DefaultObject(reinterpret_cast<UClass*>(cls));
    FStringArgHolder str(s);
    Call c(lib, "Conv_StringToText");
    c.set("InString", str.arg);
    c.invoke();
    return c.get<FText>("ReturnValue");
}

FStringArgHolder::FStringArgHolder(const std::string& s) {
    static_assert(sizeof(wchar_t) == sizeof(char16_t), "FString is UTF-16");
    std::u16string u = Utf16FromUtf8(s);
    w.assign(u.begin(), u.end());
    arg = { w.c_str(), int32_t(w.size() + 1), int32_t(w.size() + 1) };
}

std::string ReadFString(const void* fstring) {
    const FString* s = static_cast<const FString*>(fstring);
    std::string out;
    if (!s || !s->data) return out;
    for (int32_t i = 0; i < s->num && s->data[i]; ++i) out.push_back(s->data[i] < 128 ? char(s->data[i]) : '?');
    return out;
}

bool SelfTest() {
    UObject* math = FindObject("KismetMathLibrary", "Class");
    UFunction* add = math ? FindFunction(reinterpret_cast<UClass*>(math), "Add_IntInt") : nullptr;
    if (!add) { HSM_LOG("selftest: KismetMathLibrary.Add_IntInt not found"); return false; }
    FProperty* a = FindProperty(add, "A"); FProperty* b = FindProperty(add, "B"); FProperty* r = FindProperty(add, "ReturnValue");
    bool layout = a && b && r && PropertyOffset(a) == 0 && PropertyOffset(b) == 4 && PropertyOffset(r) == 8 && PropertySize(a) == 4 && ParamsSize(add) == 12;
    HSM_LOG("selftest: Add_IntInt params %s (size %d, flags %08x)", layout ? "ok" : "MISMATCH", ParamsSize(add), FunctionFlags(add));
    if (!layout) return false;
    bool indexOk = ObjectAt(ObjectIndex(math)) == math;
    HSM_LOG("selftest: object index round trip %s", indexOk ? "ok" : "MISMATCH");
    if (!indexOk) return false;
    Call c(DefaultObject(reinterpret_cast<UClass*>(math)), add);
    c.set<int32_t>("A", 2).set<int32_t>("B", 3).invoke();
    int32_t sum = c.get<int32_t>("ReturnValue");
    HSM_LOG("selftest: ProcessEvent Add_IntInt(2,3) = %d", sum);
    return sum == 5 && IsName(math, "KismetMathLibrary");
}

}  // namespace hsm::ue
