// Unit tests for hsm_save_guard.h. Run via implementation/shared/run_tests.sh test_hsm_save_guard.
#include "session/hsm_save_guard.h"
#include <cstdio>

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++g_fail; } } while (0)
using namespace hsm::session;

static void TestOffUntilJoined() {
    SaveGuardPolicy g;
    CHECK(g.Update(false, false) == SaveAction::None);
    CHECK(g.Update(false, true) == SaveAction::None);   // a single-player fight is not guarded
    CHECK(!g.Blocking());
}

static void TestJoiningBacksUpOnce() {
    SaveGuardPolicy g;
    CHECK(g.Update(true, false) == SaveAction::Backup);
    CHECK(g.Blocking());
    CHECK(g.Update(true, false) == SaveAction::None);
    CHECK(g.Update(true, true) == SaveAction::None);
    CHECK(g.Blocking());
}

static void TestLeavingInAMenuRestores() {
    SaveGuardPolicy g;
    g.Update(true, true);
    CHECK(g.Update(false, false) == SaveAction::Restore);
    CHECK(!g.Blocking());
    CHECK(g.Update(false, false) == SaveAction::None);
}

static void TestLeavingInAFightHoldsUntilTheMenu() {
    SaveGuardPolicy g;
    g.Update(true, true);
    CHECK(g.Update(false, true) == SaveAction::None);   // still in the session's fight level: still blocking
    CHECK(g.Blocking());
    CHECK(g.Update(false, true) == SaveAction::None);
    CHECK(g.Update(false, false) == SaveAction::Restore);
    CHECK(!g.Blocking());
}

static void TestRejoiningWhileHeldKeepsTheFirstBackup() {
    SaveGuardPolicy g;
    CHECK(g.Update(true, true) == SaveAction::Backup);
    CHECK(g.Update(false, true) == SaveAction::None);
    CHECK(g.Update(true, true) == SaveAction::None);    // the save on disk is still the original one
    CHECK(g.Update(false, false) == SaveAction::Restore);
}

static void TestASecondSessionBacksUpAgain() {
    SaveGuardPolicy g;
    g.Update(true, false);
    CHECK(g.Update(false, false) == SaveAction::Restore);
    CHECK(g.Update(true, false) == SaveAction::Backup);
}

static void TestLeftBackups() {
    CHECK(RestoreLeftBackup(true, false, false));    // its process is gone: put it back
    CHECK(!RestoreLeftBackup(true, true, false));    // another running copy of the game owns it
    CHECK(!RestoreLeftBackup(true, false, true));    // this process's own
    CHECK(!RestoreLeftBackup(false, false, false));
}

int main() {
    TestOffUntilJoined();
    TestJoiningBacksUpOnce();
    TestLeavingInAMenuRestores();
    TestLeavingInAFightHoldsUntilTheMenu();
    TestRejoiningWhileHeldKeepsTheFirstBackup();
    TestASecondSessionBacksUpAgain();
    TestLeftBackups();
    if (g_fail) { std::printf("test_hsm_save_guard: %d failure(s)\n", g_fail); return 1; }
    std::printf("test_hsm_save_guard: ok\n");
    return 0;
}
