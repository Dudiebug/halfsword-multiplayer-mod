#include <atomic>
#include "ue/hook.h"
#include "log.h"
#include "ue/profiler.h"
#include "util/hsm_object_ident.h"
#include <MinHook.h>
#include <algorithm>
#include <cstdio>
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
using util::ObjectIdent;
// Both tables are keyed by the function's address and carry the identity (object array slot and FName) of the function
// they were filled for. The engine frees functions inside a level (the Crypts stream their blocks in and out) and gives
// the address to the next function it makes: an entry counts only while the function at that address is still that one
// (public #3343, #3417, #3973, #3979, #3297: the hit hook ran on another function and read its OtherActor out of the
// wrong parameter layout).
struct ExactEntry { ObjectIdent ident; HookList list; };
std::shared_mutex g_mu;
std::unordered_map<UFunction*, ExactEntry> g_exact;                // includes resolved name rules
std::unordered_map<UFunction*, ObjectIdent> g_classified;          // function already checked against name rules
std::vector<NameRule> g_nameRules;
uint64_t g_replaced = 0;                                           // functions found replaced at a known address (unique lock)

// Functions with no hooks, per thread: almost every ProcessEvent call of a frame, so they skip the lock and the lookups.
// A slot counts only at the g_hookGen it was filled at; every change to the hooks bumps it (with the unique lock held).
std::atomic<uint32_t> g_hookGen{1};
struct NoHookSlot { UFunction* fn; uint32_t gen; ObjectIdent ident; };
constexpr size_t kNoHookSlots = 1024;
thread_local NoHookSlot t_noHook[kNoHookSlots];
NoHookSlot& NoHookSlotFor(UFunction* fn) {
    return t_noHook[size_t((uint64_t(reinterpret_cast<uintptr_t>(fn)) * 0x9E3779B97F4A7C15ull) >> 54)];   // top 10 bits
}

ObjectIdent IdentOf(const UFunction* fn) { return IdentityOf(reinterpret_cast<const UObject*>(fn)); }
FName NameOfKey(uint64_t key) { return FName{int32_t(uint32_t(key)), int32_t(uint32_t(key >> 32))}; }

// Adds a hook to a function's list; a list made for another function at this address is replaced. Requires the unique lock.
void AddHook(UFunction* fn, const ObjectIdent& ident, PreHook hook) {
    ExactEntry& e = g_exact[fn];
    if (e.ident != ident) e = ExactEntry{ident, nullptr};
    auto next = std::make_shared<std::vector<PreHook>>(e.list ? *e.list : std::vector<PreHook>{});
    next->push_back(std::move(hook));
    e.list = std::move(next);
    g_hookGen.fetch_add(1, std::memory_order_release);
}

// The function at fn is `now`: what the tables hold for another function at that address (a freed one) is dropped, its
// name-rule and exact hooks with it, so the function is classified afresh. Requires the unique lock.
void ForgetIfReplaced(UFunction* fn, const ObjectIdent& now) {
    bool replaced = false;
    ObjectIdent was;
    if (auto c = g_classified.find(fn); c != g_classified.end() && c->second != now) { was = c->second; g_classified.erase(c); replaced = true; }
    if (auto e = g_exact.find(fn); e != g_exact.end() && e->second.ident != now) {
        if (!replaced) was = e->second.ident;
        g_exact.erase(e);
        replaced = true;
    }
    if (!replaced) return;
    g_hookGen.fetch_add(1, std::memory_order_release);
    if (util::ReportDue(++g_replaced))   // the stored name is safe to print: the name pool never frees an entry
        HSM_LOG("hook: function %p was replaced (was index %d '%s', now index %d '%s'); its hooks re-resolved (%llu so far)",
                static_cast<void*>(fn), was.index, NameToString(NameOfKey(was.name)).c_str(), now.index,
                NameToString(NameOfKey(now.name)).c_str(), static_cast<unsigned long long>(g_replaced));
}

// Resolve name rules for a function seen for the first time. Requires the unique lock.
void Classify(UFunction* fn, const ObjectIdent& ident) {
    g_classified[fn] = ident;
    if (g_nameRules.empty()) return;
    std::string name = ObjectName(reinterpret_cast<UObject*>(fn));
    for (const NameRule& r : g_nameRules) if (r.match(name)) AddHook(fn, ident, r.hook);
}

std::atomic<unsigned long> g_gameThreadId{0};

// Stats (GetProcessEventHookStats): game thread only. t_inHooks is the depth of hook callbacks this thread is in.
uint64_t g_hookCalls = 0, g_hookNs = 0, g_allCalls = 0;
struct FnCost { uint64_t calls = 0, ns = 0; };
std::unordered_map<UFunction*, FnCost> g_fnCost;   // per hooked function, since the last TopHookFunctions
thread_local int t_inHooks = 0;
thread_local uint64_t t_discountNs = 0;   // DiscountHookTime, summed

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
    if (first.load(std::memory_order_relaxed) && first.exchange(false)) {
        g_gameThreadId.store(GetCurrentThreadId());
        HSM_LOG("hook: first ProcessEvent call (%s)", fn ? ObjectName(reinterpret_cast<UObject*>(fn)).c_str() : "?");
    }
    bool block = false;
    if (GetCurrentThreadId() == g_gameThreadId.load(std::memory_order_relaxed)) ++g_allCalls;
    prof::Scope call(fn, prof::Kind::kEvent);   // the whole call: our hooks and the original
    if (obj && fn) {
        const uint32_t gen = g_hookGen.load(std::memory_order_acquire);   // before the lookup: a later change stales the slot
        // The engine passes a live function: its slot and name (two loads) tell whether it is the one an entry was made for.
        const ObjectIdent ident = IdentOf(fn);
        NoHookSlot& slot = NoHookSlotFor(fn);
        if (slot.fn == fn && slot.gen == gen && slot.ident == ident) { g_original(obj, fn, params); return; }
        HookList hooks;
        {
            std::shared_lock<std::shared_mutex> lock(g_mu);
            auto c = g_classified.find(fn);
            bool known = c != g_classified.end() && c->second == ident;
            if (known) {
                auto it = g_exact.find(fn);
                if (it != g_exact.end()) { if (it->second.ident == ident) hooks = it->second.list; else known = false; }
            }
            if (!known) {
                lock.unlock();
                std::unique_lock<std::shared_mutex> w(g_mu);
                ForgetIfReplaced(fn, ident);
                if (!g_classified.count(fn)) Classify(fn, ident);
                auto it = g_exact.find(fn); if (it != g_exact.end()) hooks = it->second.list;
            }
        }
        if (!hooks || hooks->empty()) slot = {fn, gen, ident};   // filled before any hook runs (a hook may re-enter this detour)
        // Hooks run without the lock so they may call ProcessEvent (re-entering this detour); the list they run
        // stays alive (and unchanged) even if a rule is added meanwhile.
        if (hooks && !hooks->empty()) {
            const bool timed = t_inHooks == 0 && GetCurrentThreadId() == g_gameThreadId.load(std::memory_order_relaxed);
            const int64_t t0 = timed ? Qpc() : 0;
            const uint64_t discount0 = t_discountNs;
            {
                InHooks in;
                prof::Scope ours(fn, prof::Kind::kHook);
                for (const PreHook& h : *hooks) if (h(obj, fn, params)) block = true;
            }
            if (timed) {
                const uint64_t spent = uint64_t(QpcNs(Qpc() - t0)), discount = t_discountNs - discount0;
                const uint64_t ns = spent > discount ? spent - discount : 0;
                ++g_hookCalls;
                g_hookNs += ns;
                FnCost& f = g_fnCost[fn];
                ++f.calls;
                f.ns += ns;
            }
        }
    }
    if (!block) g_original(obj, fn, params);
}
}  // namespace

namespace {
std::atomic<bool> g_legacyRelay{false};   // set once by UseLegacyHookRelay, before the first hook
}  // namespace

void UseLegacyHookRelay(bool legacy) { g_legacyRelay.store(legacy); }

bool EnsureMinHook() {
    static bool ok = [] {
        if (MH_Initialize() != MH_OK) return false;
        MH_SetLegacyRelay(g_legacyRelay.load());
        HSM_LOG("hook: relay form %s (hook_relay=%s)", g_legacyRelay.load() ? "FF 25 jmp [rip+0] (legacy)" : "48 B8 mov rax,imm64 ; FF E0 jmp rax",
                g_legacyRelay.load() ? "legacy" : "jmp");
        return true;
    }();
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
    AddHook(fn, IdentOf(fn), std::move(hook));
}

void HookFunctionsMatching(std::function<bool(const std::string&)> match, PreHook hook) {
    std::unique_lock<std::shared_mutex> lock(g_mu);
    g_nameRules.push_back({match, hook});
    // Re-check functions already classified so the rule also applies to them. A function freed since is dropped unread:
    // the object array no longer holds it at its slot, or holds it under another name.
    size_t dropped = 0;
    for (auto it = g_classified.begin(); it != g_classified.end();) {
        UFunction* fn = it->first;
        const ObjectIdent stored = it->second;
        if (!util::CacheEntryCurrent(stored, fn, ObjectAt(stored.index), [fn] { return IdentOf(fn); })) {
            if (auto e = g_exact.find(fn); e != g_exact.end() && e->second.ident == stored) g_exact.erase(e);
            it = g_classified.erase(it);
            ++dropped;
            continue;
        }
        if (match(ObjectName(reinterpret_cast<UObject*>(fn)))) AddHook(fn, stored, hook);
        ++it;
    }
    if (dropped) {
        g_hookGen.fetch_add(1, std::memory_order_release);
        HSM_LOG("hook: %zu function(s) freed or replaced since they were classified; dropped before a new name rule", dropped);
    }
}

void ClearHooks() {
    std::unique_lock<std::shared_mutex> lock(g_mu);
    g_exact.clear(); g_classified.clear(); g_nameRules.clear();
    g_hookGen.fetch_add(1, std::memory_order_release);
}

unsigned long GameThreadId() { return g_gameThreadId.load(); }

void NoteGameThread() {
    const unsigned long me = GetCurrentThreadId();
    if (g_gameThreadId.load(std::memory_order_relaxed) == me) return;
    const unsigned long was = g_gameThreadId.exchange(me);
    if (was != me) HSM_LOG("hook: game thread is %lu; the first ProcessEvent call came from thread %lu", me, was);
}

void DiscountHookTime(uint64_t ns) { t_discountNs += ns; }

std::string TopHookFunctions(int n, double frames) {
    std::vector<std::pair<UFunction*, FnCost>> v(g_fnCost.begin(), g_fnCost.end());
    g_fnCost.clear();
    std::sort(v.begin(), v.end(), [](const auto& a, const auto& b) { return a.second.ns > b.second.ns; });
    if (frames < 1.0) frames = 1.0;
    std::string out;
    char b[200];
    for (int i = 0; i < n && i < int(v.size()); ++i) {
        std::snprintf(b, sizeof b, "%s%s %.2f ms/%.0f", out.empty() ? "" : ", ", ObjectName(reinterpret_cast<UObject*>(v[size_t(i)].first)).c_str(),
                      double(v[size_t(i)].second.ns) / 1e6 / frames, double(v[size_t(i)].second.calls) / frames);
        out += b;
    }
    return out;
}

ProcessEventHookStats GetProcessEventHookStats() {
    ProcessEventHookStats s;
    s.calls = g_hookCalls;
    s.hookNs = g_hookNs;
    s.all = g_allCalls;
    return s;
}

}  // namespace hsm::ue
