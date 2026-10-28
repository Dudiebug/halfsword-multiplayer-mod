// Unit tests for hsm_paintgate.h (fake clock). Run via implementation/shared/run_tests.sh test_hsm_paintgate.
#include "combat/hsm_paintgate.h"
#include <cstdio>

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++g_fail; } } while (0)

using namespace hsm::combat;

int main() {
    {   // never observed: not quiet
        PaintGate g;
        CHECK(!g.Quiet(1, 10000));
    }
    {   // idle must last the full quiet time
        PaintGate g;
        g.Observe(1, false, 1000);
        CHECK(!g.Quiet(1, 1000));
        CHECK(!g.Quiet(1, 1249));
        CHECK(g.Quiet(1, 1250));
        g.Observe(1, false, 1300);   // a later idle look does not restart the clock
        CHECK(g.Quiet(1, 1300));
    }
    {   // one busy look breaks it: negative control for "idle at one instant"
        PaintGate g;
        g.Observe(1, false, 1000);
        g.Observe(1, true, 1200);
        CHECK(!g.Quiet(1, 1300));
        g.Observe(1, false, 1400);
        CHECK(!g.Quiet(1, 1649));
        CHECK(g.Quiet(1, 1650));
    }
    {   // busy stays busy
        PaintGate g;
        g.Observe(2, true, 0);
        CHECK(!g.Quiet(2, 99999));
    }
    {   // per fighter
        PaintGate g;
        g.Observe(1, false, 0);
        g.Observe(2, true, 0);
        CHECK(g.Quiet(1, 300));
        CHECK(!g.Quiet(2, 300));
    }
    {   // blood hold after an armour change
        PaintGate g;
        CHECK(!g.BloodHeld(1, 0));
        g.NoteArmour(1, 5000);
        CHECK(g.BloodHeld(1, 5000));
        CHECK(g.BloodHeld(1, 5999));
        CHECK(!g.BloodHeld(1, 6000));
        CHECK(!g.BloodHeld(2, 5000));
        g.NoteArmour(1, 5900);   // a second change extends it
        CHECK(g.BloodHeld(1, 6500));
    }
    {   // forget
        PaintGate g;
        g.NoteArmour(1, 0);
        g.Observe(1, false, 0);
        g.Forget(1);
        CHECK(!g.BloodHeld(1, 1));
        CHECK(!g.Quiet(1, 9999));
    }
    if (g_fail == 0) std::printf("test_hsm_paintgate: all passed\n");
    return g_fail ? 1 : 0;
}
