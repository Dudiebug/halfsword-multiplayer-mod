// Setting one key of an ini file's text without touching the rest (comments, other sections, other keys, line endings).
// Game-independent: the caller reads and writes the file.
#pragma once
#include <string>
#include <string_view>

namespace hsm::util {

namespace ini_detail {
inline std::string_view Trim(std::string_view s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.remove_prefix(1);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r')) s.remove_suffix(1);
    return s;
}
inline bool SameAscii(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        char x = a[i], y = b[i];
        if (x >= 'A' && x <= 'Z') x = char(x - 'A' + 'a');
        if (y >= 'A' && y <= 'Z') y = char(y - 'A' + 'a');
        if (x != y) return false;
    }
    return true;
}
}  // namespace ini_detail

// Returns `text` with `key=value` under `[section]`: the key's first line there is replaced (section and key names
// compare without case, as GetPrivateProfileString does), else the line is added at the end of that section, else
// the section is added at the end of the file. Line endings follow the file's (CRLF when it has any). Key and value must
// not hold a line break or '='/']' in the key (nothing is written then: the text comes back unchanged).
inline std::string SetIniValue(const std::string& text, std::string_view section, std::string_view key, std::string_view value) {
    using namespace ini_detail;
    if (key.empty() || key.find_first_of("\r\n=") != std::string_view::npos || section.empty() || section.find_first_of("\r\n]") != std::string_view::npos ||
        value.find_first_of("\r\n") != std::string_view::npos)
        return text;
    const std::string eol = text.find("\r\n") != std::string::npos ? "\r\n" : "\n";
    const std::string entry = std::string(key) + "=" + std::string(value);
    std::string out;
    out.reserve(text.size() + entry.size() + 16);
    bool inSection = false, done = false, sawSection = false;
    size_t insertAt = std::string::npos;   // where in `out` a new line goes: after the section's header or last key line
    for (size_t pos = 0; pos < text.size();) {
        const size_t nl = text.find('\n', pos);
        const size_t next = nl == std::string::npos ? text.size() : nl + 1;
        const std::string_view line(text.data() + pos, (nl == std::string::npos ? text.size() : nl) - pos);
        const std::string_view t = Trim(line);
        bool header = false, keyLine = false, replaced = false;
        if (!t.empty() && t.front() == '[') {
            const size_t close = t.find(']');
            inSection = close != std::string_view::npos && SameAscii(Trim(t.substr(1, close - 1)), section);
            sawSection = sawSection || inSection;
            header = inSection;
        } else if (inSection && !t.empty() && t.front() != ';' && t.front() != '#') {
            keyLine = true;
            const size_t eq = t.find('=');
            if (!done && eq != std::string_view::npos && SameAscii(Trim(t.substr(0, eq)), key)) {
                out += entry;
                if (!line.empty() && line.back() == '\r') out += '\r';
                if (nl != std::string::npos) out += '\n';
                done = replaced = true;
            }
        }
        if (!replaced) out.append(text, pos, next - pos);
        if (!done && (header || keyLine)) insertAt = out.size();
        pos = next;
    }
    if (done) return out;
    if (sawSection) {
        std::string add = entry + eol;
        if (insertAt == std::string::npos) insertAt = out.size();
        if (insertAt > 0 && out[insertAt - 1] != '\n') add = eol + add;   // the file ends without a line break
        out.insert(insertAt, add);
        return out;
    }
    if (!out.empty() && out.back() != '\n') out += eol;
    out += "[" + std::string(section) + "]" + eol + entry + eol;
    return out;
}

}  // namespace hsm::util
