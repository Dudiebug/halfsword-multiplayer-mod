// In-game bug reports (pure, no engine types). A player types "/report <what happened>" in the chat; the mod sends a
// short description, the session's facts and its own log (Steam IDs, IP addresses and the profile folder name taken
// out) to the report relay (report_url), which opens a GitHub issue and keeps the logs private. The relay
// answers with the report's id and an attach token; the reporter shares both with the session (ReportShare), and each
// other player who types "/sendlog" within kReportOfferMs sends their own log to the same report. Nothing is sent
// without a player's own command.
#pragma once
#include "hsm_protocol.h"
#include "hsm_chat.h"
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace hsm {

constexpr size_t kReportIdMax = 32;           // relay ids: [A-Za-z0-9]
constexpr size_t kReportTokenMax = 64;        // attach tokens: [A-Za-z0-9_-]
constexpr size_t kReportLogTailBytes = 3u << 20;   // the end of each log that is sent (the relay takes 4 MB per file)
constexpr uint32_t kReportOfferMs = 120000;   // how long another player may answer "/sendlog"
constexpr uint32_t kReportCooldownMs = 120000;     // one report per player per two minutes
constexpr uint32_t kReportShareGapMs = 60000;      // the host relays one ReportShare per peer per minute

// ---------------------------------------------------------------- chat commands
enum class ReportCommandKind : uint8_t { None, Report, SendLog, NoLog };
struct ReportCommand { ReportCommandKind kind = ReportCommandKind::None; std::string text; };

// "/report <text>" (text may be empty), "/sendlog", "/nolog"; case-insensitive command word. Anything else is chat.
inline ReportCommand ParseReportCommand(const std::string& line) {
    ReportCommand c;
    if (line.empty() || line[0] != '/') return c;
    size_t end = line.find(' ');
    std::string word = line.substr(1, end == std::string::npos ? std::string::npos : end - 1);
    for (char& ch : word) if (ch >= 'A' && ch <= 'Z') ch = char(ch - 'A' + 'a');
    std::string rest = end == std::string::npos ? std::string() : line.substr(end + 1);
    while (!rest.empty() && rest.front() == ' ') rest.erase(rest.begin());
    while (!rest.empty() && rest.back() == ' ') rest.pop_back();
    if (word == "report" || word == "bug") { c.kind = ReportCommandKind::Report; c.text = rest; }
    else if (word == "sendlog" && rest.empty()) c.kind = ReportCommandKind::SendLog;
    else if (word == "nolog" && rest.empty()) c.kind = ReportCommandKind::NoLog;
    return c;
}

// ---------------------------------------------------------------- redaction
// FNV-1a of `s` mixed with the report's salt, as 8 hex digits: the same id reads the same within one report, and a
// random salt per report keeps a short hash of a Steam ID from being matched by trying every ID.
inline std::string RedactHash(const std::string& s, uint64_t salt) {
    uint64_t h = 1469598103934665603ull ^ salt;
    for (unsigned char c : s) { h ^= c; h *= 1099511628211ull; }
    char b[9]; std::snprintf(b, sizeof b, "%08x", unsigned((h >> 32) ^ h));
    return b;
}

namespace report_detail {
inline bool Digit(char c) { return c >= '0' && c <= '9'; }
inline bool Word(char c) { return Digit(c) || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_'; }
// A dotted quad of 1-3 digit parts, each <= 255, at s[i] (not inside a longer number or word). Length or 0.
inline size_t Ipv4At(const std::string& s, size_t i) {
    if (i > 0 && (Word(s[i - 1]) || s[i - 1] == '.')) return 0;
    size_t p = i;
    for (int part = 0; part < 4; ++part) {
        size_t start = p; int v = 0;
        while (p < s.size() && Digit(s[p]) && p - start < 3) v = v * 10 + (s[p++] - '0');
        if (p == start || v > 255) return 0;
        if (part < 3) { if (p >= s.size() || s[p] != '.') return 0; ++p; }
    }
    if (p < s.size() && (Word(s[p]) || (s[p] == '.' && p + 1 < s.size() && Digit(s[p + 1])))) return 0;
    return p - i;
}
// The profile folder after "Users\" / "Users/" or "home/": its length, or 0.
inline size_t ProfileNameAt(const std::string& s, size_t i) {
    size_t p = i;
    while (p < s.size() && s[p] != '\\' && s[p] != '/' && s[p] != '\n' && s[p] != '"' && s[p] != '\'' && p - i < 64) ++p;
    return p < s.size() && (s[p] == '\\' || s[p] == '/') ? p - i : 0;
}
}  // namespace report_detail

// Takes out what identifies a player outside the game: SteamID64s (7656119 and 10 more digits) become "steam:<hash>",
// IPv4 addresses other than 127.0.0.1 and 0.0.0.0 become "ip:<hash>", and the profile folder in "...\Users\<name>\"
// and "/home/<name>/" becomes "<user>". Player names stay: the logs go to a private store, and they tell who is who.
inline std::string RedactLog(const std::string& in, uint64_t salt) {
    using namespace report_detail;
    std::string out; out.reserve(in.size());
    size_t i = 0;
    while (i < in.size()) {
        const char c = in[i];
        if (Digit(c) && (i == 0 || !Word(in[i - 1]))) {
            size_t p = i; while (p < in.size() && Digit(in[p])) ++p;
            if (p - i == 17 && in.compare(i, 7, "7656119") == 0 && (p >= in.size() || !Word(in[p]))) {
                out += "steam:" + RedactHash(in.substr(i, 17), salt); i = p; continue;
            }
            if (size_t n = Ipv4At(in, i)) {
                const std::string ip = in.substr(i, n);
                out += ip == "127.0.0.1" || ip == "0.0.0.0" ? ip : "ip:" + RedactHash(ip, salt);
                i += n; continue;
            }
            out.append(in, i, p - i); i = p; continue;
        }
        bool profile = false;
        for (const char* key : {"Users\\", "Users/", "home/"}) {
            const size_t k = std::char_traits<char>::length(key);
            if (in.compare(i, k, key) != 0 || (i > 0 && in[i - 1] != '\\' && in[i - 1] != '/' && in[i - 1] != ':')) continue;
            if (size_t n = ProfileNameAt(in, i + k)) { out.append(key); out += "<user>"; i += k + n; profile = true; break; }
        }
        if (!profile) { out.push_back(c); ++i; }
    }
    return out;
}

// The last `max` bytes of a log, starting on a line (a cut first line is dropped), with a note when cut. `earlier`:
// bytes of the file before `log` that were never read (the client reads only the end of a long log).
inline std::string LogTail(const std::string& log, size_t max, uint64_t earlier = 0) {
    if (log.size() <= max && earlier == 0) return log;
    size_t from = log.size() > max ? log.size() - max : 0;
    size_t nl = log.find('\n', from);
    from = nl == std::string::npos ? from : nl + 1;
    return "[... " + std::to_string(earlier + from) + " earlier bytes not sent]\n" + log.substr(from);
}

// ---------------------------------------------------------------- JSON for the relay
inline std::string JsonEscape(const std::string& s) {
    std::string o; o.reserve(s.size() + 16);
    for (unsigned char c : s) {
        switch (c) {
            case '"': o += "\\\""; break;
            case '\\': o += "\\\\"; break;
            case '\n': o += "\\n"; break;
            case '\r': o += "\\r"; break;
            case '\t': o += "\\t"; break;
            default:
                if (c < 0x20 || c == 0x7F) { char b[8]; std::snprintf(b, sizeof b, "\\u%04x", c); o += b; }
                else o.push_back(char(c));
        }
    }
    return o;
}
// Logs are mostly ASCII, but a player name can be anything: invalid UTF-8 becomes '?', so the JSON always parses.
inline std::string ValidUtf8(const std::string& s) {
    std::string o; o.reserve(s.size());
    for (size_t i = 0; i < s.size();) {
        size_t len; uint32_t cp;
        if (ChatUtf8Next(s, i, len, cp)) { o.append(s, i, len); i += len; }
        else { o.push_back('?'); ++i; }
    }
    return o;
}

struct ReportLog { std::string name, text; };   // name: "HSMMod.log", "HSMMod.prev.log"
struct ReportFacts {
    std::string modBuild, gameBuild, role, transport, syncMode, level, matchFormat, matchPhase, os;
    int protocol = 0, players = 0;
};

inline std::string ReportFactsJson(const ReportFacts& f) {
    auto s = [](const char* k, const std::string& v) { return std::string("\"") + k + "\":\"" + JsonEscape(ValidUtf8(v)) + "\""; };
    return "{" + s("modBuild", f.modBuild) + "," + s("gameBuild", f.gameBuild) + ",\"protocol\":" + std::to_string(f.protocol) + "," +
           s("role", f.role) + "," + s("transport", f.transport) + "," + s("syncMode", f.syncMode) + "," + s("level", f.level) + "," +
           s("matchFormat", f.matchFormat) + "," + s("matchPhase", f.matchPhase) + ",\"players\":" + std::to_string(f.players) + "," +
           s("os", f.os) + "}";
}
inline std::string ReportLogsJson(const std::vector<ReportLog>& logs) {
    std::string o = "[";
    for (const ReportLog& l : logs)
        o += (o.size() > 1 ? "," : "") + std::string("{\"name\":\"") + JsonEscape(l.name) + "\",\"text\":\"" + JsonEscape(ValidUtf8(l.text)) + "\"}";
    return o + "]";
}
// POST /v1/reports
inline std::string ReportJson(const std::string& description, const ReportFacts& f, const std::vector<ReportLog>& logs) {
    return "{\"description\":\"" + JsonEscape(SanitizeChat(description)) + "\",\"facts\":" + ReportFactsJson(f) + ",\"logs\":" + ReportLogsJson(logs) + "}";
}
// POST /v1/reports/<id>/logs
inline std::string AttachJson(const std::string& token, const ReportFacts& f, const std::vector<ReportLog>& logs) {
    return "{\"token\":\"" + JsonEscape(token) + "\",\"facts\":" + ReportFactsJson(f) + ",\"logs\":" + ReportLogsJson(logs) + "}";
}

// A string field of the relay's flat JSON answer ({"id":"...","issue":"...","token":"...","error":"..."}); simple
// escapes only. Empty when missing.
inline std::string JsonStringField(const std::string& body, const std::string& key) {
    const std::string pat = "\"" + key + "\"";
    size_t p = body.find(pat);
    if (p == std::string::npos) return {};
    p += pat.size();
    while (p < body.size() && (body[p] == ' ' || body[p] == ':')) ++p;
    if (p >= body.size() || body[p] != '"') return {};
    std::string v;
    for (++p; p < body.size() && body[p] != '"'; ++p) {
        if (body[p] == '\\' && p + 1 < body.size()) { ++p; v.push_back(body[p] == 'n' ? '\n' : body[p]); }
        else v.push_back(body[p]);
        if (v.size() > 512) return {};
    }
    return p < body.size() ? v : std::string();
}

// ---------------------------------------------------------------- ids, tokens, the share message
inline bool ValidReportId(const std::string& s) {
    if (s.empty() || s.size() > kReportIdMax) return false;
    for (char c : s) if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'))) return false;
    return true;
}
inline bool ValidReportToken(const std::string& s) {
    if (s.size() < 16 || s.size() > kReportTokenMax) return false;
    for (char c : s) if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_' || c == '-')) return false;
    return true;
}
// The issue link the relay answered with, shown in the chat: only an https GitHub issue URL.
inline bool ValidIssueUrl(const std::string& s) {
    const std::string pre = "https://github.com/";
    if (s.size() > 200 || s.compare(0, pre.size(), pre) != 0) return false;
    for (char c : s) if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '/' || c == '-' || c == '_' || c == '.' || c == ':')) return false;
    return s.find("/issues/") != std::string::npos;
}

// ReportShare (rev 18): the reporter -> host -> every other player. fromSlot is the host's stamp (a client's own value
// is replaced), so the prompt names the real reporter.
struct ReportShareMsg { uint8_t fromSlot = 0; std::string id, token; };
inline std::vector<uint8_t> Encode(const ReportShareMsg& m) {
    Writer w; w.header(MsgType::ReportShare); w.u8(m.fromSlot); w.str(m.id, kReportIdMax); w.str(m.token, kReportTokenMax); return w.buf;
}
inline bool Decode(const uint8_t* d, size_t n, ReportShareMsg& m) {
    Reader r(d, n); MsgType t; if (!r.header(t) || t != MsgType::ReportShare) return false;
    m.fromSlot = r.u8(); m.id = r.str(kReportIdMax); m.token = r.str(kReportTokenMax);
    return r.done() && m.fromSlot < kMaxWillies && ValidReportId(m.id) && ValidReportToken(m.token);
}

// One offer to send a log, as another player's ReportShare made it; answered once.
class ReportOffer {
public:
    void Offer(const std::string& id, const std::string& token, uint32_t nowMs) { id_ = id; token_ = token; at_ = nowMs; open_ = true; }
    bool Open(uint32_t nowMs) const { return open_ && nowMs - at_ < kReportOfferMs; }
    // The offer's id and token for "/sendlog", closing it; false when there is none or it expired.
    bool Take(uint32_t nowMs, std::string& id, std::string& token) {
        if (!Open(nowMs)) { open_ = false; return false; }
        id = id_; token = token_; open_ = false; return true;
    }
    void Decline() { open_ = false; }
private:
    std::string id_, token_; uint32_t at_ = 0; bool open_ = false;
};

}  // namespace hsm
