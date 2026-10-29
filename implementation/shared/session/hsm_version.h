// The update check's logic (pure, no engine or network types). The mod asks GitHub's releases list once at startup;
// this header reads the tags ("v0.7.0-pre2", "v0.6.5"), orders them, and picks the release to offer. A player on a
// prerelease is offered prereleases and full releases, a player on a full release only full releases. The JSON scan is
// a small bounded reader for exactly what the list carries (tag_name, prerelease, draft, html_url of each release), not
// a general parser: whatever it does not understand is skipped, and a damaged answer fails instead of guessing.
#pragma once
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace hsm {

// MAJOR.MINOR.PATCH with an optional -preN. pre == 0 is the full release, which is newer than every -preN of the same
// number (0.7.0-pre2 < 0.7.0-pre3 < 0.7.0 < 0.7.1-pre1).
struct Version {
    int major = 0, minor = 0, patch = 0, pre = 0;
    bool prerelease() const { return pre > 0; }
};

inline int CompareVersion(const Version& a, const Version& b) {
    const int x[4] = {a.major, a.minor, a.patch, 0}, y[4] = {b.major, b.minor, b.patch, 0};
    for (int i = 0; i < 3; ++i)
        if (x[i] != y[i]) return x[i] < y[i] ? -1 : 1;
    const int pa = a.pre == 0 ? INT32_MAX : a.pre, pb = b.pre == 0 ? INT32_MAX : b.pre;
    return pa < pb ? -1 : pa > pb ? 1 : 0;
}

inline std::string FormatVersion(const Version& v) {
    std::string s = std::to_string(v.major) + "." + std::to_string(v.minor) + "." + std::to_string(v.patch);
    return v.pre ? s + "-pre" + std::to_string(v.pre) : s;
}

namespace version_detail {
// Up to 6 digits (a longer or empty run is no number); advances i past them.
inline bool Number(const std::string& s, size_t& i, int& out) {
    size_t n = 0;
    int v = 0;
    while (i < s.size() && s[i] >= '0' && s[i] <= '9') {
        if (++n > 6) return false;
        v = v * 10 + (s[i++] - '0');
    }
    out = v;
    return n > 0;
}
}  // namespace version_detail

// The whole string: an optional "v", MAJOR.MINOR.PATCH, an optional "-preN" (N >= 1). Anything else (-rc1, a fourth
// number, trailing text) does not parse.
inline std::optional<Version> ParseVersion(const std::string& s) {
    using version_detail::Number;
    size_t i = !s.empty() && s[0] == 'v' ? 1 : 0;
    Version v;
    if (!Number(s, i, v.major) || i >= s.size() || s[i++] != '.') return std::nullopt;
    if (!Number(s, i, v.minor) || i >= s.size() || s[i++] != '.') return std::nullopt;
    if (!Number(s, i, v.patch)) return std::nullopt;
    if (i == s.size()) return v;
    if (s.compare(i, 4, "-pre") != 0) return std::nullopt;
    i += 4;
    if (!Number(s, i, v.pre) || v.pre < 1 || i != s.size()) return std::nullopt;
    return v;
}

// A tag as "git describe --tags" prints it: "v0.7.0-pre2", or "v0.7.0-pre2-3-gabc123" (3 commits later), each with an
// optional "-dirty".
inline std::optional<Version> ParseDescribedVersion(std::string s) {
    if (s.size() > 6 && s.compare(s.size() - 6, 6, "-dirty") == 0) s.resize(s.size() - 6);
    if (std::optional<Version> v = ParseVersion(s)) return v;
    size_t g = s.rfind("-g");
    if (g == std::string::npos || g + 2 >= s.size()) return std::nullopt;
    for (size_t i = g + 2; i < s.size(); ++i)
        if (!((s[i] >= '0' && s[i] <= '9') || (s[i] >= 'a' && s[i] <= 'f'))) return std::nullopt;
    if (g == 0) return std::nullopt;
    size_t dash = s.rfind('-', g - 1);
    if (dash == std::string::npos || dash + 1 >= g) return std::nullopt;
    for (size_t i = dash + 1; i < g; ++i)
        if (s[i] < '0' || s[i] > '9') return std::nullopt;
    return ParseVersion(s.substr(0, dash));
}

// The version this build runs: kModVersion ("0.8.0-pre3", "0.7.3"), which is bumped with every release tag. The
// build's git tag is not consulted: a release is built before its own tag exists, so the nearest tag is the previous
// prerelease (0.8.0-pre3 built on top of v0.8.0-pre2 called itself pre2 and offered pre3 to its own players, #55-#57).
inline std::optional<Version> RunningVersion(const std::string& modVersion) { return ParseVersion(modVersion); }

// One release object of GitHub's list.
struct ReleaseInfo {
    std::string tag, url;   // url: https://github.com/... only, else empty
    bool prerelease = false, draft = false;
};

constexpr size_t kMaxReleasesJson = 256 * 1024;   // the caller reads no more than this; the scan refuses more
constexpr size_t kMaxReleaseTag = 64;
constexpr size_t kMaxReleaseUrl = 200;

// A link that is safe to log and show: https://github.com/..., printable ASCII without spaces or quotes.
inline bool ValidReleaseUrl(const std::string& u) {
    static const char kPrefix[] = "https://github.com/";
    if (u.size() <= sizeof kPrefix - 1 || u.size() > kMaxReleaseUrl || u.compare(0, sizeof kPrefix - 1, kPrefix) != 0) return false;
    for (char c : u)
        if (c <= 0x20 || c >= 0x7F || c == '"' || c == '\\' || c == '<' || c == '>') return false;
    return true;
}

struct ReleaseList {
    std::vector<ReleaseInfo> releases;   // every release read in full, in the list's order
    bool complete = false;               // false: cut off, malformed or not a list; releases holds what came before
};

namespace version_detail {
constexpr size_t kMaxDepth = 16;

// The string starting at s[i] == '"'; i ends just past the closing quote. false: unterminated or bad escape. \uXXXX
// below 0x80 is that character, anything else becomes '?'. Text past `cap` is dropped and flagged.
inline bool ReadString(const std::string& s, size_t& i, size_t cap, std::string& out, bool& tooLong) {
    out.clear();
    tooLong = false;
    for (++i; i < s.size();) {
        char c = s[i++];
        if (c == '"') return true;
        if (static_cast<unsigned char>(c) < 0x20) return false;
        if (c == '\\') {
            if (i >= s.size()) return false;
            char e = s[i++];
            switch (e) {
                case '"': case '\\': case '/': c = e; break;
                case 'b': c = '\b'; break;
                case 'f': c = '\f'; break;
                case 'n': c = '\n'; break;
                case 'r': c = '\r'; break;
                case 't': c = '\t'; break;
                case 'u': {
                    if (i + 4 > s.size()) return false;
                    unsigned code = 0;
                    for (int k = 0; k < 4; ++k) {
                        char h = s[i++];
                        unsigned d = h >= '0' && h <= '9' ? unsigned(h - '0') : h >= 'a' && h <= 'f' ? unsigned(h - 'a' + 10) : h >= 'A' && h <= 'F' ? unsigned(h - 'A' + 10) : 16u;
                        if (d == 16u) return false;
                        code = code * 16 + d;
                    }
                    c = code < 0x80 ? char(code) : '?';
                    break;
                }
                default: return false;
            }
        }
        if (out.size() < cap) out.push_back(c); else tooLong = true;
    }
    return false;
}

inline void SkipSpace(const std::string& s, size_t& i) {
    while (i < s.size() && (s[i] == ' ' || s[i] == '\t' || s[i] == '\n' || s[i] == '\r')) ++i;
}
}  // namespace version_detail

// Reads GitHub's "list releases" answer: a top-level array whose objects carry tag_name, html_url, prerelease and draft
// as direct members (author, assets and the like nest deeper and are never looked at, so their own html_url cannot be
// mistaken for the release's). An object without a tag_name is dropped. Over kMaxReleasesJson bytes, nesting over 16
// deep, an unterminated string or unbalanced brackets end the scan: complete stays false.
inline ReleaseList ParseReleases(const std::string& json) {
    using namespace version_detail;
    ReleaseList out;
    if (json.size() > kMaxReleasesJson) return out;
    std::string stack;   // the open '[' and '{', outermost first
    ReleaseInfo cur;
    bool haveTag = false;
    size_t i = 0;
    SkipSpace(json, i);
    if (i >= json.size() || json[i] != '[') return out;
    while (i < json.size()) {
        const char c = json[i];
        if (c == '[' || c == '{') {
            if (stack.size() >= kMaxDepth) return out;
            stack.push_back(c);
            if (c == '{' && stack.size() == 2) { cur = ReleaseInfo{}; haveTag = false; }
            ++i;
        } else if (c == ']' || c == '}') {
            if (stack.empty() || stack.back() != (c == ']' ? '[' : '{')) return out;
            if (c == '}' && stack.size() == 2 && haveTag) out.releases.push_back(cur);
            stack.pop_back();
            ++i;
            if (stack.empty()) { SkipSpace(json, i); out.complete = i == json.size(); return out; }
        } else if (c == '"') {
            std::string key;
            bool tooLong = false;
            if (!ReadString(json, i, 64, key, tooLong)) return out;
            size_t j = i;
            SkipSpace(json, j);
            if (j >= json.size() || json[j] != ':') continue;   // a value, or a key we do not need (its value is read as it comes)
            i = j + 1;
            if (stack.size() != 2 || stack[1] != '{') continue;
            SkipSpace(json, i);
            if (i >= json.size()) return out;
            if (key == "tag_name" || key == "html_url") {
                if (json[i] != '"') continue;
                std::string val;
                if (!ReadString(json, i, key == "tag_name" ? kMaxReleaseTag : kMaxReleaseUrl, val, tooLong)) return out;
                if (key == "tag_name") { cur.tag = tooLong ? std::string() : val; haveTag = !cur.tag.empty(); }
                else cur.url = !tooLong && ValidReleaseUrl(val) ? val : std::string();
            } else if (key == "prerelease" || key == "draft") {
                bool& flag = key == "draft" ? cur.draft : cur.prerelease;
                if (json.compare(i, 4, "true") == 0) { flag = true; i += 4; }
                else if (json.compare(i, 5, "false") == 0) { flag = false; i += 5; }
            }
        } else {
            ++i;   // ',', ':', whitespace, numbers, true/false/null
        }
    }
    return out;
}

struct UpdateOffer {
    Version version;
    std::string tag, url;   // url empty when the release carried no usable link
};

// The newest release newer than `running`, or nothing. Drafts are never offered; a prerelease (flagged, or tagged
// -preN) only to a player who is on a prerelease. Tags that do not parse are ignored.
inline std::optional<UpdateOffer> NewestOffer(const std::vector<ReleaseInfo>& releases, const Version& running) {
    std::optional<UpdateOffer> best;
    for (const ReleaseInfo& r : releases) {
        if (r.draft) continue;
        std::optional<Version> v = ParseVersion(r.tag);
        if (!v) continue;
        if ((r.prerelease || v->prerelease()) && !running.prerelease()) continue;
        if (CompareVersion(*v, running) <= 0) continue;
        if (!best || CompareVersion(*v, best->version) > 0) best = UpdateOffer{*v, r.tag, r.url};
    }
    return best;
}

}  // namespace hsm
