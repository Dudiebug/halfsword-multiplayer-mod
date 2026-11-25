// Unit tests for hsm_crypt_character.h (Co-op Crypts: each player's own crypt character between runs).
// Run via implementation/shared/run_tests.sh test_hsm_crypt_character.
#include "session/hsm_crypt_character.h"
#include <cstdio>
#include <string>

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++g_fail; } } while (0)

using namespace hsm::crypt;

// Each way a run ends, with and without a kit read.
static void TestAfter() {
    CHECK(CoopCharAfter(Outcome::Out, true) == CharAfter::Final);       // got out: the kit carried out
    CHECK(CoopCharAfter(Outcome::Out, false) == CharAfter::Keep);       // got out, no kit read: never lose the character
    CHECK(CoopCharAfter(Outcome::Down, true) == CharAfter::Blank);      // fell: wiped, as the game does
    CHECK(CoopCharAfter(Outcome::Down, false) == CharAfter::Blank);
    CHECK(CoopCharAfter(Outcome::Inside, true) == CharAfter::Final);    // the run ended with them inside: the last kit read alive
    CHECK(CoopCharAfter(Outcome::Inside, false) == CharAfter::Keep);
    CHECK(CoopCharAfter(Outcome::Left, true) == CharAfter::Final);      // left mid-run
    CHECK(CoopCharAfter(Outcome::Left, false) == CharAfter::Keep);
    CHECK(CoopCharAfter(Outcome::Absent, true) == CharAfter::Keep);     // never got in: unchanged, even with a kit sent
    CHECK(CoopCharAfter(Outcome::Absent, false) == CharAfter::Keep);
    CHECK(std::string(CharAfterName(CharAfter::Blank)).find("wiped") != std::string::npos);
}

static void TestPick() {
    CHECK(PickCharacterFile(true, true) == 0);
    CHECK(PickCharacterFile(true, false) == 0);
    CHECK(PickCharacterFile(false, true) == 1);      // a damaged main file: the backup
    CHECK(PickCharacterFile(false, false) == -1);    // first run
}

// One save per run: the exit's save is not overwritten by the results screen or the lobby that follow.
static void TestOnce() {
    CharSaveOnce o;
    CHECK(!o.Take(0));            // no run
    CHECK(o.Take(5));
    CHECK(!o.Take(5));
    CHECK(o.saved() == 5);
    CHECK(o.Take(6));             // the next run saves again
    CHECK(!o.Take(6));
}

int main() {
    TestAfter(); TestPick(); TestOnce();
    if (g_fail) { std::printf("test_hsm_crypt_character: %d failure(s)\n", g_fail); return 1; }
    std::printf("test_hsm_crypt_character: ok\n");
    return 0;
}
