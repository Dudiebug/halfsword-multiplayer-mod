// Window-level input injection for the agent bridge: posts real Windows keyboard/mouse messages to the game's
// own top-level window (the same messages Unreal's window procedure receives from a user). Thread-safe; runs on
// the bridge thread, so it works on screens where no Blueprint ticks yet.
#pragma once
#include "agent/hsm_agent.h"
#include <string>

namespace hsm::agent {
// Handles "key <name> [tap|down|up] [send]", "click <x> <y> [right]" (client pixels), "window".
// Returns false when the verb is not one of these.
bool HandleInput(const AgentCommand& c, std::string& reply);
}
