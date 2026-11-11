// Unit tests for hsm_player_names.h: the nickname and the lobby name a player types (cleaned, capped by characters and bytes,
// slurs refused), the names read back from the wire, and HIDE PLAYER NAMES (every other player is "Player N" by slot, the
// player's own name and the bots' names stay). Run via implementation/shared/run_tests.sh test_hsm_player_names.
#include "session/hsm_player_names.h"
#include <cstdio>

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++g_fail; } } while (0)
using namespace hsm;

static size_t Chars(const std::string& s) {
    size_t n = 0;
    for (unsigned char c : s) if ((c & 0xC0) != 0x80) ++n;
    return n;
}
static bool ValidUtf8(const std::string& s) {
    for (size_t i = 0; i < s.size();) {
        size_t len = 0; uint32_t cp = 0;
        if (!ChatUtf8Next(s, i, len, cp)) return false;
        i += len;
    }
    return true;
}

static void TestCaps() {
    CHECK(kMaxNicknameChars == 24 && kMaxLobbyNameChars == 32);
    CHECK(CleanNickname("Ann") == "Ann");
    CHECK(CleanNickname(std::string(24, 'a')) == std::string(24, 'a'));
    CHECK(CleanNickname(std::string(25, 'a')) == std::string(24, 'a'));                     // one over: cut
    CHECK(CleanNickname(std::string(200, 'a')) == std::string(24, 'a'));
    CHECK(CleanLobbyName(std::string(32, 'b')) == std::string(32, 'b'));
    CHECK(CleanLobbyName(std::string(33, 'b')) == std::string(32, 'b'));
    CHECK(CleanLobbyName(std::string(500, 'b')) == std::string(32, 'b'));
    CHECK(CleanPeerName(std::string(32, 'c')) == std::string(32, 'c'));
    CHECK(CleanPeerName(std::string(33, 'c')) == std::string(32, 'c'));
    CHECK(CleanNickname("").empty() && CleanLobbyName("").empty() && CleanPeerName("").empty());
}

static void TestCharactersNotBytes() {
    // 24 characters of two bytes is 48 bytes: more than Hello.name takes (kMaxPlayerName, 32), so the bytes cap bites first,
    // and the cut falls between characters, never inside one.
    std::string e2;
    for (int i = 0; i < 24; ++i) e2 += "\xC3\xA9";                                          // e with acute
    const std::string n = CleanNickname(e2);
    CHECK(n.size() <= kMaxPlayerName && ValidUtf8(n) && Chars(n) == 16 && n.size() == 32);
    // Three-byte characters: 10 fit in 32 bytes.
    std::string cjk;
    for (int i = 0; i < 24; ++i) cjk += "\xE6\x97\xA5";
    const std::string c = CleanNickname(cjk);
    CHECK(c.size() <= kMaxPlayerName && ValidUtf8(c) && Chars(c) == 10);
    // The lobby name is 32 characters: two-byte ones fit whole.
    std::string l2;
    for (int i = 0; i < 40; ++i) l2 += "\xC3\xA9";
    CHECK(Chars(CleanLobbyName(l2)) == 32 && ValidUtf8(CleanLobbyName(l2)));
    // A peer's name: the same bytes cap.
    CHECK(CleanPeerName(e2).size() <= kMaxPlayerName && ValidUtf8(CleanPeerName(e2)));
}

static void TestCleaning() {
    CHECK(CleanNickname("  Ann  ") == "Ann");                                              // spaces at the ends out
    CHECK(CleanNickname("A\x01n\x7F" "n") == "Ann");                                       // control characters out
    CHECK(CleanNickname("A\xC2\x85nn") == "Ann");                                          // a C1 control out
    CHECK(CleanNickname("Ann\xF0\x9F\x98\x80") == "Ann");                                  // an emoji out
    CHECK(CleanNickname("A\xFFnn") == "Ann");                                              // a bad byte out
    CHECK(CleanNickname("caf\xC3\xA9") == "caf\xC3\xA9");                                  // accents stay
    CHECK(CleanNickname("\x01\x02") .empty());
    // The cut leaves no space at the end: 23 letters, a space, then more.
    CHECK(CleanNickname(std::string(23, 'a') + " bcd") == std::string(23, 'a'));
    CHECK(CleanLobbyName(std::string(31, 'a') + " bcd") == std::string(31, 'a'));
    CHECK(CleanPeerName(std::string(31, 'a') + " bcd") == std::string(31, 'a'));
    // A peer's name keeps what a nickname would lose to the 24-character cap, up to 32 bytes.
    CHECK(CleanPeerName(std::string(30, 'a')) == std::string(30, 'a') && CleanNickname(std::string(30, 'a')) == std::string(24, 'a'));
    // A hostile name from the wire: nothing but controls and bad bytes is empty, and the result is always valid UTF-8.
    CHECK(CleanPeerName("\xFF\xFE\x01").empty());
    CHECK(CleanPeerName("  A\x01nn\xF0\x9F\x98\x80  ") == "Ann");                           // the same cleaning as a typed name
    CHECK(CleanLobbyName("  Fight\x01 night\xF0\x9F\x98\x80  ") == "Fight night");
    CHECK(ValidUtf8(CleanPeerName("ab\xE6\x97 cd")));
}

static void TestSlurs() {
    const std::string slur(kSlursAnywhere[0]);
    CHECK(UsableNickname("Ann") == "Ann");
    CHECK(UsableNickname(slur).empty());
    CHECK(UsableNickname("big " + slur).empty());
    CHECK(UsableLobbyName("Friendly fights") == "Friendly fights");
    CHECK(UsableLobbyName(slur + " lobby").empty());
    CHECK(UsableNickname("  " + std::string(kSlursWord[0]) + "  ").empty());
    CHECK(UsableNickname("").empty() && UsableLobbyName("").empty());
}

static void TestOwnName() {
    CHECK(OwnName("Ann", "AnnSteam") == "Ann");
    CHECK(OwnName("", "AnnSteam") == "AnnSteam");
    CHECK(OwnName("   ", "AnnSteam") == "AnnSteam");                                        // only spaces: none
    CHECK(OwnName(std::string(kSlursAnywhere[0]), "AnnSteam") == "AnnSteam");              // a hand-edited INI with a slur: the Steam name
    CHECK(OwnName(std::string(30, 'a'), "S") == std::string(24, 'a'));                      // cleaned and capped
    CHECK(OwnName("", "") .empty());
}

static void TestLobbyTitle() {
    CHECK(LobbyTitle("Friendly fights", "Ann") == "Friendly fights");
    CHECK(LobbyTitle("", "Ann") == "Ann's fight");
    CHECK(LobbyTitle("  ", "Ann") == "Ann's fight");
    CHECK(LobbyTitle(std::string(kSlursAnywhere[0]), "Ann") == "Ann's fight");
    CHECK(LobbyTitle(std::string(40, 'x'), "Ann") == std::string(32, 'x'));
}

static void TestHidden() {
    CHECK(HiddenName(0) == "Player 1" && HiddenName(1) == "Player 2" && HiddenName(7) == "Player 8");
    CHECK(HiddenName(-1) == "Player");
    // The switch off: every name as it is.
    CHECK(DisplayName(false, false, 3, "Bob") == "Bob" && DisplayName(false, true, 0, "Ann") == "Ann");
    // On: others by slot, the player's own name stays.
    CHECK(DisplayName(true, false, 0, "Host") == "Player 1");
    CHECK(DisplayName(true, false, 3, "Bob") == "Player 4");
    CHECK(DisplayName(true, true, 3, "Me") == "Me");
    CHECK(DisplayName(true, false, -1, "Bob") == "Player");
}

static void TestHiddenByEntity() {
    // A player's entity is its slot + 1: entity 1 is the host, "Player 1".
    CHECK(DisplayNameOfEntity(true, 2, 1, "Host") == "Player 1");
    CHECK(DisplayNameOfEntity(true, 2, 3, "Cy") == "Player 3");
    CHECK(DisplayNameOfEntity(true, 2, 2, "Me") == "Me");
    CHECK(DisplayNameOfEntity(false, 2, 3, "Cy") == "Cy");
    CHECK(DisplayNameOfEntity(true, 2, uint8_t(kMaxFighters), "Last") == "Player " + std::to_string(kMaxFighters));
    // A bot (past the players' entities) and entity 0 keep their names: they are nobody's name to hide.
    CHECK(DisplayNameOfEntity(true, 2, uint8_t(kMaxFighters + 1), "Bandit") == "Bandit");
    CHECK(DisplayNameOfEntity(true, 2, 40, "Zombie 3") == "Zombie 3");
    CHECK(DisplayNameOfEntity(true, 2, 0, "Nobody") == "Nobody");
}

static void TestHideNamesIn() {
    MatchInfo m;
    auto add = [&](uint8_t entity, const char* name) { FighterEntry f; f.entity = entity; f.name = name; f.kills = int16_t(entity); m.fighters.push_back(f); };
    add(1, "Host"); add(2, "Me"); add(3, "Cy"); add(uint8_t(kMaxFighters + 1), "Bandit");
    const MatchInfo h = HideNamesIn(m, 2);
    CHECK(h.fighters.size() == 4);
    if (h.fighters.size() == 4) {
        CHECK(h.fighters[0].name == "Player 1" && h.fighters[1].name == "Me" && h.fighters[2].name == "Player 3" && h.fighters[3].name == "Bandit");
        CHECK(h.fighters[2].kills == 3 && h.fighters[2].entity == 3);                      // only the name changes
    }
    CHECK(m.fighters[0].name == "Host" && m.fighters[2].name == "Cy");                     // the match itself is untouched: a copy
    CHECK(h.scoreLimit == m.scoreLimit && h.phase == m.phase);
    // Nobody of mine in the match (a spectator): every player is hidden.
    const MatchInfo s = HideNamesIn(m, 0);
    CHECK(s.fighters[0].name == "Player 1" && s.fighters[1].name == "Player 2" && s.fighters[3].name == "Bandit");
}

int main() {
    TestCaps();
    TestCharactersNotBytes();
    TestCleaning();
    TestSlurs();
    TestOwnName();
    TestLobbyTitle();
    TestHidden();
    TestHiddenByEntity();
    TestHideNamesIn();
    if (g_fail) { std::printf("%d player name checks failed\n", g_fail); return 1; }
    std::printf("all player name tests passed\n");
    return 0;
}
