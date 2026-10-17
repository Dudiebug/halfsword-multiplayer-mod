#pragma once
#include <string_view>

namespace hsm {
// The reflected identifiers used by this mod are ASCII. FName identity ignores case.
inline bool ReflectedNameEqual(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    auto fold = [](unsigned char c) { return c >= 'A' && c <= 'Z' ? c + ('a' - 'A') : int(c); };
    for (size_t i = 0; i < a.size(); ++i)
        if (fold(static_cast<unsigned char>(a[i])) != fold(static_cast<unsigned char>(b[i]))) return false;
    return true;
}
}  // namespace hsm
