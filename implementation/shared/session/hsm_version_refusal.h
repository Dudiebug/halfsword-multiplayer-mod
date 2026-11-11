// Who has to update when a host refuses another HSMMod version (#3073).
//
// The refusal used to say only "The host runs another HSMMod version", so a player on pre48 could not tell that the
// host was the one on an old pre46. The host now sends its own version ("0.8.0-pre46") as the Reject detail, and the
// lobby list has it in hsm_ver; this reads the two versions (session/hsm_version.h) and says who is behind.
#pragma once
#include "session/hsm_version.h"
#include <cstdint>
#include <string>

namespace hsm {

// kModVersion of a test build ends in "-test" ("0.8.0-pre49-test"): the same version for this purpose.
inline std::optional<Version> ParseModVersion(const std::string& s) {
    static const std::string kTest = "-test";
    if (s.size() > kTest.size() && s.compare(s.size() - kTest.size(), kTest.size(), kTest) == 0)
        return ParseVersion(s.substr(0, s.size() - kTest.size()));
    return ParseVersion(s);
}

enum class Behind : uint8_t { Unknown, Host, Me };

inline Behind WhoIsBehind(const std::string& host, const std::string& mine) {
    const auto h = ParseModVersion(host), m = ParseModVersion(mine);
    if (!h || !m) return Behind::Unknown;
    const int c = CompareVersion(*h, *m);
    return c < 0 ? Behind::Host : c > 0 ? Behind::Me : Behind::Unknown;
}

// The line a refused player sees. `host` is the Reject detail (empty from a host before this build).
inline std::string VersionRefusalText(const std::string& host, const std::string& mine) {
    switch (WhoIsBehind(host, mine)) {
        case Behind::Host: return "The host is on " + host + " and you are on " + mine + ". Ask the host to update HSMMod.";
        case Behind::Me: return "The host is on " + host + " and you are on " + mine + ". Update HSMMod to join.";
        case Behind::Unknown: break;
    }
    return "The host runs another HSMMod version; everyone needs the same one.";
}

// The lobby list's reason for a lobby on another version.
inline std::string VersionLobbyReason(const std::string& host, const std::string& mine) {
    switch (WhoIsBehind(host, mine)) {
        case Behind::Host: return "host on older " + host + " (you " + mine + ")";
        case Behind::Me: return "host on newer " + host + ": update HSMMod";
        case Behind::Unknown: break;
    }
    return "different mod version (host " + host + ", you " + mine + ")";
}

}  // namespace hsm
