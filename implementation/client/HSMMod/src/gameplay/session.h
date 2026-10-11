// What the UI needs from the running session.
#pragma once
#include "gameplay/lobby.h"

namespace hsm {

class Session {
public:
    virtual ~Session() = default;
    virtual game::Lobby& lobby() = 0;
    virtual bool steamMode() const = 0;
    virtual void StartOnMap(const std::string& path) = 0;   // host: open a fight level; the lobby follows
};

}  // namespace hsm
