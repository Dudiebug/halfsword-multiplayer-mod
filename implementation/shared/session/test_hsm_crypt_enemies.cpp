// Unit tests for hsm_crypt_enemies.h (the host's director enemies in the shared Crypts). Run via implementation/shared/run_tests.sh test_hsm_crypt_enemies.
#include "session/hsm_crypt_enemies.h"
#include <cstdio>
#include <vector>

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++g_fail; } } while (0)

using namespace hsm::crypt;

static void TestVerdict() {
    CHECK(EnemyVerdictOf(false, true) == EnemyVerdict::Adopt);
    CHECK(EnemyVerdictOf(false, false) == EnemyVerdict::Wait);       // all bot entities in use: asked again at the next scan
    CHECK(EnemyVerdictOf(true, true) == EnemyVerdict::Leave);        // dead: never adopted, never destroyed
    CHECK(EnemyVerdictOf(true, false) == EnemyVerdict::Leave);
    for (EnemyVerdict v : {EnemyVerdict::Adopt, EnemyVerdict::Wait, EnemyVerdict::Leave}) CHECK(EnemyVerdictText(v)[0] != '?');
}

static void TestPrune() {
    int a[6] = {1, 0, 2, 3, 0, 4};
    int32_t n = 6;
    CHECK(PruneList(a, n, [](int v) { return v == 0; }) == 2 && n == 4);   // nulls out, order kept
    CHECK(a[0] == 1 && a[1] == 2 && a[2] == 3 && a[3] == 4);
    CHECK(PruneList(a, n, [](int v) { return v == 9; }) == 0 && n == 4);   // nothing to take
    CHECK(PruneList(a, n, [](int) { return true; }) == 4 && n == 0);       // everything
    CHECK(PruneList(a, n, [](int) { return true; }) == 0 && n == 0);
    int32_t m = 3;
    CHECK(PruneList<int>(nullptr, m, [](int) { return true; }) == 0 && m == 3);   // no array: untouched
    int32_t neg = -1;
    CHECK(PruneList(a, neg, [](int) { return true; }) == 0 && neg == -1);
    int one[1] = {7}; int32_t k = 1;
    CHECK(PruneList(one, k, [](int v) { return v == 7; }) == 1 && k == 0);
    int* p[3] = {nullptr, reinterpret_cast<int*>(8), nullptr}; int32_t pn = 3;       // the director's list holds pointers
    CHECK(PruneList(p, pn, [](int* x) { return x == nullptr; }) == 2 && pn == 1 && p[0] == reinterpret_cast<int*>(8));
}

int main() {
    TestVerdict();
    TestPrune();
    if (g_fail) { std::printf("%d failure(s)\n", g_fail); return 1; }
    std::printf("test_hsm_crypt_enemies: OK\n");
    return 0;
}
