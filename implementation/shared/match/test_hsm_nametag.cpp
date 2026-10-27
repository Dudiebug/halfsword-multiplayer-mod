#include "match/hsm_nametag.h"
#include <cmath>
#include <cstdio>

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++g_fail; } } while (0)
using namespace hsm;

int main() {
    NameTagView v = NameTagLayout(true, 960, 400, 1920, 1080, 1.f, 500, true, kNoTeam, Format::FreeForAll);
    CHECK(v.visible && v.x == 960 && v.y == 400 && v.fontSize == 22.f && v.alpha == 1.f);
    CHECK(v.ink.r == style::kBone.r && v.ink.g == style::kBone.g);
    v = NameTagLayout(true, 960, 400, 1920, 1080, 2.f, 500, true, kNoTeam, Format::Duel);        // DPI scale 2
    CHECK(v.visible && v.x == 480 && v.y == 200);
    CHECK(!NameTagLayout(false, 960, 400, 1920, 1080, 1.f, 500, true, kNoTeam, Format::Duel).visible);   // behind the camera
    CHECK(!NameTagLayout(true, 960, 400, 1920, 1080, 1.f, 500, false, kNoTeam, Format::Duel).visible);   // dead
    CHECK(!NameTagLayout(true, 960, 400, 1920, 1080, 1.f, kNameTagFarCm + 1, true, kNoTeam, Format::Duel).visible);
    CHECK(NameTagLayout(true, 960, 400, 1920, 1080, 1.f, kNameTagFarCm, true, kNoTeam, Format::Duel).visible);
    CHECK(!NameTagLayout(true, -1, 400, 1920, 1080, 1.f, 500, true, kNoTeam, Format::Duel).visible);     // off screen
    CHECK(!NameTagLayout(true, 960, 1081, 1920, 1080, 1.f, 500, true, kNoTeam, Format::Duel).visible);
    CHECK(!NameTagLayout(true, NAN, 400, 1920, 1080, 1.f, 500, true, kNoTeam, Format::Duel).visible);
    CHECK(!NameTagLayout(true, 960, 400, 1920, 1080, 0.f, 500, true, kNoTeam, Format::Duel).visible);    // no scale
    NameTagView far = NameTagLayout(true, 960, 400, 1920, 1080, 1.f, kNameTagFarCm, true, kNoTeam, Format::Duel);
    CHECK(far.fontSize < v.fontSize && far.alpha < v.alpha && far.alpha > 0.3f);
    NameTagView t1 = NameTagLayout(true, 960, 400, 1920, 1080, 1.f, 500, true, 1, Format::TwoTeams);
    CHECK(t1.ink.b == style::TeamInk(1).b && t1.ink.r == style::TeamInk(1).r);
    NameTagView ffa = NameTagLayout(true, 960, 400, 1920, 1080, 1.f, 500, true, 1, Format::FreeForAll);   // no teams: bone
    CHECK(ffa.ink.r == style::kBone.r);
    std::printf(g_fail ? "%d FAILED\n" : "all name tag tests passed\n", g_fail);
    return g_fail ? 1 : 0;
}
