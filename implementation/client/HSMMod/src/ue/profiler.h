// Blueprint profiler: times every game-thread ProcessEvent call and every Blueprint function body the script VM runs
// (ue/hook.cpp and ue/script_hook.cpp bracket them with a Scope), plus HSMMod's own hooks and subsystems, over a
// capture of up to a few minutes. Stopping writes HSMProfile-<date>-<time>.txt (the functions by self and inclusive
// time, the frame times) and a .json timeline (Chrome trace format: ui.perfetto.dev, chrome://tracing, speedscope)
// next to the game exe. Start and stop with Ctrl+Shift+P in game or the agent command `prof`. When no capture runs,
// a Scope costs one relaxed atomic load. Core logic: implementation/shared/util/hsm_profile.h.
#pragma once
#include "util/hsm_profile.h"
#include <atomic>
#include <string>
#include <vector>

namespace hsm::ue::prof {
using hsm::prof::Kind;

extern std::atomic<bool> g_recording;
inline bool Recording() { return g_recording.load(std::memory_order_relaxed); }

bool BeginCall(const void* key, Kind kind);   // game thread check inside; false = nothing pushed
void EndCall();

// One call on the profiler's stack while a capture runs (game thread only; other threads pass through). key is a
// UFunction* for kEvent, kScript and kHook, a string literal (its own name) for kMod.
struct Scope {
    bool on;
    Scope(const void* key, Kind kind) : on(key && Recording() && BeginCall(key, kind)) {}
    ~Scope() { if (on) EndCall(); }
    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;
};

// Once per game frame on the game thread (the game mode's tick): the frame mark, and the capture's time limit.
// Returns a message for the player when the capture stopped (empty otherwise).
std::string Frame();
// Ctrl+Shift+P while the game window is foreground toggles a capture (call every frame, game thread). Returns a
// message for the player when it started or stopped one (empty otherwise).
std::string PollHotkey();
// Ends a running capture (writing its files) and returns the player's message; empty when none runs. A level change
// calls it: the old level's functions are freed, and a new one could be counted under a freed one's name.
std::string StopCapture();
// Agent command: `prof start [seconds] [timeline_min_us]` | `prof stop` | `prof status`. JSON object. Game thread.
std::string Command(const std::vector<std::string>& args);

}  // namespace hsm::ue::prof
