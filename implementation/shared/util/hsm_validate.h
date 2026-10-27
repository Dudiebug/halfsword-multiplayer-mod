#pragma once
#include <cctype>
#include <cstddef>
#include <string_view>

namespace hsm::util {

// Validate a bone name string before converting to FName.
// Rules: length <= 64 bytes, only [A-Za-z0-9_] characters.
// Returns true if valid, false otherwise.
inline bool ValidBoneName(std::string_view name) {
    if (name.empty() || name.size() > 64) {
        return false;
    }
    for (char c : name) {
        if (!std::isalnum(static_cast<unsigned char>(c)) && c != '_') {
            return false;
        }
    }
    return true;
}

}  // namespace hsm::util
