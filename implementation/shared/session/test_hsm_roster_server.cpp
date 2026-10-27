// role=server: the host's slot 0 is reserved, clients take slots 1..5 and the roster holds five at most.
#include "session/hsm_roster.h"
#include <cstdio>

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++g_fail; } } while (0)
using namespace hsm;

int main() {
    Roster r; r.ReserveHostSlot(true);
    CHECK(r.Add(11, "a") && *r.Add(11, "a") == 1);          // first client: slot 1, entity 2
    CHECK(r.ByEntity(2) && !r.BySlot(0) && !r.ByEntity(1));
    for (uint64_t k = 12; k <= 15; ++k) CHECK(r.Add(k, "x").has_value());
    CHECK(r.size() == 5 && !r.Add(99, "full"));             // six fighters of a normal host, five here
    CHECK(r.Remove(13)); CHECK(*r.Add(77, "y") == 3);        // freed slots are reused, lowest first
    r.Clear(); CHECK(*r.Add(1, "z") == 1);                   // the reservation survives Clear
    Roster plain; CHECK(*plain.Add(5, "host") == 0);         // normal roles unchanged
    if (!g_fail) std::printf("test_hsm_roster_server: ok\n");
    return g_fail ? 1 : 0;
}
