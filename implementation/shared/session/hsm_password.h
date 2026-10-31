// Lobby password (protocol rev 35). The public listing says only that a lobby is locked (lobby data hsm_lock = "1"/"0");
// the password itself never goes in lobby data. A joiner sends it in Hello; the host compares it before it lets anyone
// in (the Steam lobby has no kick, so this is the gate: a player who skips it sits in the Steam lobby, reads its chat and
// gets no fighter, no match traffic and no password). The host hands the password to every player it let in
// (LobbySecret 60, host -> client, reliable), so after a host migration the new host keeps the lobby locked.
//
// PasswordGate is the host's brake on guessing: per player (SteamID), 3 wrong tries lock that player out for 60 s. The
// count is kept here, not in the peer table, because a rejected joiner leaves the Steam lobby and its peer is dropped.
//
// Wire: LobbySecret 60 (host -> client, reliable): 4 header, str(password, 32). An empty password means the lobby is open
// again. Hello (hsm_protocol.h) ends with str(password, 32).
// Game-independent and unit-tested.
#pragma once
#include "hsm_protocol.h"
#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

namespace hsm {

struct LobbySecretMsg { std::string password; };

inline std::vector<uint8_t> Encode(const LobbySecretMsg& m) {
    Writer w; w.header(MsgType::LobbySecret); w.str(m.password, kMaxLobbyPassword); return w.buf;
}
inline bool Decode(const uint8_t* d, size_t n, LobbySecretMsg& m) {
    Reader r(d, n); MsgType t; if (!r.header(t) || t != MsgType::LobbySecret) return false;
    m.password = r.str(kMaxLobbyPassword); return r.done();
}

// What was typed, made comparable: control characters out, spaces at the ends trimmed, at most kMaxLobbyPassword bytes.
// Host and joiner both run it, so "  secret " and "secret" are the same password.
inline std::string CleanPassword(const std::string& in) {
    std::string s;
    for (unsigned char c : in) if (c >= 0x20 && c != 0x7F) s.push_back(char(c));
    const size_t a = s.find_first_not_of(' ');
    if (a == std::string::npos) return {};
    s = s.substr(a, s.find_last_not_of(' ') - a + 1);
    if (s.size() > kMaxLobbyPassword) s.resize(kMaxLobbyPassword);
    return s;
}

// Equal contents, in time that depends on the lengths only, not on where the first difference is.
inline bool PasswordEquals(const std::string& a, const std::string& b) {
    unsigned diff = unsigned(a.size() != b.size());
    const size_t n = std::max(a.size(), b.size());
    for (size_t i = 0; i < n; ++i) {
        const unsigned x = i < a.size() ? static_cast<unsigned char>(a[i]) : 0u;
        const unsigned y = i < b.size() ? static_cast<unsigned char>(b[i]) : 0u;
        diff |= x ^ y;
    }
    return diff == 0;
}

enum class PasswordVerdict : uint8_t { Ok, Wrong, TooManyTries };

constexpr int kPasswordTries = 3;                    // wrong tries before the wait
constexpr double kPasswordLockoutMs = 60000;         // the wait
constexpr size_t kPasswordGateMax = 64;              // players remembered; the oldest go first

class PasswordGate {
public:
    // The verdict on `given` against the lobby's `expected` password for the player `key` at nowMs. An empty `expected`
    // is an open lobby: always Ok. During a lockout the answer is TooManyTries whatever is given (a right guess does not
    // end the wait early, or the wait would not slow a guesser down). The third wrong try is itself TooManyTries: the
    // player is told about the wait at once. After the wait the player has kPasswordTries tries again. A right password
    // forgets the player's count.
    PasswordVerdict Check(uint64_t key, const std::string& given, const std::string& expected, double nowMs) {
        if (expected.empty()) return PasswordVerdict::Ok;
        Entry* e = Find(key);
        if (e && e->lockedUntil > 0 && nowMs >= e->lockedUntil) { Forget(key); e = nullptr; }   // the wait is over
        if (e && e->lockedUntil > 0) return PasswordVerdict::TooManyTries;
        if (PasswordEquals(given, expected)) { Forget(key); return PasswordVerdict::Ok; }
        if (!e) e = Add(key, nowMs);
        if (++e->fails >= kPasswordTries) { e->lockedUntil = nowMs + kPasswordLockoutMs; return PasswordVerdict::TooManyTries; }
        return PasswordVerdict::Wrong;
    }

    // Milliseconds left of the player's wait at nowMs; 0: none.
    double WaitLeftMs(uint64_t key, double nowMs) const {
        for (const Entry& e : entries_) if (e.key == key && e.lockedUntil > nowMs) return e.lockedUntil - nowMs;
        return 0;
    }
    size_t size() const { return entries_.size(); }
    void Clear() { entries_.clear(); }

private:
    struct Entry { uint64_t key = 0; int fails = 0; double lockedUntil = 0; };   // lockedUntil 0: not locked out
    Entry* Find(uint64_t key) { for (Entry& e : entries_) if (e.key == key) return &e; return nullptr; }
    void Forget(uint64_t key) { std::erase_if(entries_, [&](const Entry& e) { return e.key == key; }); }
    Entry* Add(uint64_t key, double nowMs) {
        if (entries_.size() >= kPasswordGateMax) {   // the oldest player without a running wait goes; else the oldest
            auto it = std::find_if(entries_.begin(), entries_.end(), [&](const Entry& e) { return e.lockedUntil <= nowMs; });
            entries_.erase(it != entries_.end() ? it : entries_.begin());
        }
        entries_.push_back({key, 0, 0});
        return &entries_.back();
    }
    std::vector<Entry> entries_;
};

}  // namespace hsm
