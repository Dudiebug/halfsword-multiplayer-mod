// Crossbow loading and shooting in owner sync (protocol rev 43, PaintFx kind Crossbow).
//
// The game loads and fires a crossbow through Blueprint events on the crossbow itself (BP_Weapon_Ranged_Weapon_Crossbow_
// Light): Event Load String (the string goes on the peg, String Loaded), Event Load Bolt (a bolt is seated on the rails,
// Bolt Loaded) and Event Shoot (both flags drop, the bolt is let go and pushed). Only the owner's input or AI ever calls
// them, and a stand-in has neither, so nobody else saw the string drawn, the bolt go in or the bolt leave. The owner now
// watches its own fighters' crossbows for those two flags and reports each change; every other machine replays the same
// events on its stand-in's crossbow. The replayed bolt is cosmetic: it belongs to the stand-in, so it deals no damage
// there (the owner's own bolt still makes the hit claim) and it is never announced as an object.
//
// Pure rules here: which flag change is which action (owner side), and which events a stand-in needs to show it
// (receiver side, which also catches up a stand-in that missed a step). Game-independent.
#pragma once
#include <cstdint>
#include <optional>
#include <string_view>
#include <vector>

namespace hsm::combat {

enum class CrossbowAction : uint8_t { String = 1, Bolt = 2, Shoot = 3, ShootEmpty = 4 };
constexpr uint8_t kCrossbowActionMax = uint8_t(CrossbowAction::ShootEmpty);

// The hand a crossbow report names: the bone of the hand that holds it.
inline bool IsCrossbowHand(std::string_view bone) { return bone == "hand_r" || bone == "hand_l"; }

struct CrossbowState {
    bool string = false;   // String Loaded
    bool bolt = false;     // Bolt Loaded
};

// The action the owner reports for a change of its crossbow's flags, or none. The string going slack is the shot (Event
// Shoot is the only thing that unloads it); it carried a bolt when Bolt Loaded was set just before. A bolt lost without a
// shot (Bolt Loaded dropping while the string stays drawn) has no event to replay, so it is not reported.
inline std::optional<CrossbowAction> CrossbowEdge(const CrossbowState& prev, const CrossbowState& now) {
    if (prev.string && !now.string) return prev.bolt ? CrossbowAction::Shoot : CrossbowAction::ShootEmpty;
    if (!prev.bolt && now.bolt) return CrossbowAction::Bolt;   // also covers string and bolt in the same frame
    if (!prev.string && now.string) return CrossbowAction::String;
    return std::nullopt;
}

enum class CrossbowStep : uint8_t { LoadString, LoadBolt, Shoot };

// The events to run on the stand-in's crossbow (its state as read now) so it shows the owner's action. Missing steps are
// filled in first: a bolt needs a drawn string, a shot needs the string (and, for a loaded shot, the bolt).
inline std::vector<CrossbowStep> PlanCrossbowReplay(const CrossbowState& standIn, CrossbowAction action) {
    std::vector<CrossbowStep> s;
    switch (action) {
        case CrossbowAction::String:
            if (!standIn.string) s.push_back(CrossbowStep::LoadString);
            break;
        case CrossbowAction::Bolt:
            if (standIn.bolt) break;
            if (!standIn.string) s.push_back(CrossbowStep::LoadString);
            s.push_back(CrossbowStep::LoadBolt);
            break;
        case CrossbowAction::Shoot:
            if (!standIn.string) s.push_back(CrossbowStep::LoadString);
            if (!standIn.bolt) s.push_back(CrossbowStep::LoadBolt);
            s.push_back(CrossbowStep::Shoot);
            break;
        case CrossbowAction::ShootEmpty:
            if (!standIn.string) s.push_back(CrossbowStep::LoadString);
            s.push_back(CrossbowStep::Shoot);
            break;
    }
    return s;
}

inline const char* CrossbowActionName(CrossbowAction a) {
    switch (a) {
        case CrossbowAction::String: return "string";
        case CrossbowAction::Bolt: return "bolt";
        case CrossbowAction::Shoot: return "shoot";
        case CrossbowAction::ShootEmpty: return "shoot-empty";
    }
    return "?";
}

}  // namespace hsm::combat
