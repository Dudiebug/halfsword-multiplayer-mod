// #620: a Progression win pays the due purse and spends it, a loss spends it too (the bet stays lost, stock).
#include "session/hsm_career.h"
#include <cstdio>

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++g_fail; } } while (0)
using namespace hsm;

int main() {
    { const ProgPayout p = ProgPayoutFor(true, 250); CHECK(p.fundsDelta == 250 && p.dueAfter == 0); }    // a win pays the purse
    { const ProgPayout p = ProgPayoutFor(false, 250); CHECK(p.fundsDelta == 0 && p.dueAfter == 0); }    // a loss: the bet stays lost, nothing pending
    { const ProgPayout p = ProgPayoutFor(true, 0); CHECK(p.fundsDelta == 0 && p.dueAfter == 0); }       // nothing due pays nothing
    { const ProgPayout p = ProgPayoutFor(false, 0); CHECK(p.fundsDelta == 0 && p.dueAfter == 0); }
    { const ProgPayout p = ProgPayoutFor(true, -9); CHECK(p.fundsDelta == 0 && p.dueAfter == 0); }      // a negative purse is clamped to none
    { const ProgPayout p = ProgPayoutFor(false, -9); CHECK(p.fundsDelta == 0 && p.dueAfter == 0); }
    if (g_fail) { std::printf("%d failure(s)\n", g_fail); return 1; }
    std::puts("ok");
    return 0;
}
