// In-game bug reports: posts to the report relay (report_url) over HTTPS on a worker thread. WinHTTP is
// loaded from System32 on the first report, so the DLL imports nothing new and a player who never types /report
// never loads it. Payloads and the log redaction are in session/hsm_report.h.
#pragma once
#include "session/hsm_report.h"
#include <optional>
#include <string>

namespace hsm::report {
struct Result {
    enum class Kind : uint8_t { Filed, Attached, Failed } kind = Kind::Failed;
    std::string id, token, issue;   // Filed: the relay's answer
    std::string error;              // Failed: a short reason for the chat
};

// url: https://host[:port][/base] (http only for 127.0.0.1, for testing). An empty or bad url turns reports off.
void Configure(bool enabled, const std::string& url);
bool Enabled();
bool Busy();   // one request at a time
// Each reads this instance's HSMMod.log and HSMMod.prev.log on the worker, sends their tails redacted with a fresh
// salt, and queues the Result. false: off or busy.
bool Submit(const std::string& description, const ReportFacts& facts);
bool Attach(const std::string& id, const std::string& token, const ReportFacts& facts);
std::optional<Result> Poll();   // game thread
// "Windows 10.0.26100" or "Wine 9.0 (Windows 10.0.19045)".
std::string OsName();
}  // namespace hsm::report
