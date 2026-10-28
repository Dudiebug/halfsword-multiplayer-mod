// Unit tests for hsm_paintdefer.h (fake clock). Run via implementation/shared/run_tests.sh test_hsm_paintdefer.
#include "combat/hsm_paintdefer.h"
#include <cstdio>

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++g_fail; } } while (0)

using namespace hsm::combat;

int main() {
    auto never = [](uint64_t) { return false; };
    auto always = [](uint64_t) { return true; };
    {   // a free piece waits the quiet time, then goes
        PaintDefer d; std::vector<PaintRelease> out;
        CHECK(d.Add(7, 1000));
        d.Poll(1000, never, out); CHECK(out.empty());
        d.Poll(1000 + kPaintDeferQuietMs - 1, never, out); CHECK(out.empty());
        d.Poll(1000 + kPaintDeferQuietMs, never, out);
        CHECK(out.size() == 1 && out[0].key == 7 && !out[0].timedOut && out[0].waitedMs == kPaintDeferQuietMs);
        CHECK(d.Size() == 0 && !d.Has(7));
    }
    {   // a busy look restarts the clock
        PaintDefer d; std::vector<PaintRelease> out;
        d.Add(1, 0);
        d.Poll(0, never, out);
        d.Poll(60, always, out);
        d.Poll(120, never, out); CHECK(out.empty());              // free again since 120
        d.Poll(120 + kPaintDeferQuietMs - 1, never, out); CHECK(out.empty());
        d.Poll(120 + kPaintDeferQuietMs, never, out); CHECK(out.size() == 1 && !out[0].timedOut);
    }
    {   // a later free look does not restart a running quiet clock
        PaintDefer d; std::vector<PaintRelease> out;
        d.Add(1, 0);
        d.Poll(10, never, out);
        d.Poll(60, never, out); CHECK(out.empty());
        d.Poll(10 + kPaintDeferQuietMs, never, out); CHECK(out.size() == 1);
    }
    {   // always busy: released by the time limit, flagged
        PaintDefer d; std::vector<PaintRelease> out;
        d.Add(1, 500);
        d.Poll(500 + kPaintDeferMaxMs - 1, always, out); CHECK(out.empty());
        d.Poll(500 + kPaintDeferMaxMs, always, out);
        CHECK(out.size() == 1 && out[0].timedOut && out[0].waitedMs == kPaintDeferMaxMs);
        CHECK(d.Size() == 0);
    }
    {   // never polled until late: the limit counts from parking, not from the first look
        PaintDefer d; std::vector<PaintRelease> out;
        d.Add(1, 0);
        d.Poll(kPaintDeferMaxMs + 50, always, out);
        CHECK(out.size() == 1 && out[0].timedOut);
    }
    {   // each piece on its own clock; release order is parking order
        PaintDefer d; std::vector<PaintRelease> out;
        d.Add(1, 0); d.Add(2, 0); d.Add(3, 0);
        auto only2busy = [](uint64_t k) { return k == 2; };
        d.Poll(0, only2busy, out);
        d.Poll(kPaintDeferQuietMs, only2busy, out);
        CHECK(out.size() == 2 && out[0].key == 1 && out[1].key == 3);
        CHECK(d.Size() == 1 && d.Has(2) && !d.Has(1));
    }
    {   // duplicates refused; capacity
        PaintDefer d;
        CHECK(d.Add(5, 0)); CHECK(!d.Add(5, 10));
        for (uint64_t k = 100; d.Size() < kPaintDeferMax; ++k) CHECK(d.Add(k, 0));
        CHECK(!d.Add(999, 0));
        CHECK(d.Size() == kPaintDeferMax);
        std::vector<PaintRelease> out;
        d.Poll(1, never, out); d.Poll(1 + kPaintDeferQuietMs, never, out);
        CHECK(out.size() == kPaintDeferMax && d.Size() == 0);
        CHECK(d.Add(999, 0));   // room again
    }
    {   // Drop and Clear
        PaintDefer d;
        d.Add(1, 0); d.Add(2, 0);
        d.Drop(1); CHECK(!d.Has(1) && d.Has(2) && d.Size() == 1);
        d.Drop(42); CHECK(d.Size() == 1);
        d.Clear(); CHECK(d.Size() == 0);
        std::vector<PaintRelease> out;
        d.Poll(99999, never, out); CHECK(out.empty());
    }
    if (g_fail == 0) std::printf("test_hsm_paintdefer: ok\n");
    return g_fail ? 1 : 0;
}
