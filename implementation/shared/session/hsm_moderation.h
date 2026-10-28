// Kick, ban and votekick: the host's ban list, chat/console command parsing, target lookup and the vote. Game-independent,
// unit-tested (test_hsm_moderation.cpp).
//
// Who may do what (session.cpp): a player host is the admin of its own session. A role=server host has no player: its
// console (HSMMod.cmd, fed by the Pelican egg) is the admin, plus the Steam IDs in server_admins=, and the players can
// start a votekick or (role=server only) a voteban. A ban names a Steam ID ("steam:<id>") or, for a direct-IP player, an address ("ip:<a.b.c.d>").
#pragma once
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <optional>
#include <string>
#include <vector>

namespace hsm {

constexpr size_t kMaxBans = 512;                       // the file and the list stop growing here
constexpr size_t kMaxBanName = 32;                     // a ban's remembered player name, bytes
constexpr int64_t kVoteKickOutSeconds = 15 * 60;       // a passed votekick keeps the player out this long
constexpr int64_t kVoteBanOutSeconds = 7ll * 86400;    // a passed voteban (role=server) bans the player this long
constexpr int64_t kMaxBanSeconds = 10ll * 365 * 86400; // longer reads as permanent
constexpr uint32_t kVoteKickMs = 30000;                // a vote runs this long
constexpr uint32_t kVoteCooldownMs = 120000;           // one vote per player per 2 minutes
constexpr size_t kMinVoters = 2;                       // voters besides the target; fewer: no votekick

// ---------------------------------------------------------------- keys
inline std::string SteamBanKey(uint64_t id) { return "steam:" + std::to_string(id); }
inline std::string IpBanKey(const std::string& addr) { return "ip:" + addr; }

inline bool ValidIpv4(const std::string& s) {
    int parts = 0; size_t i = 0;
    while (i <= s.size()) {
        size_t j = i; unsigned v = 0;
        while (j < s.size() && s[j] >= '0' && s[j] <= '9' && j - i < 3) { v = v * 10 + unsigned(s[j] - '0'); ++j; }
        if (j == i || v > 255 || (j - i > 1 && s[i] == '0')) return false;
        ++parts;
        if (j == s.size()) break;
        if (s[j] != '.') return false;
        i = j + 1;
    }
    return parts == 4;
}

// "steam:<1-20 digits, not 0>" or "ip:<dotted IPv4>".
inline bool ValidBanKey(const std::string& k) {
    if (k.rfind("steam:", 0) == 0) {
        const std::string d = k.substr(6);
        if (d.empty() || d.size() > 20 || d[0] == '0') return false;
        for (char c : d) if (c < '0' || c > '9') return false;
        return d.size() < 20 || d <= "18446744073709551615";
    }
    if (k.rfind("ip:", 0) == 0) return ValidIpv4(k.substr(3));
    return false;
}

// A name as the ban list keeps it: printable ASCII and UTF-8 bytes, no tabs or line breaks, capped (cut on a code point).
inline std::string BanName(const std::string& in) {
    std::string out;
    for (unsigned char c : in) if (c >= 0x20 && c != 0x7F) out += char(c);
    if (out.size() > kMaxBanName) {
        size_t n = kMaxBanName;
        while (n > 0 && (static_cast<unsigned char>(out[n]) & 0xC0) == 0x80) --n;
        out.resize(n);
    }
    return out;
}

inline std::string LowerAscii(std::string s) { for (char& c : s) if (c >= 'A' && c <= 'Z') c = char(c - 'A' + 'a'); return s; }

// ---------------------------------------------------------------- durations
// "30s", "10m", "2h", "7d", "1w", a bare number = minutes; "perm"/"permanent"/"forever"/"0" = permanent (0).
// nullopt: not a duration.
inline std::optional<int64_t> ParseBanDuration(const std::string& s) {
    const std::string w = LowerAscii(s);
    if (w == "perm" || w == "permanent" || w == "forever" || w == "0") return 0;
    if (w.empty() || w.size() > 8) return std::nullopt;
    size_t i = 0; int64_t v = 0;
    while (i < w.size() && w[i] >= '0' && w[i] <= '9') { v = v * 10 + (w[i] - '0'); ++i; }
    if (i == 0 || v <= 0) return std::nullopt;
    int64_t mul = 60;
    if (i < w.size()) {
        if (i + 1 != w.size()) return std::nullopt;
        switch (w[i]) {
            case 's': mul = 1; break;
            case 'm': mul = 60; break;
            case 'h': mul = 3600; break;
            case 'd': mul = 86400; break;
            case 'w': mul = 7 * 86400; break;
            default: return std::nullopt;
        }
    }
    const int64_t secs = v * mul;
    return secs > kMaxBanSeconds ? 0 : secs;
}

// "permanently", "for 15 min", "for 2 h", "for 3 days".
inline std::string DurationText(int64_t secs) {
    if (secs <= 0) return "permanently";
    char b[48];
    if (secs < 120) std::snprintf(b, sizeof b, "for %lld s", (long long)secs);
    else if (secs < 2 * 3600) std::snprintf(b, sizeof b, "for %lld min", (long long)((secs + 59) / 60));
    else if (secs < 2 * 86400) std::snprintf(b, sizeof b, "for %lld h", (long long)((secs + 3599) / 3600));
    else std::snprintf(b, sizeof b, "for %lld days", (long long)((secs + 86399) / 86400));
    return b;
}

// ---------------------------------------------------------------- the ban list
struct BanEntry {
    std::string key;     // steam:<id> or ip:<addr>
    int64_t until = 0;   // unix seconds; 0 = permanent
    bool kick = false;   // a kick's short block: kept in memory only, refused as "kicked"
    std::string name;    // who it was, for /bans
};

class BanList {
public:
    // Adds or replaces the entry for `key`. until: unix seconds, 0 = permanent. false: a bad key, or the list is full.
    // A kick never shortens a ban already there.
    bool Add(const std::string& key, const std::string& name, int64_t until, bool kick) {
        if (!ValidBanKey(key)) return false;
        for (BanEntry& e : list_) {
            if (e.key != key) continue;
            if (kick && !e.kick) return true;
            e.until = until; e.kick = kick; if (!name.empty()) e.name = BanName(name);
            return true;
        }
        if (list_.size() >= kMaxBans) return false;
        list_.push_back({key, until, kick, BanName(name)});
        return true;
    }

    bool Remove(const std::string& key) {
        const size_t n = list_.size();
        std::erase_if(list_, [&](const BanEntry& e) { return e.key == key; });
        return list_.size() != n;
    }

    // The entry that keeps `key` out at `now` (unix seconds), or null.
    const BanEntry* Find(const std::string& key, int64_t now) const {
        for (const BanEntry& e : list_) if (e.key == key && (e.until == 0 || e.until > now)) return &e;
        return nullptr;
    }

    // Drops expired entries. true: something went (the file should be rewritten).
    bool Prune(int64_t now) {
        const size_t n = list_.size();
        std::erase_if(list_, [&](const BanEntry& e) { return e.until != 0 && e.until <= now; });
        return list_.size() != n;
    }

    // /unban's argument: a key, "#n" (the number /bans shows, 1-based over the bans, not the kicks), a bare Steam ID or
    // address, or an exact or unique case-insensitive name prefix. Empty when nothing or more than one entry matches.
    std::string Resolve(const std::string& query) const {
        if (query.empty()) return {};
        if (ValidBanKey(query)) return query;
        if (ValidBanKey("steam:" + query)) return "steam:" + query;
        if (ValidBanKey("ip:" + query)) return "ip:" + query;
        const std::vector<const BanEntry*> b = Bans();
        if (query[0] == '#') {
            size_t n = 0;
            for (size_t i = 1; i < query.size() && i < 6; ++i) { if (query[i] < '0' || query[i] > '9') return {}; n = n * 10 + size_t(query[i] - '0'); }
            return n >= 1 && n <= b.size() && query.size() < 6 ? b[n - 1]->key : std::string();
        }
        const std::string q = LowerAscii(query);
        const BanEntry* hit = nullptr; int prefixHits = 0;
        for (const BanEntry* e : b) {
            const std::string nm = LowerAscii(e->name);
            if (nm == q) return e->key;
            if (nm.rfind(q, 0) == 0) { hit = e; ++prefixHits; }
        }
        return prefixHits == 1 ? hit->key : std::string();
    }

    // The bans (not the kicks), in the order /bans numbers them.
    std::vector<const BanEntry*> Bans() const {
        std::vector<const BanEntry*> out;
        for (const BanEntry& e : list_) if (!e.kick) out.push_back(&e);
        return out;
    }

    // One line per ban: key TAB until TAB name. Kicks are not written.
    std::string Serialize() const {
        std::string out = "# HSMMod bans: key, expiry (unix seconds, 0 = permanent), name. Edit with the server stopped.\n";
        for (const BanEntry& e : list_) {
            if (e.kick) continue;
            out += e.key; out += '\t'; out += std::to_string(e.until); out += '\t'; out += e.name; out += '\n';
        }
        return out;
    }

    // Replaces the bans with the file's (kicks stay). Bad and expired lines are skipped. Returns how many were read.
    size_t Parse(const std::string& text, int64_t now) {
        std::erase_if(list_, [](const BanEntry& e) { return !e.kick; });
        size_t read = 0, pos = 0;
        while (pos < text.size()) {
            size_t end = text.find('\n', pos);
            if (end == std::string::npos) end = text.size();
            std::string line = text.substr(pos, end - pos);
            pos = end + 1;
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (line.empty() || line[0] == '#') continue;
            const size_t t1 = line.find('\t');
            if (t1 == std::string::npos) continue;
            const size_t t2 = line.find('\t', t1 + 1);
            const std::string key = line.substr(0, t1);
            const std::string untilText = line.substr(t1 + 1, t2 == std::string::npos ? std::string::npos : t2 - t1 - 1);
            if (untilText.empty() || untilText.size() > 12) continue;
            int64_t until = 0; bool digits = true;
            for (char c : untilText) { if (c < '0' || c > '9') { digits = false; break; } until = until * 10 + (c - '0'); }
            if (!digits || (until != 0 && until <= now)) continue;
            const std::string name = t2 == std::string::npos ? std::string() : line.substr(t2 + 1);
            if (Add(key, name, until, false)) ++read;
        }
        return read;
    }

    const std::vector<BanEntry>& entries() const { return list_; }
    size_t size() const { return list_.size(); }

private:
    std::vector<BanEntry> list_;
};

// ---------------------------------------------------------------- commands
enum class ModCommandKind { None, Kick, Ban, Unban, Bans, VoteKick, VoteBan, Yes, No, Players, Say };

struct ModCommand {
    ModCommandKind kind = ModCommandKind::None;
    std::string target;      // kick/ban/votekick/voteban: "#slot" or a name; unban: see BanList::Resolve; say: the text
    int64_t seconds = 0;     // ban: 0 = permanent
};

// Chat: "/kick <who>", "/ban <who> [duration]", "/unban <who>", "/bans", "/votekick <who>" (or "/vk"), "/voteban <who>"
// (or "/vb"), "/yes", "/no",
// "/players". The server console takes the same words without the slash, plus "say <text>". Command word is
// case-insensitive. A line that is none of these (or misses its argument) is None and stays chat.
inline ModCommand ParseModCommand(const std::string& line, bool console = false) {
    ModCommand c;
    std::string s = line;
    while (!s.empty() && (s.back() == ' ' || s.back() == '\r' || s.back() == '\n' || s.back() == '\t')) s.pop_back();
    size_t lead = 0;
    while (lead < s.size() && (s[lead] == ' ' || s[lead] == '\t')) ++lead;
    s.erase(0, lead);
    if (s.empty()) return c;
    if (s[0] == '/') s.erase(0, 1);
    else if (!console) return c;
    const size_t sp = s.find(' ');
    const std::string word = LowerAscii(s.substr(0, sp));
    std::string rest = sp == std::string::npos ? std::string() : s.substr(sp + 1);
    while (!rest.empty() && rest.front() == ' ') rest.erase(rest.begin());
    while (!rest.empty() && rest.back() == ' ') rest.pop_back();

    if (word == "kick" && !rest.empty()) { c.kind = ModCommandKind::Kick; c.target = rest; }
    else if (word == "ban" && !rest.empty()) {
        c.kind = ModCommandKind::Ban; c.target = rest;
        const size_t last = rest.rfind(' ');
        if (last != std::string::npos) {                   // "/ban Bob Smith 2h": a trailing duration is the duration
            if (std::optional<int64_t> d = ParseBanDuration(rest.substr(last + 1))) {
                c.seconds = *d; c.target = rest.substr(0, last);
                while (!c.target.empty() && c.target.back() == ' ') c.target.pop_back();
            }
        }
    }
    else if (word == "unban" && !rest.empty()) { c.kind = ModCommandKind::Unban; c.target = rest; }
    else if ((word == "bans" || word == "banlist") && rest.empty()) c.kind = ModCommandKind::Bans;
    else if ((word == "votekick" || word == "vk") && !rest.empty()) { c.kind = ModCommandKind::VoteKick; c.target = rest; }
    else if ((word == "voteban" || word == "vb") && !rest.empty()) { c.kind = ModCommandKind::VoteBan; c.target = rest; }
    else if ((word == "yes" || word == "y") && rest.empty()) c.kind = ModCommandKind::Yes;
    else if ((word == "no" || word == "n") && rest.empty()) c.kind = ModCommandKind::No;
    else if ((word == "players" || word == "who") && rest.empty()) c.kind = ModCommandKind::Players;
    else if (console && word == "say" && !rest.empty()) { c.kind = ModCommandKind::Say; c.target = rest; }
    return c;
}

// ---------------------------------------------------------------- targets
struct ModPlayer { uint8_t slot = 0; std::string name; };

enum class TargetMatch { Found, NoMatch, Ambiguous };

// "#n" = slot n; else an exact case-insensitive name, else a unique case-insensitive name prefix.
inline TargetMatch FindTarget(const std::string& query, const std::vector<ModPlayer>& players, uint8_t& slot) {
    if (query.empty()) return TargetMatch::NoMatch;
    if (query[0] == '#') {
        if (query.size() < 2 || query.size() > 4) return TargetMatch::NoMatch;
        unsigned n = 0;
        for (size_t i = 1; i < query.size(); ++i) { if (query[i] < '0' || query[i] > '9') return TargetMatch::NoMatch; n = n * 10 + unsigned(query[i] - '0'); }
        for (const ModPlayer& p : players) if (p.slot == n) { slot = p.slot; return TargetMatch::Found; }
        return TargetMatch::NoMatch;
    }
    const std::string q = LowerAscii(query);
    const ModPlayer* exact = nullptr; const ModPlayer* prefix = nullptr;
    int exactHits = 0, prefixHits = 0;
    for (const ModPlayer& p : players) {
        const std::string nm = LowerAscii(p.name);
        if (nm == q) { exact = &p; ++exactHits; }
        else if (nm.rfind(q, 0) == 0) { prefix = &p; ++prefixHits; }
    }
    if (exactHits == 1) { slot = exact->slot; return TargetMatch::Found; }
    if (exactHits > 1) return TargetMatch::Ambiguous;
    if (prefixHits == 1) { slot = prefix->slot; return TargetMatch::Found; }
    return prefixHits > 1 ? TargetMatch::Ambiguous : TargetMatch::NoMatch;
}

// ---------------------------------------------------------------- votekick
// Players are named by their stable session key (Steam ID, or the direct-IP peer key). The starter votes yes. A kick
// passes once more than half of the voters (everyone but the target) said yes, a ban (7 days, role=server) once two
// thirds did; either fails once that can no longer happen or after kVoteKickMs. One vote at a time; each player may
// start one per kVoteCooldownMs.
class VoteKick {
public:
    enum class Start { Ok, Running, Cooldown, TooFew, Self };
    enum class Outcome { None, Running, Passed, Failed, Cancelled };

    Start Begin(uint64_t starter, uint64_t target, const std::vector<uint64_t>& players, uint32_t now, bool ban = false) {
        if (running_) return Start::Running;
        if (starter == target) return Start::Self;
        for (const auto& [k, at] : started_)
            if (k == starter && now - at < kVoteCooldownMs) return Start::Cooldown;
        std::vector<uint64_t> voters;
        for (uint64_t p : players) if (p != target && std::find(voters.begin(), voters.end(), p) == voters.end()) voters.push_back(p);
        if (std::find(voters.begin(), voters.end(), starter) == voters.end()) voters.push_back(starter);
        if (voters.size() < kMinVoters) return Start::TooFew;
        running_ = true; ban_ = ban; target_ = target; startedAt_ = now; voters_ = voters; votes_.assign(voters.size(), 0);
        votes_[size_t(std::find(voters_.begin(), voters_.end(), starter) - voters_.begin())] = 1;
        std::erase_if(started_, [&](const std::pair<uint64_t, uint32_t>& s) { return s.first == starter || now - s.second >= kVoteCooldownMs; });
        started_.push_back({starter, now});
        return Start::Ok;
    }

    // false: no vote running, or `who` is not a voter. A voter may change their mind.
    bool Cast(uint64_t who, bool yes) {
        if (!running_) return false;
        for (size_t i = 0; i < voters_.size(); ++i) if (voters_[i] == who) { votes_[i] = yes ? 1 : -1; return true; }
        return false;
    }

    // Someone left: the target leaving cancels the vote, a voter leaving takes their vote with them.
    void Left(uint64_t who) {
        if (!running_) return;
        if (who == target_) { running_ = false; cancelled_ = true; return; }
        for (size_t i = 0; i < voters_.size(); ++i)
            if (voters_[i] == who) { voters_.erase(voters_.begin() + long(i)); votes_.erase(votes_.begin() + long(i)); break; }
    }

    // Call each tick; Passed, Failed and Cancelled are returned once, then None.
    Outcome Tick(uint32_t now) {
        if (cancelled_) { cancelled_ = false; return Outcome::Cancelled; }
        if (!running_) return Outcome::None;
        if (voters_.size() < kMinVoters) { running_ = false; return Outcome::Cancelled; }
        if (Yes() >= Needed()) { running_ = false; return Outcome::Passed; }
        if (Yes() + Undecided() < Needed() || now - startedAt_ >= kVoteKickMs) { running_ = false; return Outcome::Failed; }
        return Outcome::Running;
    }

    void Clear() { running_ = false; cancelled_ = false; voters_.clear(); votes_.clear(); started_.clear(); }

    bool Active() const { return running_; }
    bool Ban() const { return ban_; }               // the running (or last) vote is a voteban
    uint64_t target() const { return target_; }
    size_t Yes() const { return size_t(std::count(votes_.begin(), votes_.end(), int8_t(1))); }
    size_t No() const { return size_t(std::count(votes_.begin(), votes_.end(), int8_t(-1))); }
    size_t Undecided() const { return size_t(std::count(votes_.begin(), votes_.end(), int8_t(0))); }
    size_t Needed() const { return ban_ ? (voters_.size() * 2 + 2) / 3 : voters_.size() / 2 + 1; }   // ban: two thirds, rounded up
    uint32_t SecondsLeft(uint32_t now) const { uint32_t el = now - startedAt_; return el >= kVoteKickMs ? 0 : (kVoteKickMs - el + 999) / 1000; }
    size_t Voters() const { return voters_.size(); }
    // 1 yes, -1 no, 0 not yet; -2: no vote running, or `who` is not a voter (the target, or joined after it began).
    int VoteOf(uint64_t who) const {
        if (!running_) return -2;
        for (size_t i = 0; i < voters_.size(); ++i) if (voters_[i] == who) return votes_[i];
        return -2;
    }

private:
    bool running_ = false, cancelled_ = false, ban_ = false;
    uint64_t target_ = 0;
    uint32_t startedAt_ = 0;
    std::vector<uint64_t> voters_;
    std::vector<int8_t> votes_;                           // 1 yes, -1 no, 0 not yet
    std::vector<std::pair<uint64_t, uint32_t>> started_;  // who started a vote, when (cooldown)
};

// server_admins=: Steam IDs separated by commas, spaces or semicolons. Bad entries are skipped.
inline std::vector<uint64_t> ParseAdminList(const std::string& s) {
    std::vector<uint64_t> out;
    std::string cur;
    auto flush = [&] {
        if (!cur.empty() && ValidBanKey("steam:" + cur) && out.size() < 64) out.push_back(std::stoull(cur));
        cur.clear();
    };
    for (char c : s) { if (c == ',' || c == ' ' || c == ';' || c == '\t') flush(); else cur += c; }
    flush();
    return out;
}

}  // namespace hsm
