// Client side: which Travel messages a client follows (H2) and which setting values it applies (L8).
//
// A Travel carries a level name and an OpenLevel options string, both from the host and both passed to the engine. A
// modified host could send "listen" (the client starts a server), "?game=..." (another game mode class), "1.2.3.4:7777"
// (a different server), "//share/x" (a UNC path) or a path to a map the game does not ship as a playable level. The
// catalog (match/hsm_maps.h) plus the menu maps are the only levels a client follows; options must be empty (the host's
// own travels, HostTravel and the game's OpenLevel, never carry any).
#pragma once
#include "match/hsm_maps.h"
#include "protocol/hsm_protocol.h"
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace hsm {

inline constexpr const char* kTravelMenuLevel = "/Game/Maps/Menus/Map_Menu_Startup";
// Option keys a travel may carry (before any '='); none yet. Add a key here only with a log line showing the game sends it.
inline constexpr const char* kAllowedOptionKeys[] = {nullptr};

namespace travel_detail {
inline bool NameChar(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-';
}
// "/Game/Maps/Menus/Map_Menu_<name>" with an optional ".<name>" object suffix: any menu map of the game.
inline bool MenuLevel(std::string_view s) {
    constexpr std::string_view prefix = "/Game/Maps/Menus/Map_Menu_";
    if (s.size() <= prefix.size() || !map_detail::EqualsNoCase(s.substr(0, prefix.size()), prefix)) return false;
    s.remove_prefix(prefix.size());
    size_t i = 0;
    while (i < s.size() && NameChar(s[i])) ++i;
    if (i == 0) return false;
    if (i == s.size()) return true;
    if (s[i] != '.') return false;
    for (size_t j = i + 1; j < s.size(); ++j) if (!NameChar(s[j])) return false;
    return s.size() > i + 1;
}
}  // namespace travel_detail

// A host-sent name for the log: printable ASCII only, at most 64 characters.
inline std::string TravelLogText(std::string_view s) {
    std::string out;
    for (size_t i = 0; i < s.size() && i < 64; ++i) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        out += (c >= 0x20 && c < 0x7F) ? char(c) : '?';
    }
    return out;
}

inline bool TravelLevelAllowed(std::string_view level) {
    if (level.empty() || level.size() > kMaxLevelName) return false;
    for (char c : level) {
        const unsigned char u = static_cast<unsigned char>(c);
        if (u < 0x20 || u >= 0x7F || c == ':' || c == '?' || c == '\\' || c == ' ') return false;
    }
    if (level.find("//") != std::string_view::npos || level.find("..") != std::string_view::npos) return false;
    return FindMap(level) != nullptr || travel_detail::MenuLevel(level);
}

// Empty only, or every '?'/'&'-separated key (text before '=') on kAllowedOptionKeys and no control characters.
inline bool TravelOptionsAllowed(std::string_view options) {
    if (options.empty()) return true;
    if (options.size() > kMaxLevelName) return false;
    for (char c : options) {
        const unsigned char u = static_cast<unsigned char>(c);
        if (u < 0x20 || u >= 0x7F) return false;
    }
    if (options.find(':') != std::string_view::npos || options.find("//") != std::string_view::npos || options.find('\\') != std::string_view::npos) return false;
    size_t pos = 0;
    while (pos <= options.size()) {
        size_t end = options.find_first_of("?&", pos);
        if (end == std::string_view::npos) end = options.size();
        std::string_view part = options.substr(pos, end - pos);
        pos = end + 1;
        if (part.empty()) continue;
        const size_t eq = part.find('=');
        const std::string_view key = part.substr(0, eq);
        bool ok = false;
        for (const char* k : kAllowedOptionKeys) if (k && map_detail::EqualsNoCase(key, k)) ok = true;
        if (!ok) return false;
    }
    return true;
}

inline bool TravelAllowed(std::string_view level, std::string_view options) {
    return TravelLevelAllowed(level) && TravelOptionsAllowed(options);
}

// L8: a host-sent GI setting value, by the property type it is applied to (bytes are the raw field).
enum class SettingKind : uint8_t { Bool, Byte, Int, Float, Double };
constexpr uint8_t kMaxEnumByte = 64;   // a conservative cap on the game's enums (Game/Play/Combat Mode); tighten from Enum_* if known
inline bool SettingValueOk(SettingKind kind, const uint8_t* bytes, size_t size) {
    switch (kind) {
    case SettingKind::Bool: return size == 1 && bytes[0] <= 1;
    case SettingKind::Byte: return size == 1 && bytes[0] <= kMaxEnumByte;
    case SettingKind::Int: return size == 4;
    case SettingKind::Float: {
        if (size != 4) return false;
        uint32_t u = uint32_t(bytes[0]) | uint32_t(bytes[1]) << 8 | uint32_t(bytes[2]) << 16 | uint32_t(bytes[3]) << 24;
        const uint32_t exp = (u >> 23) & 0xFF;
        return exp != 0xFF;   // not NaN, not Inf
    }
    case SettingKind::Double: {
        if (size != 8) return false;
        uint64_t u = 0;
        for (int i = 0; i < 8; ++i) u |= uint64_t(bytes[i]) << (8 * i);
        return ((u >> 52) & 0x7FF) != 0x7FF;
    }
    }
    return false;
}

}  // namespace hsm
