#include "ue/script_hook.h"
#include "ue/hook.h"
#include "log.h"
#include <windows.h>
#include <algorithm>
#include <atomic>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <utility>
#include <vector>

namespace hsm::ue {
namespace {

// ---- FFrame / FOutParmRec layout of this build, read from ProcessScriptFunction (0x1414A21F0, which builds the callee
// frame), ProcessInternal and ProcessLocalScriptFunction: Node 0x10, Object 0x18, Code 0x20, Locals 0x28, OutParms 0x80.
constexpr size_t kFrameNode = 0x10, kFrameLocals = 0x28, kFrameOutParms = 0x80;
constexpr size_t kOutRecProperty = 0x00, kOutRecAddr = 0x08, kOutRecNext = 0x10;
constexpr uint64_t kCPF_ConstParm = 0x2, kCPF_OutParm = 0x100, kCPF_ReturnParm = 0x400, kCPF_ReferenceParm = 0x8000000;
constexpr int kMaxFunctions = 32;

template <class T> T Read(const void* base, size_t off) { T v; std::memcpy(&v, static_cast<const uint8_t*>(base) + off, sizeof(T)); return v; }

using ExecutorFn = void (*)(UObject*, void*, void*);
ExecutorFn g_original = nullptr;

struct Hook { ScriptPre pre; ScriptPost post; ScriptPost before; bool observer = false; };
struct Rule { std::string owner, className, functionName; Hook hook; };
struct Entry { std::vector<Hook> hooks; bool hasObserver = false; };
// Published snapshot read lock-free by the detour on every Blueprint call. Never modified once published; superseded
// snapshots stay allocated (a call on another thread may still be reading one) and are small.
struct Table { int n = 0; UFunction* fns[kMaxFunctions] = {}; const Entry* entries[kMaxFunctions] = {}; };

std::mutex g_mu;
std::vector<Rule> g_rules;
std::vector<std::unique_ptr<const Table>> g_tables;
std::vector<std::unique_ptr<const Entry>> g_entries;
std::atomic<const Table*> g_table{nullptr};
thread_local int t_bypass = 0;

// Stats. g_calls is bumped without a locked RMW (load + store), so it may drop increments from other threads.
std::atomic<uint64_t> g_calls{0};
uint64_t g_hooked = 0, g_blocked = 0, g_hookNs = 0;   // game thread only

int64_t QpcNs(int64_t ticks) {
    static const int64_t freq = [] { LARGE_INTEGER f; QueryPerformanceFrequency(&f); return f.QuadPart; }();
    return ticks * 1000000000ll / freq;
}
int64_t Qpc() { LARGE_INTEGER t; QueryPerformanceCounter(&t); return t.QuadPart; }

void RunHooked(const Entry& e, UObject* ctx, void* stack, void* result) {
    ScriptCall call(ctx, stack, result);
    ++g_hooked;
    bool block = false;
    int64_t t0 = Qpc();
    for (const Hook& h : e.hooks) if (h.before) h.before(call);
    for (const Hook& h : e.hooks) if (h.pre && h.pre(call)) block = true;
    int64_t spent = Qpc() - t0;
    if (block) ++g_blocked;
    else {
        g_original(ctx, stack, result);
        int64_t t1 = Qpc();
        for (const Hook& h : e.hooks) if (h.post) h.post(call);
        spent += Qpc() - t1;
    }
    g_hookNs += uint64_t(QpcNs(spent));
}

// A call under ScriptHookBypass: only observers run, around the body, which always runs.
void RunObserved(const Entry& e, UObject* ctx, void* stack, void* result) {
    ScriptCall call(ctx, stack, result);
    int64_t t0 = Qpc();
    for (const Hook& h : e.hooks) if (h.observer && h.before) h.before(call);
    int64_t spent = Qpc() - t0;
    g_original(ctx, stack, result);
    int64_t t1 = Qpc();
    for (const Hook& h : e.hooks) if (h.observer && h.post) h.post(call);
    spent += Qpc() - t1;
    g_hookNs += uint64_t(QpcNs(spent));
}

// The work every executor entry pays: count it and look its function up in the published table.
inline const Entry* Lookup(UFunction* node) {
    g_calls.store(g_calls.load(std::memory_order_relaxed) + 1, std::memory_order_relaxed);
    if (const Table* t = g_table.load(std::memory_order_acquire))
        for (int i = 0; i < t->n; ++i)
            if (t->fns[i] == node) return t->entries[i];
    return nullptr;
}

void Detour(UObject* ctx, void* stack, void* result) {
    const Entry* e = Lookup(Read<UFunction*>(stack, kFrameNode));
    if (e && GetCurrentThreadId() == GameThreadId()) {
        if (t_bypass == 0) { RunHooked(*e, ctx, stack, result); return; }
        if (e->hasObserver) { RunObserved(*e, ctx, stack, result); return; }
    }
    g_original(ctx, stack, result);
}

// The rule's function and every override of it in a subclass: same FName, outer class derived from the rule's class.
std::vector<UFunction*> Resolve(const Rule& r) {
    std::vector<UFunction*> out;
    UClass* cls = FindClass(r.className);
    UFunction* base = cls ? FindFunction(cls, r.functionName) : nullptr;
    if (!base) return out;
    out.push_back(base);
    FName name = ObjectFName(reinterpret_cast<UObject*>(base));
    UClass* functionClass = ClassOf(reinterpret_cast<UObject*>(base));
    for (int32_t i = 0, n = ObjectCount(); i < n; ++i) {
        UObject* o = ObjectAt(i);
        if (!o || o == reinterpret_cast<UObject*>(base)) continue;
        FName on = ObjectFName(o);
        if (on.index != name.index || on.number != name.number || ClassOf(o) != functionClass) continue;
        if (IsChildOf(reinterpret_cast<UClass*>(OuterOf(o)), cls)) out.push_back(reinterpret_cast<UFunction*>(o));
    }
    return out;
}

// The parameters of a hooked function as the engine spells them, once per function per process. A shipping build
// displays a name in the casing registered first ("bone" for a "Bone" parameter), so this line is the record of what a
// ScriptCall lookup is matched against. Requires g_mu.
std::set<std::string> g_paramsLogged;
void LogParamsOnce(UFunction* fn) {
    const std::string name = FullName(reinterpret_cast<UObject*>(fn));
    if (!g_paramsLogged.insert(name).second) return;
    std::string list;
    for (FProperty* p : Params(fn)) list += (list.empty() ? "" : ", ") + PropertyName(p);
    HSM_LOG("vm: params of %s: %s", name.c_str(), list.empty() ? "(none)" : list.c_str());
}

// Rebuilds and publishes the table from the rules; returns how many functions are hooked, and in lastRule how many the
// newest rule resolved to. Requires g_mu.
int Publish(int* lastRule = nullptr) {
    std::map<UFunction*, std::unique_ptr<Entry>> byFn;
    std::vector<UFunction*> order;
    for (const Rule& r : g_rules) {
        std::vector<UFunction*> fns = Resolve(r);
        if (lastRule) *lastRule = 0;
        if (fns.empty()) HSM_LOG("vm: %s.%s not loaded yet", r.className.c_str(), r.functionName.c_str());
        for (UFunction* fn : fns) {
            if (!IsScriptFunction(fn)) {
                HSM_LOG("vm: %s is not a Blueprint body (Func %p); not hooked", FullName(reinterpret_cast<UObject*>(fn)).c_str(), FunctionNative(fn));
                continue;
            }
            if (lastRule) ++*lastRule;
            LogParamsOnce(fn);
            auto& e = byFn[fn];
            if (!e) { e = std::make_unique<Entry>(); order.push_back(fn); }
            e->hooks.push_back(r.hook);
            e->hasObserver = e->hasObserver || r.hook.observer;
        }
    }
    auto t = std::make_unique<Table>();
    for (UFunction* fn : order) {
        if (t->n == kMaxFunctions) { HSM_LOG("vm: more than %d hooked functions; %s dropped", kMaxFunctions, FullName(reinterpret_cast<UObject*>(fn)).c_str()); continue; }
        t->fns[t->n] = fn;
        t->entries[t->n] = byFn[fn].get();
        ++t->n;
        g_entries.push_back(std::move(byFn[fn]));
    }
    int n = t->n;
    g_table.store(t.get(), std::memory_order_release);
    g_tables.push_back(std::move(t));
    return n;
}

std::mutex g_propMu;
std::map<std::pair<UFunction*, std::string>, FProperty*> g_props;
std::set<std::pair<UFunction*, std::string>> g_sizeWarned;
}  // namespace

// ---------------------------------------------------------------- ScriptCall
UFunction* ScriptCall::function() const { return Read<UFunction*>(stack_, kFrameNode); }

FProperty* ScriptCall::prop(const std::string& name) const {
    std::lock_guard<std::mutex> lock(g_propMu);
    auto key = std::make_pair(function(), name);
    auto it = g_props.find(key);
    if (it != g_props.end()) return it->second;
    FProperty* p = FindProperty(function(), name);
    g_props.emplace(key, p);
    return p;
}

void* ScriptCall::param(const std::string& name) const {
    FProperty* p = prop(name);
    if (!p) return nullptr;
    uint64_t flags = PropertyFlags(p);
    if (flags & kCPF_ReturnParm) return result_;
    if (flags & kCPF_OutParm) {
        for (const void* rec = Read<void*>(stack_, kFrameOutParms); rec; rec = Read<void*>(rec, kOutRecNext))
            if (Read<FProperty*>(rec, kOutRecProperty) == p) return Read<void*>(rec, kOutRecAddr);
        return nullptr;
    }
    return Read<uint8_t*>(stack_, kFrameLocals) + PropertyOffset(p);
}

void* ScriptCall::sized(const std::string& name, size_t size) const {
    FProperty* p = prop(name);
    if (p && size_t(PropertySize(p)) == size && PropertyArrayDim(p) == 1) return param(name);
    std::lock_guard<std::mutex> lock(g_propMu);
    if (g_sizeWarned.insert({function(), name}).second)
        HSM_LOG("vm: %s param %s: %s", ObjectName(reinterpret_cast<UObject*>(function())).c_str(), name.c_str(),
                p ? ("size " + std::to_string(PropertySize(p)) + " != " + std::to_string(size)).c_str() : "missing");
    return nullptr;
}

bool ScriptCall::getBool(const std::string& name, bool& out) const { return GetBoolValue(prop(name), param(name), out); }
bool ScriptCall::setBool(const std::string& name, bool v) { return SetBoolValue(prop(name), param(name), v); }

void ScriptCall::ZeroOutputs() {
    for (FProperty* p : Params(function())) {
        uint64_t flags = PropertyFlags(p);
        void* v = nullptr;
        if (flags & kCPF_ReturnParm) v = result_;
        else if ((flags & kCPF_OutParm) && !(flags & (kCPF_ConstParm | kCPF_ReferenceParm))) v = param(PropertyName(p));
        if (v) ClearPropertyValue(p, v);
    }
}

// ---------------------------------------------------------------- install / rules
bool InstallScriptHook() {
    if (g_original) return true;
    void* exec = ScriptExecutorAddress();
    if (!exec) { HSM_LOG("vm: script executor not verified for this build; Blueprint call hooks disabled"); return false; }
    if (!HookNative(exec, reinterpret_cast<void*>(&Detour), reinterpret_cast<void**>(&g_original), "ProcessLocalScriptFunction")) return false;
    ResolveScriptHooks();
    return true;
}

bool ScriptHookActive() { return g_original != nullptr; }

int HookScript(const std::string& owner, const std::string& className, const std::string& functionName, ScriptPre pre,
               ScriptPost post) {
    std::lock_guard<std::mutex> lock(g_mu);
    Hook hook;
    hook.pre = std::move(pre);
    hook.post = std::move(post);
    g_rules.push_back({owner, className, functionName, std::move(hook)});
    int mine = 0, total = Publish(&mine);
    HSM_LOG("vm: [%s] hook %s.%s -> %d function(s), %d hooked in all", owner.c_str(), className.c_str(), functionName.c_str(), mine, total);
    return mine;
}

int ObserveScript(const std::string& owner, const std::string& className, const std::string& functionName,
                  ScriptPost before, ScriptPost after) {
    std::lock_guard<std::mutex> lock(g_mu);
    Hook hook;
    hook.before = std::move(before);
    hook.post = std::move(after);
    hook.observer = true;
    g_rules.push_back({owner, className, functionName, std::move(hook)});
    int mine = 0, total = Publish(&mine);
    HSM_LOG("vm: [%s] observe %s.%s -> %d function(s), %d hooked in all", owner.c_str(), className.c_str(), functionName.c_str(), mine, total);
    return mine;
}

int ResolveScriptHooks() {
    std::lock_guard<std::mutex> lock(g_mu);
    return Publish();
}

int ClearScriptHooks(const std::string& owner) {
    std::lock_guard<std::mutex> lock(g_mu);
    const size_t before = g_rules.size();
    g_rules.erase(std::remove_if(g_rules.begin(), g_rules.end(), [&](const Rule& r) { return r.owner == owner; }), g_rules.end());
    const int removed = int(before - g_rules.size());
    if (removed) Publish();
    return removed;
}

ScriptHookBypass::ScriptHookBypass() { ++t_bypass; }
ScriptHookBypass::~ScriptHookBypass() { --t_bypass; }

double MeasureScriptHookMissNs() {
    constexpr int kIters = 200000;
    static UFunction* volatile probe = nullptr;   // matches no hooked function: the full scan every unhooked call takes
    const Entry* volatile sink = nullptr;
    uint64_t before = g_calls.load(std::memory_order_relaxed);
    int64_t t0 = Qpc();
    for (int k = 0; k < kIters; ++k) sink = Lookup(probe);
    int64_t spent = Qpc() - t0;
    (void)sink;
    g_calls.store(before, std::memory_order_relaxed);   // the probes are not executor calls
    return double(QpcNs(spent)) / kIters;
}

ScriptHookStats GetScriptHookStats() {
    ScriptHookStats s;
    s.executorCalls = g_calls.load(std::memory_order_relaxed);
    s.hookedCalls = g_hooked; s.blocked = g_blocked; s.hookNs = g_hookNs;
    if (const Table* t = g_table.load(std::memory_order_acquire)) s.functions = t->n;
    return s;
}

}  // namespace hsm::ue
