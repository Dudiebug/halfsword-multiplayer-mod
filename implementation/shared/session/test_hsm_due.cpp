// K9: deadline compares on the 32-bit ms clock stay right across the wrap.
#include "session/hsm_reconnect.h"
#include <cstdio>

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++g_fail; } } while (0)
using namespace hsm;

int main() {
    CHECK(DueAt(0, 0) && DueAt(10, 5) && !DueAt(5, 10));
    const uint32_t due = 0xFFFFFF00u + 1000u;                    // wraps to 744
    CHECK(!DueAt(0xFFFFFF00u, due) && !DueAt(0xFFFFFFFFu, due) && !DueAt(743u, due) && DueAt(744u, due) && DueAt(5000u, due));
    if (g_fail) { std::printf("%d failure(s)\n", g_fail); return 1; }
    std::puts("ok");
    return 0;
}
