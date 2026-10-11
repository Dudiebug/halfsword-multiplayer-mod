// Local agent bridge: sensors and actuators for automated testing, enabled only by -hsmbridge=<port>.
// Listens on 127.0.0.1 only, one connection at a time; each text line is one command, answered with one JSON
// line. Commands run on the game thread (Tick), never on the socket thread.
#pragma once
#include "agent/hsm_agent.h"
#include <functional>
#include <string>

namespace hsm::agent {

using Handler = std::function<std::string(const AgentCommand&)>;   // returns a JSON object

bool Start(uint16_t port);          // false if disabled or the port cannot be bound
void Tick(const Handler& handler);  // game thread: answer queued commands
bool Enabled();

}  // namespace hsm::agent
