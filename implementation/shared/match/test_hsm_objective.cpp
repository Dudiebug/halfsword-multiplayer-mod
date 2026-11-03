// The objective layer: validity, the HUD markers (where, text, ink, layout) and the MatchInfo round trip with objectives.
#include "match/hsm_ctf.h"
#include "match/hsm_hud_scene.h"
#include <cmath>
#include <cstdio>

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++g_fail; } } while (0)
using namespace hsm;

static Objective Flag(int8_t owner, ObjectiveState s, uint8_t carrier = 0, float x = 10, float y = 20, float z = 30, uint32_t timer = 0) {
    Objective o; o.owner = owner; o.state = s; o.carrier = carrier; o.x = x; o.y = y; o.z = z; o.timerMs = timer; return o;
}

static void TestValid() {
    CHECK(ValidObjective(Flag(0, ObjectiveState::Home)));
    CHECK(ValidObjective(Flag(1, ObjectiveState::Carried, 3)));
    CHECK(ValidObjective(Flag(1, ObjectiveState::Dropped, 0, 0, 0, 0, 20000)));
    CHECK(ValidObjective(Flag(kNoTeam, ObjectiveState::Home)));
    CHECK(!ValidObjective(Flag(8, ObjectiveState::Home)) && !ValidObjective(Flag(-2, ObjectiveState::Home)));
    CHECK(!ValidObjective(Flag(0, ObjectiveState::Carried, 0)));                // carried by nobody
    CHECK(!ValidObjective(Flag(0, ObjectiveState::Home, 3)) && !ValidObjective(Flag(0, ObjectiveState::Dropped, 3)));   // a carrier while not carried
    CHECK(!ValidObjective(Flag(0, ObjectiveState::Home, 0, 0, 0, 0, 5)) && !ValidObjective(Flag(0, ObjectiveState::Carried, 3, 0, 0, 0, 5)));   // a timer while not dropped
    CHECK(!ValidObjective(Flag(0, ObjectiveState::Dropped, 0, 0, 0, 0, kMaxObjectiveTimerMs + 1)));
    CHECK(!ValidObjective(Flag(0, ObjectiveState(3))));
    Objective k = Flag(0, ObjectiveState::Home); k.kind = ObjectiveKind(1);
    CHECK(!ValidObjective(k));
    CHECK(!ValidObjective(Flag(0, ObjectiveState::Home, 0, NAN)) && !ValidObjective(Flag(0, ObjectiveState::Home, 0, 0, INFINITY)));
    CHECK(!ValidObjective(Flag(0, ObjectiveState::Home, 0, 0, 0, 2e7f)));
}

static void TestMarkers() {
    const std::vector<Objective> objs = {Flag(0, ObjectiveState::Home, 0, 100, 200, 10), Flag(1, ObjectiveState::Carried, 4, 0, 0, 0),
                                         Flag(1, ObjectiveState::Dropped, 0, 5, 6, 7, 13200)};
    HeadLookup head = [](uint8_t e, double& x, double& y, double& z) { if (e != 4) return false; x = 1; y = 2; z = 3; return true; };
    std::vector<MarkerSrc> m = MarkersFor(objs, head);
    CHECK(m.size() == 3);
    CHECK(m[0].label == "RED FLAG" && m[0].wx == 100 && m[0].wy == 200 && m[0].wz == 10 + kMarkerAboveGroundCm && m[0].state == ObjectiveState::Home);
    CHECK(m[1].label == "BLUE FLAG" && m[1].wx == 1 && m[1].wy == 2 && m[1].wz == 3 + kMarkerAboveHeadCm && m[1].carrier == 4);   // rides on the carrier
    CHECK(m[2].wx == 5 && m[2].wz == 7 + kMarkerAboveGroundCm && m[2].timerMs == 13200);
    // A carrier the lookup cannot place gets no marker; invalid objectives none; no lookup, no carried marker.
    CHECK(MarkersFor({Flag(1, ObjectiveState::Carried, 9)}, head).empty());
    CHECK(MarkersFor({Flag(1, ObjectiveState::Carried, 4)}, nullptr).empty());
    CHECK(MarkersFor({Flag(9, ObjectiveState::Home)}, head).empty());
    CHECK(MarkersFor({}, head).empty());

    CHECK(MarkerText(m[0], 1234.f) == "RED FLAG  12 m");
    CHECK(MarkerText(m[1], 1234.f) == "BLUE FLAG CARRIED");
    CHECK(MarkerText(m[2], 4000.f) == "BLUE FLAG 14 s  40 m");
    CHECK(MarkerText(m[0], -1.f) == "RED FLAG");

    MarkerView v = MarkerLayout(m[2], true, 960, 400, 1920, 1080, 2.f, 500);
    CHECK(v.visible && v.x == 480 && v.y == 200 && v.fontSize == 24.f && v.alpha == 1.f && v.text == "BLUE FLAG 14 s  5 m");
    CHECK(v.ink.b == style::TeamInk(1).b && v.ink.r == style::TeamInk(1).r);
    MarkerView home = MarkerLayout(m[0], true, 960, 400, 1920, 1080, 1.f, 500);
    CHECK(home.visible && home.alpha < v.alpha && home.alpha > 0.f);            // a home flag is dimmer
    MarkerView far = MarkerLayout(m[2], true, 960, 400, 1920, 1080, 1.f, kMarkerFarCm);
    CHECK(far.visible && far.fontSize < 24.f && far.alpha < 1.f && far.alpha > 0.3f);
    CHECK(!MarkerLayout(m[2], true, 960, 400, 1920, 1080, 1.f, kMarkerFarCm + 1).visible);
    CHECK(!MarkerLayout(m[2], false, 960, 400, 1920, 1080, 1.f, 500).visible);   // behind the camera
    CHECK(!MarkerLayout(m[2], true, -1, 400, 1920, 1080, 1.f, 500).visible && !MarkerLayout(m[2], true, 960, 1081, 1920, 1080, 1.f, 500).visible);
    CHECK(!MarkerLayout(m[2], true, NAN, 400, 1920, 1080, 1.f, 500).visible && !MarkerLayout(m[2], true, 960, 400, 1920, 1080, 0.f, 500).visible);
    CHECK(!MarkerLayout(m[2], true, 960, 400, 1920, 1080, 1.f, NAN).visible && !MarkerLayout(m[2], true, 960, 400, 1920, 1080, 1.f, -5).visible);
}

static MatchInfo CtfInfo() {
    MatchInfo m; m.format = uint8_t(Format::CaptureTheFlag); m.scoreLimit = 3; m.phase = uint8_t(MatchPhase::Live);
    m.fighters.push_back({1, 0, 0, 0, "Ann", 0, true, false, 0, 0});
    m.fighters.push_back({2, 1, 0, 0, "Bo", 0, true, false, 0, 0});
    m.scores = {{0, 2}, {1, 1}};
    m.objectives = {Flag(0, ObjectiveState::Carried, 2, 100, -200, 30), Flag(1, ObjectiveState::Dropped, 0, 5.5f, 6.5f, 7.5f, 14000)};
    return m;
}

static void TestMatchInfoWire() {
    MatchInfo m = CtfInfo();
    auto b = Encode(m); MatchInfo o;
    CHECK(Decode(b.data(), b.size(), o) && o.objectives == m.objectives && o.scores.size() == 2 && o.scores[0].score == 2 && o.fighters.size() == 2);
    CHECK(o.objectives[1].timerMs == 14000 && o.objectives[0].carrier == 2 && o.objectives[1].x == 5.5f);
    for (size_t cut = 0; cut < b.size(); ++cut) { MatchInfo t; CHECK(!Decode(b.data(), cut, t)); }
    // Other formats carry none: one count byte of zero.
    MatchInfo plain; plain.format = uint8_t(Format::TwoTeams);
    auto pb = Encode(plain); MatchInfo po;
    CHECK(Decode(pb.data(), pb.size(), po) && po.objectives.empty() && pb.size() + 2 * 18 == Encode([&] { MatchInfo q = plain; q.objectives = {Flag(0, ObjectiveState::Home), Flag(1, ObjectiveState::Home)}; return q; }()).size());
    // Bad values.
    auto bad = [&](auto&& f) { MatchInfo x = CtfInfo(); f(x); auto bb = Encode(x); MatchInfo t; return !Decode(bb.data(), bb.size(), t); };
    CHECK(bad([](MatchInfo& x) { x.objectives[0].carrier = 0; }));                 // carried by nobody
    CHECK(bad([](MatchInfo& x) { x.objectives[1].state = ObjectiveState(3); }));
    CHECK(bad([](MatchInfo& x) { x.objectives[1].owner = 9; }));
    CHECK(bad([](MatchInfo& x) { x.objectives[1].timerMs = kMaxObjectiveTimerMs + 1; }));
    CHECK(bad([](MatchInfo& x) { x.objectives[0].x = INFINITY; }));
    CHECK(bad([](MatchInfo& x) { x.objectives[0].kind = ObjectiveKind(5); }));
    CHECK(!bad([](MatchInfo& x) { x.objectives.clear(); }));
    // Too many objectives are cut to the cap on the way out, and a count over the cap is refused on the way in.
    MatchInfo many = CtfInfo(); many.objectives.assign(kMaxObjectives + 3, Flag(0, ObjectiveState::Home));
    auto mb = Encode(many); MatchInfo mo;
    CHECK(Decode(mb.data(), mb.size(), mo) && mo.objectives.size() == kMaxObjectives);
    auto cb = Encode(CtfInfo()); const size_t countAt = cb.size() - 5 - 2 * 18 - 1;   // 5: the queue fields after the objectives (two counts, three bytes)
    cb[countAt] = uint8_t(kMaxObjectives + 1); MatchInfo co;
    CHECK(!Decode(cb.data(), cb.size(), co));
    // Trailing bytes are refused.
    auto tb = Encode(CtfInfo()); tb.push_back(0); MatchInfo to; CHECK(!Decode(tb.data(), tb.size(), to));
}

static void TestExtrapolateAndScene() {
    MatchInfo m = CtfInfo();
    MatchInfo e = Extrapolate(m, 1000, 4000);
    CHECK(e.objectives[1].timerMs == 11000 && e.objectives[0].timerMs == 0);
    CHECK(Extrapolate(m, 1000, 30000).objectives[1].timerMs == 0);
    LocalHud l; l.me = 1; l.infoAtMs = 1000;
    HudScene s = BuildHudScene(m, l, 4000);
    CHECK(s.flags.size() == 2 && s.flags[0].text == "RED FLAG: taken by Bo" && s.flags[1].text == "BLUE FLAG: dropped 11 s");
    CHECK(s.flags[0].color.b == style::TeamInk(0).b && s.flags[1].color.b == style::TeamInk(1).b);
    CHECK(s.pill.text == "FIRST TO 3 CAPTURES");
    m.scoreLimit = 1; CHECK(BuildHudScene(m, l, 1000).pill.text == "FIRST TO 1 CAPTURE");
    m.phase = uint8_t(MatchPhase::Results); m.winner = 0;
    s = BuildHudScene(m, l, 1000);
    CHECK(s.flags.empty() && s.pill.text == "RED WINS");                       // results: the winner, no flag lines
    MatchInfo other = CtfInfo(); other.format = uint8_t(Format::TwoTeams);
    CHECK(BuildHudScene(other, l, 1000).flags.empty());                      // another format never shows flags
    // The feed row of a flag event.
    FeedRow r = MakeCtfFeedRow(CtfInfo(), {CtfEventKind::Captured, 1, 1}, 1);
    CHECK(r.mine && r.spans.size() == 3 && r.spans[0].text == "Ann" && r.spans[1].text == " captured the " && r.spans[2].text == "BLUE flag");
    CHECK(r.spans[0].color.b == style::TeamInk(0).b && r.spans[2].color.b == style::TeamInk(1).b);
    FeedRow t = MakeCtfFeedRow(CtfInfo(), {CtfEventKind::Expired, 0, 0}, 1);
    CHECK(!t.mine && t.spans.size() == 3 && t.spans[1].text == "RED flag" && t.spans[2].text == " went home");
    CHECK(!MakeCtfFeedRow(CtfInfo(), {CtfEventKind::Taken, 0, 2}, 1).mine);
    // The banner for sudden death.
    BannerTracker bt; MatchInfo live = CtfInfo(); live.phase = uint8_t(MatchPhase::Live);
    bt.Observe(live, 1, 0);
    live.phase = uint8_t(MatchPhase::SuddenDeath); bt.Observe(live, 1, 100);
    CHECK(bt.shot().text == "SUDDEN DEATH" && bt.shot().sub == "NEXT CAPTURE WINS");
}

int main() {
    TestValid();
    TestMarkers();
    TestMatchInfoWire();
    TestExtrapolateAndScene();
    std::printf(g_fail ? "%d FAILED\n" : "all objective tests passed\n", g_fail);
    return g_fail ? 1 : 0;
}
