#include <atomic>
#include "ue/hook.h"
#include "log.h"
#include <MinHook.h>
#include <memory>
#include <shared_mutex>
#include <unordered_map>
#include <vector>

namespace hsm::ue {
namespace {
using ProcessEventFn = void (*)(UObject*, UFunction*, void*);
ProcessEventFn g_original = nullptr;
void* g_target = nullptr;

struct NameRule { std::function<bool(const std::string&)> match; PreHook hook; };

// A function's hooks are an immutable list, replaced (never edited) when a rule is added: a hooked call takes a reference
// instead of copying the list (test 5: the hit hook alone ran 8970 times in one frame).
using HookList = std::shared_ptr<const std::vector<PreHook>>;
std::shared_mutex g_mu;
std::unordered_map<UFunction*, HookList> g_exact;                  // includes resolved name rules
std::unordered_map<UFunction*, bool> g_classified;                 // function already checked against name rules
std::vector<NameRule> g_nameRules;

// Adds a hook to a function's list. Requires the unique lock.
void AddHook(UFunction* fn, PreHook hook) {
    HookList& list = g_exact[fn];
    auto next = std::make_shared<std::vector<PreHook>>(list ? *list : std::vector<PreHook>{});
    next->push_back(std::move(hook));
    list = std::move(next);
}

// Resolve name rules for a function seen for the first time. Requires the unique lock.
void Classify(UFunction* fn) {
    g_classified[fn] = true;
    if (g_nameRules.empty()) return;
    std::string name = ObjectName(reinterpret_cast<UObject*>(fn));
    for (const NameRule& r : g_nameRules) if (r.match(name)) AddHook(fn, r.hook);
}

std::atomic<unsigned long> g_gameThreadId{0};

// Stats (GetProcessEventHookStats): game thread only. t_inHooks is the depth of hook callbacks this thread is in.
uint64_t g_hookCalls = 0, g_hookNs = 0;
thread_local int t_inHooks = 0;

int64_t QpcNs(int64_t ticks) {
    static const int64_t freq = [] { LARGE_INTEGER f; QueryPerformanceFrequency(&f); return f.QuadPart; }();
    return ticks * 1000000000ll / freq;
}
int64_t Qpc() { LARGE_INTEGER t; QueryPerformanceCounter(&t); return t.QuadPart; }

struct InHooks {
    InHooks() { ++t_inHooks; }
    ~InHooks() { --t_inHooks; }
    InHooks(const InHooks&) = delete;
    InHooks& operator=(const InHooks&) = delete;
};

void Detour(UObject* obj, UFunction* fn, void* params) {
    static std::atomic<bool> first{true};
    if (first.exchange(false)) {
        g_gameThreadId.store(GetCurrentThreadId());
        HSM_LOG("hook: first ProcessEvent call (%s)", fn ? ObjectName(reinterpret_cast<UObject*>(fn)).c_str() : "?");
    }
    bool block = false;
    if (obj && fn) {
        HookList hooks;
        {
            std::shared_lock<std::shared_mutex> lock(g_mu);
            bool known = g_classified.count(fn) != 0;
            if (known) { auto it = g_exact.find(fn); if (it != g_exact.end()) hooks = it->second; }
            if (!known) {
                lock.unlock();
                std::unique_lock<std::shared_mutex> w(g_mu);
                if (!g_classified.count(fn)) Classify(fn);
                auto it = g_exact.find(fn); if (it != g_exact.end()) hooks = it->second;
            }
        }
        // Hooks run without the lock so they may call ProcessEvent (re-entering this detour); the list they run
        // stays alive (and unchanged) even if a rule is added meanwhile.
        if (hooks && !hooks->empty()) {
            const bool timed = t_inHooks == 0 && GetCurrentThreadId() == g_gameThreadId.load(std::memory_order_relaxed);
            const int64_t t0 = timed ? Qpc() : 0;
            {
                InHooks in;
                for (const PreHook& h : *hooks) if (h(obj, fn, params)) block = true;
            }
            if (timed) { ++g_hookCalls; g_hookNs += uint64_t(QpcNs(Qpc() - t0)); }
        }
    }
    if (!block) g_original(obj, fn, params);
}
}  // namespace

bool EnsureMinHook() {
    static bool ok = MH_Initialize() == MH_OK;
    return ok;
}

bool HookNative(void* target, void* detour, void** original, const char* label) {
    if (!target || !EnsureMinHook()) return false;
    if (MH_CreateHook(target, detour, original) != MH_OK || MH_EnableHook(target) != MH_OK) {
        HSM_LOG("hook: failed to hook %s at %p", label, target); return false;
    }
    HSM_LOG("hook: %s hooked at %p", label, target);
    return true;
}

bool InstallProcessEventHook() {
    g_target = ProcessEventAddress();
    if (!g_target) { HSM_LOG("hook: ProcessEvent address unavailable"); return false; }
    if (!EnsureMinHook()) { HSM_LOG("hook: MH_Initialize failed"); return false; }
    if (MH_CreateHook(g_target, reinterpret_cast<void*>(&Detour), reinterpret_cast<void**>(&g_original)) != MH_OK ||
        MH_EnableHook(g_target) != MH_OK) {
        HSM_LOG("hook: failed to hook ProcessEvent at %p", g_target); return false;
    }
    HSM_LOG("hook: ProcessEvent hooked at %p", g_target);
    return true;
}

void RemoveProcessEventHook() {
    if (g_target) { MH_DisableHook(g_target); MH_RemoveHook(g_target); g_target = nullptr; }
}

void HookFunction(UFunction* fn, PreHook hook) {
    if (!fn) return;
    std::unique_lock<std::shared_mutex> lock(g_mu);
    AddHook(fn, std::move(hook));
}

void HookFunctionsMatching(std::function<bool(const std::string&)> match, PreHook hook) {
    std::unique_lock<std::shared_mutex> lock(g_mu);
    g_nameRules.push_back({match, hook});
    // Re-check functions already classified so the rule also applies to them.
    for (auto& [fn, _] : g_classified)
        if (match(ObjectName(reinterpret_cast<UObject*>(fn)))) AddHook(fn, hook);
}

void ClearHooks() {
    std::unique_lock<std::shared_mutex> lock(g_mu);
    g_exact.clear(); g_classified.clear(); g_nameRules.clear();
}

unsigned long GameThreadId() { return g_gameThreadId.load(); }

ProcessEventHookStats GetProcessEventHookStats() {
    ProcessEventHookStats s;
    s.calls = g_hookCalls;
    s.hookNs = g_hookNs;
    return s;
}

}  // namespace hsm::ue
