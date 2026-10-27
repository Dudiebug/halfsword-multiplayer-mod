// Unit tests for DecideLeftBackup (hsm_save_guard.h). Run via implementation/shared/run_tests.sh test_hsm_save_guard_age.
#include "session/hsm_save_guard.h"
#include <cstdio>

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++g_fail; } } while (0)
using namespace hsm::session;

// exists, ownerAlive, own, bytes, backupMtime, saveExists, saveMtime
static LeftBackup D(bool exists, bool alive, bool own, uint64_t bytes, int64_t bm, bool se, int64_t sm) {
    return DecideLeftBackup(exists, alive, own, bytes, bm, se, sm);
}

static void TestIgnore() {
    CHECK(D(false, false, false, 100, 1000, true, 1000) == LeftBackup::Ignore);   // no file
    CHECK(D(true, false, true, 100, 1000, true, 1000) == LeftBackup::Ignore);     // our own session's
    CHECK(D(true, true, false, 100, 1000, true, 1000) == LeftBackup::Ignore);     // a live process owns it
    CHECK(D(true, true, false, 0, 1000, true, 9999) == LeftBackup::Ignore);       // ignore wins over stale rules
}

static void TestRestore() {
    CHECK(D(true, false, false, 100, 1000, true, 1000) == LeftBackup::Restore);   // same age
    CHECK(D(true, false, false, 100, 1000, true, 500) == LeftBackup::Restore);    // save older than the backup
    CHECK(D(true, false, false, 100, 1000, false, 0) == LeftBackup::Restore);     // save gone: put the backup back
}

static void TestNewerSaveBoundary() {
    CHECK(D(true, false, false, 100, 1000, true, 1060) == LeftBackup::Restore);   // exactly 60 s: still restore
    CHECK(D(true, false, false, 100, 1000, true, 1061) == LeftBackup::KeepAside); // 61 s: the save is newer
    CHECK(D(true, false, false, 100, 1000, true, 100000) == LeftBackup::KeepAside);
}

static void TestEmptyBackupNeverDeletes() {
    CHECK(D(true, false, false, 0, 1000, true, 1000) == LeftBackup::KeepAside);
    CHECK(D(true, false, false, 0, 1000, true, 0) == LeftBackup::KeepAside);
    CHECK(D(true, false, false, 0, 1000, false, 0) == LeftBackup::KeepAside);
}

int main() {
    TestIgnore(); TestRestore(); TestNewerSaveBoundary(); TestEmptyBackupNeverDeletes();
    if (g_fail) { std::printf("%d failure(s)\n", g_fail); return 1; }
    std::printf("test_hsm_save_guard_age: ok\n");
    return 0;
}
