// When HSMMod's dwmapi.dll may chain-load UE4SS (public #4221, #4251, #4255 on 0.8.0; #2475 on UE4SS 3.0.1).
//
// UE4SS's stock BPModLoaderMod calls IterateGameDirectories at the top of its main.lua, on UE4SS's own thread, as soon as
// UE4SS starts its mods. That reaches UObjectGlobals::RootDir and then UBlueprintPathsLibrary::RootDir, which looks up
// "/Script/Engine.Default__BlueprintPathsLibrary" once and calls ProcessEvent on it with no null check. While the engine
// is still starting up that default object is not made yet: a crash at the splash screen, UE4SS.dll's UObject::ProcessEvent
// reading 0x0, with the stock UE4SS mods only. HSMMod loads UE4SS (ue4ss_order=after) as soon as its own hooks are in,
// which is still engine start-up, so HSMMod decides when that lookup runs.
// November 2026 logs: in about 3,350 starts where UE4SS's call was the process's first ProcessEvent, 77 crashed there
// (2.3%); in 119 starts where the game thread had made its first ProcessEvent call (the game instance's ReceiveInit) before
// UE4SS's, none did. By ReceiveInit the engine has started every module, so every native class has its default object.
// So HSMMod waits for the game's first ProcessEvent call (its hook sees it) before it loads UE4SS. That call came 4.7 s
// after HSMMod's hook at the median, 9.3 s at the 90th percentile, 58 s at most (524 starts with the game's call first).
// ue4ss_wait=0 loads UE4SS at once, as before.
// Game-independent and unit-tested (session/test_hsm_ue4ss_wait.cpp); ue4ss_load.cpp carries it out.
#pragma once
#include "session/hsm_hook_order.h"
#include <cstdint>

namespace hsm {

constexpr uint32_t kUe4ssLoadWaitMs = 60000;   // then UE4SS loads anyway (the longest wait seen was 58 s)
// While UE4SS waits its load is Pending, and HSMMod's script VM hook waits for UE4SS's (hsm_hook_order.h): the load wait and
// UE4SS's own hook time stay inside that wait, so the order holds.
static_assert(kUe4ssLoadWaitMs + kUe4ssHookWaitMs < kUe4ssPendingWaitMs, "the VM hook would stop waiting for UE4SS");

enum class Ue4ssLoadGate : uint8_t {
    Wait,       // not yet
    NoWait,     // load now: ue4ss_wait=0, or no ProcessEvent hook to see the game's call (role=off, ue4ss_order=before, a failed start)
    GameRan,    // load now: the game has made a ProcessEvent call
    TimedOut,   // load now: no call from the game within kUe4ssLoadWaitMs
};

struct Ue4ssLoadWait {
    bool waitOn = true;       // ue4ss_wait (default 1)
    bool hookIn = false;      // HSMMod's ProcessEvent hook is installed, so it sees the game's first call
    bool gameRan = false;     // that hook has seen a call (before UE4SS loads, every call is the game's)
    uint32_t waitedMs = 0;    // since the wait began
};

inline Ue4ssLoadGate Ue4ssLoadGateOf(const Ue4ssLoadWait& w) {
    if (!w.waitOn || !w.hookIn) return Ue4ssLoadGate::NoWait;
    if (w.gameRan) return Ue4ssLoadGate::GameRan;
    if (w.waitedMs >= kUe4ssLoadWaitMs) return Ue4ssLoadGate::TimedOut;
    return Ue4ssLoadGate::Wait;
}

inline const char* Ue4ssLoadGateText(Ue4ssLoadGate g) {
    switch (g) {
        case Ue4ssLoadGate::Wait: return "waiting";
        case Ue4ssLoadGate::NoWait: return "no wait";
        case Ue4ssLoadGate::GameRan: return "the game made its first ProcessEvent call";
        case Ue4ssLoadGate::TimedOut: return "no ProcessEvent call from the game in time";
    }
    return "?";
}

}  // namespace hsm
