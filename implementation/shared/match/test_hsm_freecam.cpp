// Spectator camera maths: the orbit (mouse yaw wraps, pitch and distance clamp), the wall pull-in, the free-camera
// integrator (speed caps, dt spikes, bounds at the corners) and the follow / free state (host switch, angle kept across
// target switches and mode changes, reset on respawn).
#include "match/hsm_freecam.h"
#include "match/hsm_spectate.h"
#include <cmath>
#include <cstdio>
#include <limits>
#include <string>

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++g_fail; } } while (0)
using namespace hsm::freecam;

static bool Near(double a, double b, double eps = 1e-6) { return std::fabs(a - b) <= eps; }
static const double kNaN = std::numeric_limits<double>::quiet_NaN();
static const double kInf = std::numeric_limits<double>::infinity();

static void TestYawWraps() {
    CHECK(Near(WrapYaw(190), -170) && Near(WrapYaw(-190), 170) && Near(WrapYaw(180), 180) && Near(WrapYaw(-180), 180));
    CHECK(Near(WrapYaw(720), 0) && Near(WrapYaw(0), 0) && WrapYaw(kNaN) == 0.0 && WrapYaw(kInf) == 0.0);
    Orbit o; o.yaw = 170;
    o = ApplyMouse(o, 100, 0, 0.2);                      // +20 degrees: past 180, comes round to -170
    CHECK(Near(o.yaw, -170));
    o = ApplyMouse(o, -100, 0, 0.2);
    CHECK(Near(o.yaw, 170));
    Orbit full; for (int i = 0; i < 100; ++i) full = ApplyMouse(full, 50, 0, 0.1);   // 500 degrees round and round
    CHECK(Near(full.yaw, WrapYaw(500)));
}

static void TestPitchClamp() {
    Orbit o;
    o = ApplyMouse(o, 0, 100000, 1.0);                    // far up
    CHECK(Near(o.pitch, kPitchMax));
    o = ApplyMouse(o, 0, -100000, 1.0);
    CHECK(Near(o.pitch, kPitchMin));
    o = ApplyMouse(o, 0, 100, 0.1);                       // 10 degrees up from -60
    CHECK(Near(o.pitch, -50));
    CHECK(kPitchMin == -60.0 && kPitchMax == 70.0);
}

static void TestBadMouse() {
    Orbit o; o.yaw = 10; o.pitch = 5;
    for (double bad : {kNaN, kInf, -kInf}) {
        Orbit r = ApplyMouse(o, bad, 0), s = ApplyMouse(o, 0, bad);
        CHECK(r.yaw == 10 && r.pitch == 5 && s.yaw == 10 && s.pitch == 5);
    }
    Orbit r = ApplyMouse(o, 10, 10, kNaN); CHECK(r.yaw == 10 && r.pitch == 5);
    r = ApplyMouse(o, 10, 10, 0.0); CHECK(r.yaw == 10 && r.pitch == 5);
    r = ApplyMouse(o, 10, 10, -1.0); CHECK(r.yaw == 10 && r.pitch == 5);
    r = ApplyMouse(o, 1e12, 0, 1.0); CHECK(Near(r.yaw, WrapYaw(10 + kMaxMouseCounts)));   // a glitch delta is capped
}

static void TestWheel() {
    Orbit o;
    CHECK(Near(ApplyWheel(o, 1).dist, kDistDefault - kWheelStepCm));          // wheel up: closer
    CHECK(Near(ApplyWheel(o, -2).dist, kDistDefault + 2 * kWheelStepCm));
    CHECK(Near(ApplyWheel(o, 1000).dist, kDistMin) && Near(ApplyWheel(o, -1000).dist, kDistMax));
    CHECK(ApplyWheel(o, kNaN).dist == kDistDefault && ApplyWheel(o, kInf).dist == kDistDefault);
    Orbit z = ApplyWheel(ApplyWheel(o, 1000), 1);                              // already at the near limit
    CHECK(Near(z.dist, kDistMin));
}

static void TestOrbitPosition() {
    Orbit o; o.yaw = 0; o.pitch = 0; o.dist = 200;
    Vec3 head{100, 50, 170};
    Vec3 p = OrbitCameraPos(head, o);                     // looking along +X: the camera is 200 behind, at -X
    CHECK(Near(p.x, -100) && Near(p.y, 50) && Near(p.z, 170));
    o.yaw = 90; p = OrbitCameraPos(head, o);              // looking along +Y: behind is -Y
    CHECK(Near(p.x, 100) && Near(p.y, -150));
    o.yaw = 0; o.pitch = -30; p = OrbitCameraPos(head, o);   // looking down: the camera is above
    CHECK(p.z > head.z && Near(p.z, 170 + 200 * 0.5, 1e-9));
    o.pitch = 30; p = OrbitCameraPos(head, o);            // looking up: below
    CHECK(p.z < head.z);
    // The camera looks at the head: head - pos is the view direction times the distance.
    o.yaw = 33; o.pitch = -12; p = OrbitCameraPos(head, o, 150);
    const Vec3 d = ViewDir(o.yaw, o.pitch);
    CHECK(Near(head.x - p.x, d.x * 150) && Near(head.y - p.y, d.y * 150) && Near(head.z - p.z, d.z * 150));
    CHECK(Near(std::sqrt(d.x * d.x + d.y * d.y + d.z * d.z), 1.0));
}

static void TestPullIn() {
    CHECK(Near(PullIn(260, -1), 260) && Near(PullIn(260, kNaN), 260) && Near(PullIn(260, kInf), 260));   // no hit
    CHECK(Near(PullIn(260, 100), 100 - kWallMargin));                                                      // wall at 1 m
    CHECK(Near(PullIn(260, 500), 260));                                                                     // wall behind the camera spot
    CHECK(Near(PullIn(260, 10), kPullMinCm));                                                               // wall right at the head
    CHECK(Near(PullIn(260, 0), kPullMinCm));
    CHECK(Near(PullIn(30, 5), 30));                                                                         // wanted nearer than the floor: stays
    CHECK(PullIn(kNaN, 100) == kDistDefault && PullIn(-5, 100) == kDistDefault);
    CHECK(Near(PullIn(260, 100, 0), 100));
    // easing: in at once, out gradually
    CHECK(Near(EaseDist(260, 80, 0.016), 80));
    const double out = EaseDist(80, 260, 0.016);
    CHECK(out > 80 && out < 260);
    CHECK(Near(EaseDist(80, 260, 0), 80));
    CHECK(EaseDist(80, 260, 5.0) < 260 && EaseDist(80, 260, 5.0) > out);                  // a spike is capped, never overshoots
    CHECK(EaseDist(kNaN, 200, 0.1) == 200 && EaseDist(100, kNaN, 0.1) == 100);
}

static Bounds Box() {
    const Vec3 pts[] = {{0, 0, 0}, {1000, 2000, 100}, {kNaN, 5, 5}};
    return MakeBounds(pts, 3);
}

static void TestBounds() {
    Bounds b = Box();
    CHECK(b.valid && Near(b.min.x, -kBoundsMarginCm) && Near(b.max.x, 1000 + kBoundsMarginCm));
    CHECK(Near(b.min.y, -kBoundsMarginCm) && Near(b.max.y, 2000 + kBoundsMarginCm));
    CHECK(Near(b.min.z, -kBoundsBelowCm) && Near(b.max.z, 100 + kBoundsAboveCm));         // the NaN point was skipped
    CHECK(!MakeBounds(nullptr, 0).valid);
    const Vec3 bad[] = {{kNaN, 0, 0}, {0, kInf, 0}};
    CHECK(!MakeBounds(bad, 2).valid);
    const Vec3 one[] = {{5, 6, 7}};
    CHECK(MakeBounds(one, 1).valid && Near(MakeBounds(one, 1).min.z, 7 - kBoundsBelowCm));
    // clamp at every corner
    for (int sx : {-1, 1}) for (int sy : {-1, 1}) for (int sz : {-1, 1}) {
        Vec3 far{sx * 1e9, sy * 1e9, sz * 1e9};
        Vec3 c = Clamp(far, b);
        CHECK(Near(c.x, sx < 0 ? b.min.x : b.max.x) && Near(c.y, sy < 0 ? b.min.y : b.max.y) && Near(c.z, sz < 0 ? b.min.z : b.max.z));
    }
    Vec3 in{10, 10, 10}; CHECK(Near(Clamp(in, b).x, 10));
    Bounds none; Vec3 any{1e9, 1e9, 1e9}; CHECK(Near(Clamp(any, none).x, 1e9));
}

static void TestStepFree() {
    Bounds b = Box();
    Pose p; p.pos = {500, 500, 50}; p.yaw = 0; p.pitch = 0;
    Pose q = StepFree(p, {1, 0, 0}, false, 0.1, b);                 // 0.1 s forward at 6 m/s: 60 cm along +X
    CHECK(Near(q.pos.x, 560) && Near(q.pos.y, 500) && Near(q.pos.z, 50));
    q = StepFree(p, {1, 0, 0}, true, 0.1, b);                       // Shift: 18 m/s
    CHECK(Near(q.pos.x, 680));
    q = StepFree(p, {-1, 0, 0}, false, 0.1, b); CHECK(Near(q.pos.x, 440));
    q = StepFree(p, {0, 1, 0}, false, 0.1, b);                      // right of +X is +Y
    CHECK(Near(q.pos.x, 500) && Near(q.pos.y, 560));
    q = StepFree(p, {0, 0, 1}, false, 0.1, b); CHECK(Near(q.pos.z, 110));     // Space: up
    q = StepFree(p, {0, 0, -1}, false, 0.1, b); CHECK(Near(q.pos.z, -10));    // Ctrl: down
    q = StepFree(p, {1, 1, 0}, false, 0.1, b);                      // a diagonal is no faster
    CHECK(Near(std::hypot(q.pos.x - 500, q.pos.y - 500), 60, 1e-9));
    q = StepFree(p, {5, 5, 5}, false, 0.1, b);                      // inputs held to -1..1, sum to unit length
    CHECK(Near(std::sqrt(std::pow(q.pos.x - 500, 2) + std::pow(q.pos.y - 500, 2) + std::pow(q.pos.z - 50, 2)), 60, 1e-9));
    q = StepFree(p, {kNaN, kInf, 0}, false, 0.1, b); CHECK(Near(q.pos.x, 500) && Near(q.pos.y, 500));
    Pose pitched = p;                                               // looking nearly straight up: forward is up
    pitched.pitch = 89; q = StepFree(pitched, {1, 0, 0}, false, 0.1, b); CHECK(q.pos.z > 50 + 59);
    Pose turned = p; turned.yaw = 90; q = StepFree(turned, {1, 0, 0}, false, 0.1, b); CHECK(Near(q.pos.y, 560) && Near(q.pos.x, 500));
}

static void TestDtSpike() {
    Bounds b = Box();
    Pose p; p.pos = {500, 500, 50};
    Pose spike = StepFree(p, {1, 0, 0}, true, 5.0, b);              // a 5 s hitch counts as kMaxStepS
    CHECK(Near(spike.pos.x, 500 + kFastSpeedCmS * kMaxStepS));
    CHECK(Near(StepFree(p, {1, 0, 0}, false, -1.0, b).pos.x, 500));  // negative time: no move
    CHECK(Near(StepFree(p, {1, 0, 0}, false, kNaN, b).pos.x, 500));
    CHECK(Near(StepFree(p, {1, 0, 0}, false, kInf, b).pos.x, 500));
    CHECK(Near(StepFree(p, {1, 0, 0}, false, 0.0, b).pos.x, 500));
    CHECK(kSpeedCmS == 600.0 && kFastSpeedCmS == 1800.0);
}

static void TestBoundsInFlight() {
    Bounds b = Box();
    Pose p; p.pos = {b.max.x - 10, b.max.y - 10, b.max.z - 10}; p.yaw = 45; p.pitch = 45;
    for (int i = 0; i < 100; ++i) p = StepFree(p, {1, 0, 1}, true, 0.1, b);
    CHECK(Near(p.pos.x, b.max.x) && Near(p.pos.y, b.max.y) && Near(p.pos.z, b.max.z));        // pinned in the far corner
    p.yaw = 225; p.pitch = -45;
    for (int i = 0; i < 400; ++i) p = StepFree(p, {1, 0, -1}, true, 0.1, b);
    CHECK(Near(p.pos.x, b.min.x) && Near(p.pos.y, b.min.y) && Near(p.pos.z, b.min.z));        // and the near one
    Pose out; out.pos = {1e9, -1e9, 1e9};                                                      // started outside: pulled in
    out = StepFree(out, {}, false, 0.016, b);
    CHECK(Near(out.pos.x, b.max.x) && Near(out.pos.y, b.min.y) && Near(out.pos.z, b.max.z));
    Pose nan; nan.pos = {kNaN, 0, 0};                                                          // not finite: the box's middle
    nan = StepFree(nan, {}, false, 0.016, b);
    CHECK(Near(nan.pos.x, (b.min.x + b.max.x) / 2) && Near(nan.pos.y, (b.min.y + b.max.y) / 2));
}

static void TestFreeLook() {
    Pose p;
    p = ApplyLook(p, 0, 100000, 1.0); CHECK(Near(p.pitch, kFreePitchLimit));
    p = ApplyLook(p, 0, -100000, 1.0); CHECK(Near(p.pitch, -kFreePitchLimit));
    p.yaw = 175; p = ApplyLook(p, 100, 0, 0.1); CHECK(Near(p.yaw, -175));
    Pose same = ApplyLook(p, kNaN, 0); CHECK(same.yaw == p.yaw);
}

static Pose Here() { Pose p; p.pos = {10, 20, 30}; p.yaw = 400; p.pitch = 120; return p; }

static void TestStateMachine() {
    SpectateCam c; Bounds b = Box();
    CHECK(c.allowed() && !c.free());
    CHECK(c.Toggle(Here(), b) && c.free());                          // F: follow -> free, from where the camera is
    CHECK(Near(c.pose().pos.x, 10) && Near(c.pose().yaw, 40) && Near(c.pose().pitch, kFreePitchLimit));   // yaw wrapped, pitch held
    CHECK(c.Toggle(Here(), b) && !c.free());                         // F again: back to following
}

static void TestDisabledByHost() {
    SpectateCam c; Bounds b = Box();
    c.SetAllowed(false);
    CHECK(!c.Toggle(Here(), b) && !c.free());                        // forbidden: stays following
    c.SetAllowed(true);
    CHECK(c.Toggle(Here(), b) && c.free());
    c.SetAllowed(false);                                             // the host turns it off while flying
    CHECK(!c.free());
    CHECK(!c.Toggle(Here(), b));
    SpectateCam d; Bounds none;                                      // no usable box: not offered
    CHECK(!d.Toggle(Here(), none) && !d.free());
    Pose bad = Here(); bad.pos.x = kNaN;
    CHECK(!d.Toggle(bad, b) && !d.free());
    // flying into a start outside the box is pulled inside
    Pose outside = Here(); outside.pos = {1e9, 0, 0};
    CHECK(d.Toggle(outside, b) && Near(d.pose().pos.x, b.max.x));
}

static void TestAngleKept() {
    SpectateCam c; Bounds b = Box();
    c.OrientOnce(100);                                               // the first target's facing
    CHECK(Near(c.orbit().yaw, 100));
    c.Mouse(100, 100, 0.1);                                          // yaw 110, pitch -5
    c.Wheel(2);
    const Orbit set = c.orbit();
    // switching target: the next fighter is faced from the current angle, not its own facing
    hsm::SpectateCycler cyc;
    cyc.Refresh(1, hsm::kNoTeam, {{2, hsm::kNoTeam, true}, {3, hsm::kNoTeam, true}});
    CHECK(*cyc.Target() == 2);
    cyc.Next();
    c.OrientOnce(-90);
    CHECK(*cyc.Target() == 3 && Near(c.orbit().yaw, set.yaw) && Near(c.orbit().pitch, set.pitch) && Near(c.orbit().dist, set.dist));
    // free and back keeps the angle, and the mouse moves the free pose, not the orbit
    CHECK(c.Toggle(Here(), b));
    c.Mouse(500, 0, 0.1);
    c.Wheel(5);                                                      // the wheel does nothing while flying
    CHECK(Near(c.orbit().yaw, set.yaw) && Near(c.orbit().dist, set.dist));
    CHECK(Near(c.pose().yaw, WrapYaw(40 + 50)));
    CHECK(c.Toggle(Here(), b));
    CHECK(Near(c.orbit().yaw, set.yaw) && Near(c.orbit().pitch, set.pitch) && Near(c.orbit().dist, set.dist));
    c.Mouse(10, 0, 0.1);                                             // back in follow, the mouse orbits again
    CHECK(Near(c.orbit().yaw, WrapYaw(set.yaw + 1)));
}

static void TestResetOnRespawn() {
    SpectateCam c; Bounds b = Box();
    c.OrientOnce(100); c.Mouse(300, 300, 0.1); c.Wheel(3);
    c.Toggle(Here(), b);
    c.Reset();
    CHECK(!c.free() && c.allowed());
    CHECK(Near(c.orbit().yaw, 0) && Near(c.orbit().pitch, kPitchDefault) && Near(c.orbit().dist, kDistDefault));
    c.OrientOnce(-45);                                               // the next death faces the new target afresh
    CHECK(Near(c.orbit().yaw, -45));
    c.SetAllowed(false); c.Reset(); CHECK(!c.allowed());             // the host's switch is not a respawn's to undo
}

static void TestFlyOnlyWhenFree() {
    SpectateCam c; Bounds b = Box();
    c.Fly({1, 0, 0}, false, 0.1, b);
    CHECK(Near(c.pose().pos.x, 0));                                  // following: no flight
    Pose p; p.pos = {500, 500, 50};
    c.Toggle(p, b); c.Fly({1, 0, 0}, true, 0.1, b);
    CHECK(Near(c.pose().pos.x, 680));
}

// Integration (Wave 2): the look swallow stays on the dead body only. A respawn (round reset, CHANGE LOADOUT) hands the player a new body
// whose look input must pass even while the spectate state has not been torn down yet.
static void TestSwallowLookOnDeadBodyOnly() {
    int dead = 0, born = 0;
    CHECK(SwallowLook(true, &dead, &dead));
    CHECK(!SwallowLook(false, &dead, &dead));                        // not watching: the game's look input is the player's
    CHECK(!SwallowLook(true, &born, &dead));                         // the new body after a respawn
    CHECK(!SwallowLook(true, &dead, nullptr));                       // no dead body named
    CHECK(!SwallowLook(true, nullptr, nullptr));
}

// The living player's free camera (#2624): on only while the fighter lives in a running match with the host's Free cam
// row on; the key always ends it, and so does the end of any of those; H hides the HUD only while it flies.
static LiveCamWorld Live() { LiveCamWorld w; w.matchLevel = w.fighting = w.alive = w.allowed = true; return w; }
static void TestLiveCam() {
    const Bounds b = Box();
    Pose p; p.pos = {500, 500, 50}; p.yaw = 190; p.pitch = 120;
    CHECK(LiveCamMay(Live()));
    for (int i = 0; i < 4; ++i) {                                    // each condition on its own
        LiveCamWorld w = Live();
        (i == 0 ? w.matchLevel : i == 1 ? w.fighting : i == 2 ? w.alive : w.allowed) = false;
        CHECK(!LiveCamMay(w));
        LiveCam c;
        CHECK(c.Toggle(w, p, b) == (i < 2 ? LiveCamAnswer::NoMatch : i == 2 ? LiveCamAnswer::Dead : LiveCamAnswer::NotAllowed) && !c.on());
        CHECK(c.Toggle(Live(), p, b) == LiveCamAnswer::On && c.on());
        CHECK(c.Keep(Live()) && c.on());                             // nothing changed: it flies on
        c.ToggleHud();
        CHECK(c.hudHidden() && !c.Keep(w) && !c.on() && !c.hudHidden());   // the condition ended: so does the flight, once
        CHECK(c.Keep(w) && !c.on());
    }
    { LiveCamWorld w = Live(); w.alive = false; w.fighting = false; LiveCam c; CHECK(c.Toggle(w, p, b) == LiveCamAnswer::NoMatch); }   // no match first
    { LiveCamWorld w = Live(); w.alive = false; w.allowed = false; LiveCam c; CHECK(c.Toggle(w, p, b) == LiveCamAnswer::Dead); }       // then the death
    LiveCam c;
    CHECK(!c.on() && !c.hudHidden());
    c.ToggleHud(); CHECK(!c.hudHidden());                            // H does nothing on the ground
    c.Look(100, 100); c.Fly({1, 0, 0}, false, 0.1, b);
    CHECK(Near(c.pose().pos.x, 0) && Near(c.pose().yaw, 0));         // nor do the mouse and the keys
    // a flight: from where the camera is, the view wrapped and clamped, inside the box
    CHECK(c.Toggle(Live(), p, b) == LiveCamAnswer::On && Near(c.pose().yaw, -170) && Near(c.pose().pitch, kFreePitchLimit) && Near(c.pose().pos.x, 500));
    Pose far = p; far.pos = {99999, -99999, 99999};
    { LiveCam f; CHECK(f.Toggle(Live(), far, b) == LiveCamAnswer::On && Near(f.pose().pos.x, b.max.x) && Near(f.pose().pos.y, b.min.y) && Near(f.pose().pos.z, b.max.z)); }
    const double yaw0 = c.pose().yaw;
    c.Look(10, 0);
    CHECK(Near(c.pose().yaw, yaw0 + 10 * kSensitivity));
    c.Fly({0, 0, 1}, false, 0.1, b);
    CHECK(c.pose().pos.z > 50 && c.pose().pos.z <= b.max.z);
    for (int i = 0; i < 1000; ++i) c.Fly({1, 1, 1}, true, 0.1, b);
    CHECK(c.pose().pos.x <= b.max.x && c.pose().pos.y <= b.max.y && c.pose().pos.z <= b.max.z && c.pose().pos.x >= b.min.x);
    c.ToggleHud(); CHECK(c.hudHidden());
    c.ToggleHud(); CHECK(!c.hudHidden() && c.on());
    c.ToggleHud();
    CHECK(c.Toggle(Live(), p, b) == LiveCamAnswer::Off && !c.on() && !c.hudHidden());   // the key ends it, whatever the world says
    CHECK(c.Toggle(Live(), p, b) == LiveCamAnswer::On && !c.hudHidden());               // a new flight shows the HUD again
    { LiveCamWorld w = Live(); w.allowed = false; CHECK(c.Toggle(w, p, b) == LiveCamAnswer::Off); }   // off always works
    // no camera to start from, or no box: refused
    Pose bad = p; bad.yaw = kNaN;
    CHECK(c.Toggle(Live(), bad, b) == LiveCamAnswer::NoCamera && !c.on());
    bad = p; bad.pos.x = kInf;
    CHECK(c.Toggle(Live(), bad, b) == LiveCamAnswer::NoCamera);
    bad = p; bad.pitch = kNaN;
    CHECK(c.Toggle(Live(), bad, b) == LiveCamAnswer::NoCamera);
    CHECK(c.Toggle(Live(), p, Bounds{}) == LiveCamAnswer::NoCamera && !c.on());
    c.Toggle(Live(), p, b); c.End();
    CHECK(!c.on());
    // what the player is told: every answer but the dead player's (the spectator's camera has its own F)
    for (LiveCamAnswer a : {LiveCamAnswer::On, LiveCamAnswer::Off, LiveCamAnswer::NoMatch, LiveCamAnswer::NotAllowed, LiveCamAnswer::NoCamera}) CHECK(*LiveCamLine(a));
    CHECK(!*LiveCamLine(LiveCamAnswer::Dead) && std::string(LiveCamLine(LiveCamAnswer::Off)) == "Free camera off");
    CHECK(std::string(LiveCamLine(LiveCamAnswer::On)).find("H hides the HUD") != std::string::npos);
    CHECK(std::string(LiveCamLine(LiveCamAnswer::NotAllowed)).find("host") != std::string::npos && std::string(LiveCamLine(LiveCamAnswer::NoMatch)).find("match") != std::string::npos);
}

// The body's own input while the camera flies: axes read zero, presses are dropped, releases pass; all of it passes on the ground.
static void TestLiveCamInput() {
    CHECK(LiveCamInput(true, true, false) == HeldInput::Zero && LiveCamInput(true, true, true) == HeldInput::Zero);
    CHECK(LiveCamInput(true, false, true) == HeldInput::Drop && LiveCamInput(true, false, false) == HeldInput::Pass);
    for (bool axis : {false, true}) for (bool pressed : {false, true}) CHECK(LiveCamInput(false, axis, pressed) == HeldInput::Pass);
}

int main() {
    TestLiveCam(); TestLiveCamInput();
    TestSwallowLookOnDeadBodyOnly();
    TestYawWraps();
    TestPitchClamp();
    TestBadMouse();
    TestWheel();
    TestOrbitPosition();
    TestPullIn();
    TestBounds();
    TestStepFree();
    TestDtSpike();
    TestBoundsInFlight();
    TestFreeLook();
    TestStateMachine();
    TestDisabledByHost();
    TestAngleKept();
    TestResetOnRespawn();
    TestFlyOnlyWhenFree();
    std::printf(g_fail ? "%d FAILED\n" : "all freecam tests passed\n", g_fail);
    return g_fail ? 1 : 0;
}
