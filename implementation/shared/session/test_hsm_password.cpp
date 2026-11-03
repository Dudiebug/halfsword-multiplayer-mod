// Unit tests for hsm_password.h. Run via implementation/shared/run_tests.sh test_hsm_password.
#include "session/hsm_password.h"
#include <cstdio>

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++g_fail; } } while (0)

using namespace hsm;

static void TestLobbySecretWire() {
    LobbySecretMsg m; m.password = "open sesame";
    auto b = Encode(m); LobbySecretMsg o;
    CHECK(Decode(b.data(), b.size(), o) && o.password == "open sesame");
    MsgType t; CHECK(PeekType(b.data(), b.size(), t) && t == MsgType::LobbySecret && uint8_t(t) == 60);
    CHECK(b.size() == 4 + 1 + m.password.size());                                        // header, length byte, bytes
    for (size_t cut = 0; cut < b.size(); ++cut) CHECK(!Decode(b.data(), cut, o));       // truncation
    auto extra = b; extra.push_back(0); CHECK(!Decode(extra.data(), extra.size(), o));   // trailing bytes
    auto rev = b; rev[2] = kProtocolRevision + 1; CHECK(!Decode(rev.data(), rev.size(), o));
    auto other = b; other[3] = uint8_t(MsgType::Chat); CHECK(!Decode(other.data(), other.size(), o));   // another type
    auto lie = b; lie[4] = uint8_t(kMaxLobbyPassword + 1); lie.resize(5 + kMaxLobbyPassword + 1, 'x');
    CHECK(!Decode(lie.data(), lie.size(), o));                                           // a length past the cap
    auto short1 = b; short1[4] = 200; CHECK(!Decode(short1.data(), short1.size(), o));   // a length past the data
    LobbySecretMsg none; auto nb = Encode(none);
    CHECK(Decode(nb.data(), nb.size(), o) && o.password.empty() && nb.size() == 5);      // empty: the lobby is open again
    LobbySecretMsg longPw; longPw.password = std::string(100, 'z'); auto lb = Encode(longPw);
    CHECK(Decode(lb.data(), lb.size(), o) && o.password == std::string(kMaxLobbyPassword, 'z'));
    CHECK(!Decode(nullptr, 0, o));
}

static void TestCleanAndEquals() {
    CHECK(CleanPassword("secret") == "secret" && CleanPassword("  secret \t") == "secret");
    CHECK(CleanPassword("a b") == "a b" && CleanPassword("a\nb\x01" "c") == "abc" && CleanPassword("   ").empty() && CleanPassword("").empty());
    CHECK(CleanPassword(std::string(80, 'q')) == std::string(kMaxLobbyPassword, 'q'));
    CHECK(PasswordEquals("abc", "abc") && PasswordEquals("", ""));
    CHECK(!PasswordEquals("abc", "abd") && !PasswordEquals("abc", "ab") && !PasswordEquals("ab", "abc") && !PasswordEquals("abc", ""));
    CHECK(!PasswordEquals(std::string("a\0b", 3), std::string("a\0c", 3)) && PasswordEquals(std::string("a\0b", 3), std::string("a\0b", 3)));
    CHECK(!PasswordEquals("abc", "ABC"));
}

static void TestGateThreeTriesThenWait() {
    PasswordGate g;
    const uint64_t k = 76561198000000001ull;
    CHECK(g.Check(k, "no", "yes", 1000) == PasswordVerdict::Wrong);
    CHECK(g.Check(k, "no", "yes", 2000) == PasswordVerdict::Wrong);
    CHECK(g.Check(k, "no", "yes", 3000) == PasswordVerdict::TooManyTries);          // the third wrong try starts the wait, and says so
    CHECK(g.WaitLeftMs(k, 3000) == kPasswordLockoutMs);
    CHECK(g.Check(k, "no", "yes", 4000) == PasswordVerdict::TooManyTries);
    CHECK(g.Check(k, "yes", "yes", 5000) == PasswordVerdict::TooManyTries);         // even the right one waits (guessing is slowed, not just counted)
    CHECK(g.Check(k, "yes", "yes", 3000 + kPasswordLockoutMs - 1) == PasswordVerdict::TooManyTries);
    CHECK(g.WaitLeftMs(k, 3000 + kPasswordLockoutMs - 1) == 1);
    CHECK(g.Check(k, "yes", "yes", 3000 + kPasswordLockoutMs) == PasswordVerdict::Ok);   // 60 s after the lockout began: allowed again
    CHECK(g.WaitLeftMs(k, 3000 + kPasswordLockoutMs) == 0 && g.size() == 0);
}

static void TestGateWaitThenThreeMore() {
    PasswordGate g;
    const uint64_t k = 5;
    for (int i = 0; i < 3; ++i) g.Check(k, "x", "p", 0);
    const double t = kPasswordLockoutMs + 10;
    CHECK(g.Check(k, "x", "p", t) == PasswordVerdict::Wrong);                        // the wait is over: tries start again
    CHECK(g.Check(k, "x", "p", t + 1) == PasswordVerdict::Wrong);
    CHECK(g.Check(k, "x", "p", t + 2) == PasswordVerdict::TooManyTries);
    CHECK(g.Check(k, "p", "p", t + 3) == PasswordVerdict::TooManyTries);
    CHECK(g.Check(k, "p", "p", t + 2 + kPasswordLockoutMs) == PasswordVerdict::Ok);
}

static void TestGateResetAndIndependence() {
    PasswordGate g;
    const uint64_t a = 1, b = 2;
    CHECK(g.Check(a, "x", "p", 0) == PasswordVerdict::Wrong && g.Check(a, "x", "p", 1) == PasswordVerdict::Wrong);
    CHECK(g.Check(a, "p", "p", 2) == PasswordVerdict::Ok);                           // a right password resets the count
    CHECK(g.Check(a, "x", "p", 3) == PasswordVerdict::Wrong && g.Check(a, "x", "p", 4) == PasswordVerdict::Wrong);   // two again, not the third
    CHECK(g.Check(a, "x", "p", 5) == PasswordVerdict::TooManyTries);
    CHECK(g.Check(b, "x", "p", 6) == PasswordVerdict::Wrong);                        // b is on its own count
    CHECK(g.Check(b, "p", "p", 7) == PasswordVerdict::Ok);
    CHECK(g.Check(a, "p", "p", 8) == PasswordVerdict::TooManyTries);                 // and a's wait is untouched by b
    // An open lobby (no password) lets everyone in and keeps nothing.
    PasswordGate open;
    CHECK(open.Check(a, "", "", 0) == PasswordVerdict::Ok && open.Check(a, "anything", "", 1) == PasswordVerdict::Ok && open.size() == 0);
    // Empty guess at a locked lobby is wrong, and counts.
    PasswordGate g2;
    CHECK(g2.Check(a, "", "p", 0) == PasswordVerdict::Wrong && g2.size() == 1);
    // Changing the password mid-wait does not release anyone; a changed password after a wrong try is judged by the new one.
    PasswordGate g3;
    for (int i = 0; i < 3; ++i) g3.Check(a, "x", "old", 0);
    CHECK(g3.Check(a, "new", "new", 10) == PasswordVerdict::TooManyTries);
    g3.Clear();
    CHECK(g3.Check(a, "new", "new", 11) == PasswordVerdict::Ok);
}

static void TestGateBounded() {
    PasswordGate g;
    for (uint64_t k = 1; k <= 500; ++k) g.Check(k, "x", "p", double(k));
    CHECK(g.size() == kPasswordGateMax);
    // Players still waiting are kept over those with a stray wrong try: lock out 1, then flood with single wrong tries.
    PasswordGate h;
    for (int i = 0; i < 3; ++i) h.Check(1, "x", "p", 0);
    for (uint64_t k = 100; k < 400; ++k) h.Check(k, "x", "p", 1000);
    CHECK(h.size() == kPasswordGateMax);
    CHECK(h.Check(1, "p", "p", 2000) == PasswordVerdict::TooManyTries);              // still waiting
    // When every entry waits, the oldest goes.
    PasswordGate full;
    for (uint64_t k = 1; k <= kPasswordGateMax; ++k) for (int i = 0; i < 3; ++i) full.Check(k, "x", "p", 10);
    full.Check(999, "x", "p", 20);
    CHECK(full.size() == kPasswordGateMax);
    CHECK(full.WaitLeftMs(1, 20) == 0 && full.WaitLeftMs(2, 20) > 0);
}

int main() {
    TestLobbySecretWire();
    TestCleanAndEquals();
    TestGateThreeTriesThenWait();
    TestGateWaitThenThreeMore();
    TestGateResetAndIndependence();
    TestGateBounded();
    if (g_fail) { std::printf("%d check(s) failed\n", g_fail); return 1; }
    std::printf("hsm_password: all tests passed\n");
    return 0;
}
