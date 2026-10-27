// M6: a Steam "join friend" launch carries "+connect_lobby <id>" on the command line. The -hsm* overrides (role, host,
// port, name, ...) must not be honoured on such a launch: a link or a lobby invite could otherwise pick the host
// address or turn the mod off. The ini is unaffected.
#pragma once
#include <cstddef>
#include <string_view>

namespace hsm {

// True when the command line has the token "+connect_lobby" (at the start or after whitespace; the token ends at
// whitespace or the end), case-insensitive.
inline bool CommandLineIgnoresHsmFlags(std::string_view cmdline) {
    constexpr std::string_view tok = "+connect_lobby";
    for (size_t i = 0; i + tok.size() <= cmdline.size(); ++i) {
        if (i && cmdline[i - 1] != ' ' && cmdline[i - 1] != '\t') continue;
        size_t k = 0;
        for (; k < tok.size(); ++k) {
            char c = cmdline[i + k];
            if (c >= 'A' && c <= 'Z') c = char(c - 'A' + 'a');
            if (c != tok[k]) break;
        }
        if (k != tok.size()) continue;
        const size_t e = i + tok.size();
        if (e == cmdline.size() || cmdline[e] == ' ' || cmdline[e] == '\t') return true;
    }
    return false;
}

}  // namespace hsm
