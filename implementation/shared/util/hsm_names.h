// Reflected-name comparison. Pure, no engine headers.
//
// Unreal compares FNames case-insensitively, and a shipping build keeps one spelling per name: the display string of
// every FName equal to "Bone" is whichever casing was registered first in the process (a "bone" socket registered
// before the Blueprint parameter makes the parameter display as "bone"). Looking a reflected name up by its display
// string must therefore ignore ASCII case, as the engine does, or a parameter that exists is reported missing.
#pragma once
#include <cstddef>
#include <string_view>

namespace hsm {

// Equal under ASCII case folding ('A'-'Z' only; every other byte compares exactly).
constexpr char FoldAscii(char c) { return c >= 'A' && c <= 'Z' ? char(c - 'A' + 'a') : c; }

inline bool SameNameAscii(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i)
        if (FoldAscii(a[i]) != FoldAscii(b[i])) return false;
    return true;
}

// s starts with prefix under the same folding.
inline bool StartsWithNameAscii(std::string_view s, std::string_view prefix) {
    return s.size() >= prefix.size() && SameNameAscii(s.substr(0, prefix.size()), prefix);
}

}  // namespace hsm
