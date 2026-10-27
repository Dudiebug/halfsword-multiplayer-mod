// Mesh pure parts (session/hsm_mesh.h).
#include "session/hsm_mesh.h"
#include <cstdio>

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++g_fail; } } while (0)
using namespace hsm;

int main() {
    MeshHearing h; uint8_t m = 0xAA;
    CHECK(h.Mask(0) == 0 && !h.Heard(1, 0));
    CHECK(!h.TakeChange(5000, &m));                          // nothing heard, nothing sent: no report
    h.Note(2, 10000); h.Note(3, 10100);
    CHECK(h.Heard(2, 11000) && h.Mask(11000) == 0x0C);
    CHECK(h.Heard(2, 12000) && !h.Heard(2, 12001));          // held 2000 ms
    CHECK(h.TakeChange(11000, &m) && m == 0x0C);
    CHECK(!h.TakeChange(11500, &m));                         // unchanged
    CHECK(!h.TakeChange(11900, &m));                         // still unchanged
    CHECK(h.TakeChange(12050, &m) && m == 0x08);             // slot 2 lapsed (gap since 11000 passed); slot 3 still held
    h.Note(2, 12100); CHECK(!h.TakeChange(12500, &m));       // changed back, but only 450 ms since the last report
    CHECK(h.TakeChange(13100, &m) && m == 0x04);             // slot 3 lapsed at 12100; slot 2 heard again
    CHECK(h.TakeChange(20000, &m) && m == 0);                // everything lapsed
    h.Note(9, 1); CHECK(h.Mask(1) == 0);                     // out of range ignored
    h.Clear(); CHECK(h.Mask(20000) == 0);

    MeshMasks k;
    k.Set(1, 0xFF);                                          // own bit and bits >= 6 stripped
    CHECK(!k.Hears(1, 1) && k.Hears(1, 0) && k.Hears(1, 5) && !k.Hears(1, 6));
    k.Set(6, 0xFF); CHECK(!k.Hears(6, 0));
    std::vector<uint8_t> conn = {0, 1, 2, 3};
    k.Clear(); k.Set(2, 0x02);                               // slot 2 hears slot 1 directly
    auto t = RelayTargets(1, conn, k);
    CHECK(t.size() == 1 && t[0] == 3);                       // 2 skipped, host and sender never
    t = RelayTargets(3, conn, k); CHECK(t.size() == 2);      // slot 2 does not hear 3: relayed
    CHECK(RelayTargets(1, conn) .size() == 2);               // the plain overload is unchanged
    k.Reset(1);                                              // slot 1 left: nobody hears it
    t = RelayTargets(1, conn, k); CHECK(t.size() == 2);
    k.Set(2, 0x08); k.Reset(2); CHECK(!k.Hears(2, 3));
    MeshMasks z; CHECK(RelayTargets(1, conn, z) == RelayTargets(1, conn));   // mask 0 == 0.6.5 behaviour

    if (g_fail) { std::printf("%d failures\n", g_fail); return 1; }
    std::puts("ok");
    return 0;
}
