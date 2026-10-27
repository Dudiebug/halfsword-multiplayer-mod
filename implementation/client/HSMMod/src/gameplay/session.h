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
    virtual std::string SubmitReport(const std::string& text) = 0;   // a bug report (same checks as /bug); the line to show
    virtual std::string ReportStatus() const = 0;                    // the last report's outcome line, as told to the player
    virtual uint32_t ReportStatusSeq() const = 0;                    // changes with every new outcome (0: none yet)
    virtual void LobbyChatLine(const std::string& line) = 0;         // typed in the lobby chat box: /bug, /sendlog and /nolog work as in the match chat, else it goes to every member
    virtual std::string TakeNotice() = 0;                            // why the host turned us away (kicked, banned, full...), once; empty: nothing new
    virtual bool HelpImprove() const = 0;                            // help_improve in HSMMod.ini: also send the log when someone in the lobby files a report
    virtual void SetHelpImprove(bool on) = 0;                        // also written back to HSMMod.ini
};

}  // namespace hsm
