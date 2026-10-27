#pragma once
#include <string>
#include <string_view>

namespace hsm::util {

// Strip CR/LF and control characters (0x00-0x1F, 0x7F) from a string.
// Returns a new string with the characters removed.
inline std::string StripControlChars(std::string_view input) {
    std::string output;
    output.reserve(input.size());
    for (unsigned char c : input) {
        // Keep printable ASCII and extended ASCII (0x80-0xFF)
        if (c >= 0x20 && c != 0x7F) {
            output += c;
        }
    }
    return output;
}

}  // namespace hsm::util
