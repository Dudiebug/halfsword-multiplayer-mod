// Script-VM interception. Blueprint-to-Blueprint calls (a weapon calling "Deal Complex Damage" on the fighter it hit,
// Deal Complex Damage calling "Get Damage", ...) run inside the Kismet VM and never reach ProcessEvent. Every Blueprint
// function body, whatever called it (ProcessEvent through ProcessInternal, UObject::CallFunction, a local final or
// virtual call opcode), executes through one out-of-line executor, ProcessLocalScriptFunction(Context, Stack, Result),
// with Stack.Node the function and its parameters already evaluated. This detours that executor for a short list of
// functions: pre-hooks see and may rewrite the parameters and may block the body; post-hooks see the outputs.
#pragma once
#include "ue/engine.h"
#include <cstdint>
#include <cstring>
#include <functional>
#include <string>

namespace hsm::ue {

// One Blueprint function call caught in the VM. Valid only inside the hook that received it.
class ScriptCall {
public:
    ScriptCall(UObject* self, void* stack, void* result) : self_(self), stack_(stack), result_(result) {}
    UObject* self() const { return self_; }          // the object the function runs on (FFrame::Object)
    UFunction* function() const;                     // FFrame::Node
    // A parameter's value: value parameters live in the callee's frame, out and by-reference parameters in the
    // caller's storage (the FFrame::OutParms chain), the return value in the caller's result buffer. nullptr if absent.
    void* param(const std::string& name) const;
    // Typed access. Fails (false, logged once per function and name) unless the property's size is exactly sizeof(T).
    template <class T> bool get(const std::string& name, T& out) const {
        const void* p = sized(name, sizeof(T));
        if (p) std::memcpy(&out, p, sizeof(T));
        return p != nullptr;
    }
    template <class T> T get(const std::string& name) const { T v{}; get(name, v); return v; }
    template <class T> bool set(const std::string& name, const T& v) {
        void* p = sized(name, sizeof(T));
        if (p) std::memcpy(p, &v, sizeof(T));
        return p != nullptr;
    }
    bool getBool(const std::string& name, bool& out) const;
    bool setBool(const std::string& name, bool v);
    // A blocked call leaves the caller's out variables as they were (possibly stale from an earlier call). This clears
    // every pure output and the return value; by-reference inputs are left alone. Set specific outputs after it.
    void ZeroOutputs();
private:
    FProperty* prop(const std::string& name) const;
    void* sized(const std::string& name, size_t size) const;
    UObject* self_;
    void* stack_;
    void* result_;
};

using ScriptPre = std::function<bool(ScriptCall&)>;    // true = block: the body does not run (post-hooks are skipped)
using ScriptPost = std::function<void(ScriptCall&)>;   // after the body ran

// Verifies this build's executor (engine ScriptExecutorAddress) and detours it. False = VM hooks unavailable: the
// failure is logged and every HookScript stays inert (fail closed). Call on the game thread once Blueprints are loaded.
bool InstallScriptHook();
bool ScriptHookActive();

// Hooks the Blueprint function functionName of class className and every override of it in subclasses. Names are
// resolved now and again by ResolveScriptHooks (call after each level load: classes and overrides load with levels).
// Returns how many UFunctions it resolved to now. At most 32 functions can be hooked; hooks run on the game thread only
// (calls on other threads, e.g. thread-safe anim graph functions, pass straight through). owner names who registered the
// rule ("vmhook", "standin", ...): ClearScriptHooks(owner) removes that owner's rules only, so turning one feature's
// hooks off never drops another's (the stand-in damage guard survives `vmhook off`).
int HookScript(const std::string& owner, const std::string& className, const std::string& functionName, ScriptPre pre,
               ScriptPost post = {});
int ResolveScriptHooks();   // total UFunctions hooked
int ClearScriptHooks(const std::string& owner);   // removes that owner's rules; returns how many

// Calls made on this thread while one is alive pass through the VM hook untouched, including every call nested inside
// them (our own native replays, e.g. Deal Complex Damage and the Get Damage it calls).
struct ScriptHookBypass {
    ScriptHookBypass();
    ~ScriptHookBypass();
    ScriptHookBypass(const ScriptHookBypass&) = delete;
    ScriptHookBypass& operator=(const ScriptHookBypass&) = delete;
};

struct ScriptHookStats {
    uint64_t executorCalls = 0;   // executor entries on all threads (approximate: counted without a locked add)
    uint64_t hookedCalls = 0;     // calls to a hooked function that ran the hooks (game thread)
    uint64_t blocked = 0;         // bodies skipped by a pre-hook
    uint64_t hookNs = 0;          // time spent in hook callbacks (not in the bodies)
    int functions = 0;            // UFunctions currently hooked
};
ScriptHookStats GetScriptHookStats();   // game thread
// Measured cost of the detour's lookup for a function that is not hooked (what every other Blueprint call pays), ns.
// Times 200k lookups (about a millisecond on the game thread): for diagnostics, not every frame.
double MeasureScriptHookMissNs();

}  // namespace hsm::ue
