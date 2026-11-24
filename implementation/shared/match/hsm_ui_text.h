// The rules every line of player text follows (docs/UI_STYLE.md), as code the tests can hold to: the length of a hint and the
// wording of a count with its unit. Pure; the scenes (hsm_lobby_scene.h, hsm_mp_scene.h, ...) call it, ui/*.cpp only draws.
#pragma once
#include <cstddef>
#include <string>
#include <string_view>

namespace hsm {

// A hint is one short line under a row, a card or a box. Longer than this it no longer fits beside the row on screen, and it
// says more than the label and the value already do. A hint of several lines (separated by '\n') is held to it line by line.
inline constexpr size_t kHintMax = 120;

// Characters on a line, not bytes: "\xC2\xB7" (the middle dot) is one.
inline size_t TextChars(std::string_view s) {
    size_t n = 0;
    for (char c : s) if ((static_cast<unsigned char>(c) & 0xC0) != 0x80) ++n;
    return n;
}

// Whether every line of a hint is within kHintMax characters. Empty fits (a row with no hint).
inline bool HintFits(std::string_view hint) {
    size_t from = 0;
    for (;;) {
        const size_t nl = hint.find('\n', from);
        const std::string_view line = hint.substr(from, nl == std::string_view::npos ? std::string_view::npos : nl - from);
        if (TextChars(line) > kHintMax) return false;
        if (nl == std::string_view::npos) return true;
        from = nl + 1;
    }
}

// "1 round", "3 rounds": a count with its unit, the unit in the singular for exactly one.
inline std::string CountOf(int n, std::string_view one, std::string_view many) {
    return std::to_string(n) + " " + std::string(n == 1 ? one : many);
}

}  // namespace hsm
