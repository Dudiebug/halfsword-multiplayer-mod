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
    virtual void StartProgression() = 0;                    // host: the stock Progression hub, Free Mode off
    virtual std::string SubmitReport(const std::string& text) = 0;   // a bug report (same checks as /report); the line to show
};

}  // namespace hsm
