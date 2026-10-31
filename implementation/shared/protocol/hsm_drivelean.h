// Lean driving of a remote fighter (0.8.0-pre19, [net] drive_lean). Pure logic, unit-tested.
//
// DriveFighter plays another player's input on a Willie here: up to ten axis events through ProcessEvent into the
// Blueprint handlers, then the fighter's control rotation. These decide which of that the game can do without. Each rule
// is proven against the decompiled Willie_BP.cpp (Character/Blueprints); what could not be proven is left alone:
//  - Move axes (11404, 11421): AddMovementInput every call, so a non-zero value is fed every frame.
//  - Arm axes (11290, 11450): FInterpTo of Alt Thrust, which the tick zeroes again while no alt thrust runs (17223),
//    so a held trigger is not idempotent. Only the quiet rule (hsm_input.h, value 0) applies.
//  - Look axes: each of the four runs Update Aim (22657), which adds the mouse value to the aim vector and chases it at
//    a capped speed per call, and decays it per call. Feeding fewer of them changes how fast the arms aim.
#pragma once
#include "hsm_protocol.h"
#include <array>

namespace hsm {

// The guard axes (a mouse button is mapped to each: LeftMouseButton to Right Guard, RightMouseButton to Left Guard).
// Right Guard Axis (Willie_BP.cpp:11466-11533) stores its value, then: >= 0.5 runs the press branch once, a DoOnce
// (Temp_bool_IsClosed_Variable_47 closes at its start, the release branch reopens it); every later call returns at
// once. Below 0.5 the release branch runs once the same way (IsClosed_50). Left Guard Axis (11300-11393) is the same
// with its own gates (51/53/55). The stored values (K2Node_InputAxisEvent_AxisValue_8 and _9) and the gate variables
// are read nowhere else in Willie_BP. So feeding a guard axis again on the same side of 0.5 as the last value fed
// changes nothing: the call is skipped.
constexpr float kGuardPressAt = 0.5f;
inline bool IsGuardAxis(Axis a) { return a == Axis::RightGuard || a == Axis::LeftGuard; }

class LeanFeed {
public:
    // The fighter the axes go to; a new one has run no handler yet (its gates are open).
    void Target(const void* w) { if (w != target_) { target_ = w; fed_.fill(false); } }
    // skip: the quiet marks of InputApplier::Tick (axes[i] is 0 where set: the game holds 0 there). Adds the guard
    // axes that need no call; returns how many it added. Call once per frame per fighter, after Target.
    unsigned Trim(const std::array<float, kAxisCount>& axes, std::array<bool, kAxisCount>& skip) {
        unsigned added = 0;
        for (size_t i = 0; i < kAxisCount; ++i) {
            const bool down = axes[i] >= kGuardPressAt;
            if (skip[i]) { last_[i] = false; fed_[i] = true; continue; }   // quiet means a 0 was fed before
            if (IsGuardAxis(Axis(i)) && fed_[i] && down == last_[i]) { skip[i] = true; ++added; continue; }
            last_[i] = down; fed_[i] = true;
        }
        return added;
    }
private:
    const void* target_ = nullptr;
    std::array<bool, kAxisCount> fed_{};    // an event has gone to the game since the last Target
    std::array<bool, kAxisCount> last_{};   // the side of kGuardPressAt of the last value the game got
};

// A remote fighter's control rotation (SetControlRotation, aim from the remote player): the game writes it back
// unchanged each tick (Willie_BP.cpp:16529, Player and no lock-on: Current Control Rotation, which is the controller's
// own), and AddControllerYawInput does nothing to an AI controller. So once set it stays; it is set again only when it
// changed, the controller is another one, or kRefreshMs have passed (a heartbeat against anything unseen).
class ControlRotationGate {
public:
    static constexpr double kRefreshMs = 500.0;
    bool Needed(const void* ctrl, float yawDeg, float pitchDeg, double nowMs) {
        const bool same = ctrl == ctrl_ && yawDeg == yaw_ && pitchDeg == pitch_ && nowMs >= atMs_ && nowMs - atMs_ < kRefreshMs;
        if (same) return false;
        ctrl_ = ctrl; yaw_ = yawDeg; pitch_ = pitchDeg; atMs_ = nowMs;
        return true;
    }
private:
    const void* ctrl_ = nullptr;
    float yaw_ = 0, pitch_ = 0;
    double atMs_ = 0;
};

}  // namespace hsm
