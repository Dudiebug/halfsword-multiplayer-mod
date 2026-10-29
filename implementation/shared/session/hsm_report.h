// In-game bug reports (pure, no engine types). A player types "/report <what happened>" in the chat; the mod sends a
// short description, the session's facts and its own log (Steam IDs, IP addresses and the profile folder name taken
// out) to the report relay (report_url), which opens a GitHub issue and keeps the logs private. The relay
// answers with the report's id and an attach token; the reporter shares both with the session (ReportShare), and each
// other player who types "/sendlog" within kReportOfferMs sends their own log to the same report. Nothing is sent
// without a player's own command, except for players who turned on help_improve: their log joins any report filed in
// their lobby on its own, and a crash in their last session is reported at the next start (CrashLineOf).
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
constexpr uint32_t kReportCooldownMs = 30000;      // one report per player per 30 s (the relay allows 20 an hour, 60 a day per address)
constexpr uint32_t kReportShareGapMs = 60000;      // the host relays one ReportShare per peer per minute
constexpr uint32_t kPendingShareMs = 1800000;      // a crash report filed at start is shared with a session joined within 30 min
constexpr size_t kCrashLineMax = 200;
// The last line of a log whose game closed the normal way (log.cpp LogExit, from DllMain at process exit). A crash ends
// the process without it: the engine's crash reporter calls TerminateProcess, which runs no DLL detach.
constexpr const char* kCleanExitLine = "exit: the game closed normally";

// ---------------------------------------------------------------- chat commands
enum class ReportCommandKind : uint8_t { None, Report, SendLog, NoLog };
struct ReportCommand { ReportCommandKind kind = ReportCommandKind::None; std::string text; };

// "/bug <text>" or "/report <text>" (text may be empty), "/sendlog", "/nolog"; case-insensitive command word. Anything
// else is chat.
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
// FNV-1a of `s` mixed with the salt, as 8 hex digits. The client takes one random salt per upload (every log of that
// upload shares it), so an id reads the same across them, and a short hash of a Steam ID cannot be matched by trying
// every ID. Another player's upload to the same report has its own salt.
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
inline bool Hex(char c) { return Digit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'); }
// An IPv6 address at s[i] (not inside a longer word or address): hex groups of 1-4 digits split by ':', either all eight
// or fewer with one "::", optionally ending in a dotted quad ("::ffff:10.0.0.5") and a "%zone". Length or 0. "::1" and
// "::" are 0 (loopback and unspecified stay, like 127.0.0.1). A clock ("12:34:56") has no "::" and too few groups.
inline size_t Ipv6At(const std::string& s, size_t i) {
    if (i > 0 && (Word(s[i - 1]) || s[i - 1] == ':' || s[i - 1] == '.')) return 0;
    size_t p = i, colons = 0, groups = 0, lastGroup = i;
    bool dbl = false;
    while (p < s.size()) {
        if (s[p] == ':') {
            if (p + 1 < s.size() && s[p + 1] == ':') { if (dbl) return 0; dbl = true; colons += 2; p += 2; continue; }
            ++colons; ++p; continue;
        }
        if (!Hex(s[p])) break;
        size_t g = p; while (p < s.size() && Hex(s[p]) && p - g < 5) ++p;
        if (p - g > 4) return 0;
        ++groups; lastGroup = g;
    }
    if (p > i && s[p - 1] == ':' && !(p >= i + 2 && s[p - 2] == ':')) { --p; --colons; }   // a sentence's ':' after it
    if (p < s.size() && s[p] == '.' && groups > 0) {                                      // an IPv4 tail
        if (size_t n = Ipv4At(s, lastGroup)) { p = lastGroup + n; ++groups; }
        else return 0;
    }
    if (p < s.size() && (Word(s[p]) || s[p] == '.')) return 0;
    const bool full = !dbl && groups == 8 && colons == 7;
    const bool compressed = dbl && groups >= 1 && groups <= 7 && colons <= 8;
    if (!full && !compressed) return 0;
    const std::string a = s.substr(i, p - i);
    if (a == "::1" || a == "::") return 0;
    if (p < s.size() && s[p] == '%') { ++p; while (p < s.size() && (Word(s[p]) || s[p] == '-') && p - i < 64) ++p; }
    return p - i;
}
inline char Lower(char c) { return c >= 'A' && c <= 'Z' ? char(c - 'A' + 'a') : c; }
inline bool Sep(char c) { return c == '\\' || c == '/'; }
// The profile folder at s[i], just after "Users" (any case, as Windows paths are) or "home": its length, or 0. A folder
// ends at a separator, or for "Users" also at a quote or the end of the line.
inline size_t ProfileNameAt(const std::string& s, size_t i, bool users) {
    size_t p = i;
    while (p < s.size() && !Sep(s[p]) && s[p] != '\n' && s[p] != '\r' && s[p] != '"' && s[p] != '\'' && p - i < 64) ++p;
    if (p == i) return 0;
    if (p < s.size() && Sep(s[p])) return p - i;
    return users && (p >= s.size() || s[p] == '\n' || s[p] == '\r' || s[p] == '"' || s[p] == '\'') ? p - i : 0;
}
// "Users\<name>", "Users/<name>", the escaped "Users\\<name>" of a JSON-ish line (any case) or "home/<name>" at s[i], after
// a separator, ':' or the start. Sets `keyLen` to the length up to the folder and returns the folder's length, or 0.
inline size_t ProfileAt(const std::string& s, size_t i, size_t& keyLen) {
    if (i > 0 && !Sep(s[i - 1]) && s[i - 1] != ':') return 0;
    const bool users = i + 5 <= s.size() && Lower(s[i]) == 'u' && Lower(s[i + 1]) == 's' && Lower(s[i + 2]) == 'e' && Lower(s[i + 3]) == 'r' && Lower(s[i + 4]) == 's';
    const bool home = s.compare(i, 4, "home") == 0;
    if (!users && !home) return 0;
    size_t p = i + (users ? 5 : 4);
    if (p < s.size() && s[p] == '/') ++p;
    else if (users) { size_t q = p; while (q < s.size() && s[q] == '\\' && q - p < 4) ++q; if (q == p) return 0; p = q; }   // one, two, ... backslashes
    else return 0;
    keyLen = p - i;
    return ProfileNameAt(s, p, users);
}
// The value of "host=" at s[i] (a host name, or an address): its length, or 0 when it is empty, an IPv4 address (that is
// redacted on its own), "localhost" or a loopback.
inline size_t HostValueAt(const std::string& s, size_t i) {
    size_t p = i;
    while (p < s.size() && s[p] > ' ' && s[p] != ',' && s[p] != ';' && s[p] != '"' && s[p] != '\'' && p - i < 253) ++p;
    const std::string v = s.substr(i, p - i);
    if (v.empty() || Ipv4At(s, i) || v == "localhost" || v == "::1" || v == "[::1]") return 0;
    return p - i;
}
}  // namespace report_detail

// Takes out what identifies a player outside the game: SteamID64s (7656119 and 10 more digits) become "steam:<hash>", any
// other 17- or 18-digit number (a Steam lobby or group id) becomes "steamid:<hash>", IPv4 addresses other than 127.0.0.1
// and 0.0.0.0 become "ip:<hash>", IPv6 addresses other than ::1 and :: become "ip6:<hash>" (0.8.0-pre7), a "host=<name>"
// becomes "host=host:<hash>", and the profile folder in "...\Users\<name>\"
// (any case, also the escaped "\\Users\\") and "/home/<name>/" becomes "<user>". Player names stay: the logs go to a
// private store, and they tell who is who.
inline std::string RedactLog(const std::string& in, uint64_t salt) {
    using namespace report_detail;
    std::string out; out.reserve(in.size());
    size_t i = 0;
    while (i < in.size()) {
        const char c = in[i];
        if ((Hex(c) || c == ':')) {
            if (size_t n = Ipv6At(in, i)) { out += "ip6:" + RedactHash(in.substr(i, n), salt); i += n; continue; }
        }
        if (Digit(c) && (i == 0 || !Word(in[i - 1]))) {
            size_t p = i; while (p < in.size() && Digit(in[p])) ++p;
            const bool wordEnd = p >= in.size() || !Word(in[p]);
            if (wordEnd && (p - i == 17 || p - i == 18)) {
                const bool steam = p - i == 17 && in.compare(i, 7, "7656119") == 0;
                out += (steam ? "steam:" : "steamid:") + RedactHash(in.substr(i, p - i), salt); i = p; continue;
            }
            if (size_t n = Ipv4At(in, i)) {
                const std::string ip = in.substr(i, n);
                out += ip == "127.0.0.1" || ip == "0.0.0.0" ? ip : "ip:" + RedactHash(ip, salt);
                i += n; continue;
            }
            out.append(in, i, p - i); i = p; continue;
        }
        if ((c == 'h' || c == 'H') && (i == 0 || !Word(in[i - 1])) && in.size() - i > 5 && Lower(in[i + 1]) == 'o' && Lower(in[i + 2]) == 's' &&
            Lower(in[i + 3]) == 't' && in[i + 4] == '=') {
            out.append(in, i, 5); i += 5;
            if (size_t n = HostValueAt(in, i)) { out += "host:" + RedactHash(in.substr(i, n), salt); i += n; }
            continue;
        }
        size_t keyLen = 0;
        if (size_t n = ProfileAt(in, i, keyLen)) { out.append(in, i, keyLen); out += "<user>"; i += keyLen + n; continue; }
        out.push_back(c); ++i;
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
    std::string ue4ss, ue4ssMods;   // UE4SS version ("none", "off", "3.0.1.0"...) and the enabled UE4SS mods' names
    int protocol = 0, players = 0;
};

inline std::string ReportFactsJson(const ReportFacts& f) {
    auto s = [](const char* k, const std::string& v) { return std::string("\"") + k + "\":\"" + JsonEscape(ValidUtf8(v)) + "\""; };
    return "{" + s("modBuild", f.modBuild) + "," + s("gameBuild", f.gameBuild) + ",\"protocol\":" + std::to_string(f.protocol) + "," +
           s("role", f.role) + "," + s("transport", f.transport) + "," + s("syncMode", f.syncMode) + "," + s("level", f.level) + "," +
           s("matchFormat", f.matchFormat) + "," + s("matchPhase", f.matchPhase) + ",\"players\":" + std::to_string(f.players) + "," +
           s("os", f.os) + "," + s("ue4ss", f.ue4ss) + "," + s("ue4ssMods", f.ue4ssMods) + "}";
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

// ---------------------------------------------------------------- what the player is told
// One request to the relay comes to exactly one outcome line in the chat (and the log). "Filed" is said only for a
// 200/201 that carries a valid id, token and issue URL. A request that left the machine and got no (usable) answer may
// still have been filed, so it says so instead of "not sent"; "not sent" is only for a request that never got out.
enum class ReportJob : uint8_t { Report, Crash, Log };   // /bug and the Report buttons; the crash report at start; /sendlog and help_improve
enum class ReportOutcome : uint8_t { Done, MaybeDone, NotSent, Refused };
// How the HTTP exchange ended before any status (net/http_client.h HttpResponse::Fail, mirrored here without engine types).
enum class ReportTransport : uint8_t { Ok, Unavailable, NoConnection, Timeout, Certificate, Unreachable, Other };
struct ReportVerdict {
    ReportOutcome outcome = ReportOutcome::NotSent;
    std::string reason;              // why, for every outcome but Done; lower case, no full stop
    std::string id, token, issue;    // a filed report (Done, Report/Crash)
};

// `sent`: the request (with its body) left the machine, so the relay may have acted on it. `status` and `body` only
// count when transport is Ok.
inline ReportVerdict JudgeReportAnswer(ReportJob job, ReportTransport transport, bool sent, unsigned status, const std::string& body) {
    ReportVerdict v;
    if (transport != ReportTransport::Ok) {
        switch (transport) {
            case ReportTransport::Unavailable: v.reason = "Windows' web component (WinHTTP) is unavailable"; break;
            case ReportTransport::NoConnection: v.reason = "no internet connection"; break;
            case ReportTransport::Certificate: v.reason = "the report server's certificate is not valid"; break;
            case ReportTransport::Unreachable: v.reason = "cannot reach the report server"; break;
            case ReportTransport::Timeout: v.reason = sent ? "no answer from the report server" : "the report server did not answer"; break;
            default: v.reason = sent ? "the connection broke before the report server answered" : "the connection failed"; break;
        }
        // Only a request that got out and then went quiet (or broke) may have reached the relay.
        const bool afterSend = sent && (transport == ReportTransport::Timeout || transport == ReportTransport::Other);
        v.outcome = afterSend ? ReportOutcome::MaybeDone : ReportOutcome::NotSent;
        return v;
    }
    if (status == 200 || status == 201) {
        if (job == ReportJob::Log) { v.outcome = ReportOutcome::Done; return v; }
        v.id = JsonStringField(body, "id"); v.token = JsonStringField(body, "token"); v.issue = JsonStringField(body, "issue");
        if (ValidReportId(v.id) && ValidReportToken(v.token) && ValidIssueUrl(v.issue)) { v.outcome = ReportOutcome::Done; return v; }
        v.id.clear(); v.token.clear(); v.issue.clear();
        v.outcome = ReportOutcome::MaybeDone;
        v.reason = "the report server's answer was not understood";
        return v;
    }
    if (status >= 200 && status < 300) { v.outcome = ReportOutcome::MaybeDone; v.reason = "the report server gave an unexpected answer"; return v; }
    if (status == 504) { v.outcome = ReportOutcome::MaybeDone; v.reason = "the report server timed out"; return v; }   // the proxy gave up waiting on the relay
    v.outcome = ReportOutcome::Refused;
    if (status == 413) { v.reason = "the report is too large"; return v; }
    std::string clean;   // the relay's own "error" (printable ASCII only), e.g. its 502 when GitHub failed
    for (char ch : JsonStringField(body, "error")) if (ch >= 0x20 && ch < 0x7F && clean.size() < 80) clean.push_back(ch);
    // 429: the relay says which limit was hit ("too many reports from you, try again later"); the generic line only
    // when it says nothing.
    if (status == 429) { v.reason = clean.empty() ? "too many reports right now, try again in a few minutes" : clean; return v; }
    v.reason = clean.empty() ? "the report server answered " + std::to_string(status) : clean;
    return v;
}

// The one line the player sees for a verdict. `shared`: a filed report was offered to the other players in the session.
inline std::string ReportOutcomeLine(ReportJob job, const ReportVerdict& v, bool shared = false) {
    const std::string what = job == ReportJob::Crash ? "Crash report" : job == ReportJob::Log ? "Your log" : "Bug report";
    std::string reason = v.reason;
    if (!reason.empty() && reason[0] >= 'a' && reason[0] <= 'z') reason[0] = char(reason[0] - 'a' + 'A');
    switch (v.outcome) {
        case ReportOutcome::Done:
            if (job == ReportJob::Log) return "Your log was added to the bug report. Thanks!";
            return what + " filed: " + v.issue + " Thanks!" + (shared ? " The other players were asked to add their logs." : "");
        case ReportOutcome::MaybeDone:
            if (job == ReportJob::Log) return reason + ": your log may have been added.";
            return reason + ": your " + (job == ReportJob::Crash ? "crash" : "bug") +
                   " report may have been filed. Check the mod's GitHub issues before reporting again.";
        case ReportOutcome::NotSent: return what + (job == ReportJob::Log ? " was not sent: " : " not sent: ") + v.reason + ".";
        case ReportOutcome::Refused: return what + (job == ReportJob::Log ? " was not added: " : " not filed: ") + v.reason + ".";
    }
    return what + " not sent.";
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

// ---------------------------------------------------------------- crash reports (help_improve)
// Where the last log line starting with `token` begins (after the "[time][thread] " prefix, or at a line start), or npos.
// A token quoted inside another line does not count: "report: ... sending a crash report: crash: fault ..." is not a
// fault, and counting it re-reported the last crash at every start after it (public #7).
inline size_t LastLogLineAt(const std::string& log, const char* token) {
    for (size_t at = log.rfind(token); at != std::string::npos; at = at ? log.rfind(token, at - 1) : std::string::npos)
        if (at == 0 || log[at - 1] == '\n' || log[at - 1] == '\r' || (at >= 2 && log[at - 1] == ' ' && log[at - 2] == ']'))
            return at;
    return std::string::npos;
}

// The last "crash: fault ..." line crash_log.cpp wrote in a log, printable ASCII only and cut to kCrashLineMax; empty
// when the session did not crash: no fault, or the game closed normally after it (kCleanExitLine follows). A fault the
// game plays on past is no proof: the engine's crash reporter lets the game run for seconds (3 to 20 in public #11,
// #13, #14) before it ends the process. It becomes the report's (public) text: code, module+offset, thread; no paths.
inline std::string CrashLineOf(const std::string& log) {
    const size_t at = LastLogLineAt(log, "crash: fault ");
    if (at == std::string::npos) return {};
    const size_t exited = LastLogLineAt(log, kCleanExitLine);
    if (exited != std::string::npos && exited > at) return {};
    std::string line;
    for (size_t i = at; i < log.size() && log[i] != '\n' && log[i] != '\r' && line.size() < kCrashLineMax; ++i)
        if (log[i] >= 0x20 && log[i] < 0x7F) line += log[i];
    return line;
}
inline std::string CrashReportText(const std::string& crashLine) { return "Crash (sent automatically): " + crashLine; }

// A report filed while no session was up (the crash report at start): its id and token wait for the next session, so
// the players there who turned on help_improve add their logs. Taken once; gone after kPendingShareMs.
class PendingShare {
public:
    void Hold(const std::string& id, const std::string& token, uint32_t nowMs) { id_ = id; token_ = token; at_ = nowMs; held_ = true; }
    bool Held(uint32_t nowMs) const { return held_ && nowMs - at_ < kPendingShareMs; }
    bool Take(uint32_t nowMs, std::string& id, std::string& token) {
        const bool ok = Held(nowMs);
        if (ok) { id = id_; token = token_; }
        held_ = false;
        return ok;
    }
private:
    std::string id_, token_; uint32_t at_ = 0; bool held_ = false;
};

}  // namespace hsm
