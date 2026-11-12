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

// The game's photo mode in a match (#2624): the key opens it only on a living fighter in a running match with the host's
// Free cam row on, no mod screen up and its pause switched off; the mod closes it when the first three end.
static PhotoWorld Shot() { PhotoWorld w; w.matchLevel = w.fighting = w.alive = w.allowed = w.ready = true; return w; }
static void TestPhotoMode() {
    CHECK(PhotoKeyAnswer(Shot()) == PhotoKey::Open && PhotoEnds(Shot()) == PhotoEnd::Keep);
    { PhotoWorld w = Shot(); w.matchLevel = false; CHECK(PhotoKeyAnswer(w) == PhotoKey::NoMatch && PhotoEnds(w) == PhotoEnd::MatchOver); }
    { PhotoWorld w = Shot(); w.fighting = false; CHECK(PhotoKeyAnswer(w) == PhotoKey::NoMatch && PhotoEnds(w) == PhotoEnd::MatchOver); }
    { PhotoWorld w = Shot(); w.alive = false; CHECK(PhotoKeyAnswer(w) == PhotoKey::Dead && PhotoEnds(w) == PhotoEnd::Down); }
    { PhotoWorld w = Shot(); w.allowed = false; CHECK(PhotoKeyAnswer(w) == PhotoKey::NotAllowed && PhotoEnds(w) == PhotoEnd::HostOff); }
    // a mod screen or a pause switch that would not go off refuses the key; neither closes a photo mode that is open
    { PhotoWorld w = Shot(); w.busy = true; CHECK(PhotoKeyAnswer(w) == PhotoKey::Busy && PhotoEnds(w) == PhotoEnd::Keep); }
    { PhotoWorld w = Shot(); w.ready = false; CHECK(PhotoKeyAnswer(w) == PhotoKey::NotReady && PhotoEnds(w) == PhotoEnd::Keep); }
    // the first reason in this order: no match, the death, the host, a mod screen, the switches
    { PhotoWorld w = Shot(); w.alive = false; w.fighting = false; CHECK(PhotoKeyAnswer(w) == PhotoKey::NoMatch && PhotoEnds(w) == PhotoEnd::MatchOver); }
    { PhotoWorld w = Shot(); w.alive = false; w.allowed = false; CHECK(PhotoKeyAnswer(w) == PhotoKey::Dead && PhotoEnds(w) == PhotoEnd::Down); }
    { PhotoWorld w = Shot(); w.allowed = false; w.busy = true; CHECK(PhotoKeyAnswer(w) == PhotoKey::NotAllowed); }
    { PhotoWorld w = Shot(); w.busy = true; w.ready = false; CHECK(PhotoKeyAnswer(w) == PhotoKey::Busy); }
    CHECK(PhotoKeyAnswer(PhotoWorld{}) == PhotoKey::NoMatch && PhotoEnds(PhotoWorld{}) == PhotoEnd::MatchOver);
    // open: the key is the screen's, whatever else holds (the fighter's own event would toggle it a second time)
    { PhotoWorld w = Shot(); w.open = true; CHECK(PhotoKeyAnswer(w) == PhotoKey::Close && PhotoEnds(w) == PhotoEnd::Keep); }
    { PhotoWorld w; w.open = true; w.closedForMs = 0; CHECK(PhotoKeyAnswer(w) == PhotoKey::Close); }
    // just closed: the same press does not open it again; then it does
    { PhotoWorld w = Shot(); w.closedForMs = 0; CHECK(PhotoKeyAnswer(w) == PhotoKey::Busy); }
    { PhotoWorld w = Shot(); w.closedForMs = kPhotoSettleMs - 1; CHECK(PhotoKeyAnswer(w) == PhotoKey::Busy && PhotoEnds(w) == PhotoEnd::Keep); }
    { PhotoWorld w = Shot(); w.closedForMs = kPhotoSettleMs; CHECK(PhotoKeyAnswer(w) == PhotoKey::Open); }
    { PhotoWorld w; w.closedForMs = 0; CHECK(PhotoKeyAnswer(w) == PhotoKey::Busy); }   // and says nothing meanwhile
    CHECK(kPhotoSettleMs >= 200 && kPhotoSettleMs <= 1000 && kPhotoCloseAfterMs >= 100 && kPhotoCloseAfterMs < kPhotoSettleMs);
    // the key reached my fighter while it was open: the screen has kPhotoCloseAfterMs to close it itself
    CHECK(!PhotoKeyCloses(true, 0) && !PhotoKeyCloses(true, kPhotoCloseAfterMs - 1) && PhotoKeyCloses(true, kPhotoCloseAfterMs) && PhotoKeyCloses(true, 5000));
    CHECK(!PhotoKeyCloses(false, 0) && !PhotoKeyCloses(false, 5000));
    // what the player is told: only where the key did nothing and no other screen answered
    for (PhotoKey a : {PhotoKey::NoMatch, PhotoKey::NotAllowed, PhotoKey::NotReady}) CHECK(*PhotoKeyLine(a));
    for (PhotoKey a : {PhotoKey::Open, PhotoKey::Close, PhotoKey::Dead, PhotoKey::Busy}) CHECK(!*PhotoKeyLine(a));
    CHECK(std::string(PhotoKeyLine(PhotoKey::NotAllowed)).find("host") != std::string::npos && std::string(PhotoKeyLine(PhotoKey::NoMatch)).find("match") != std::string::npos);
    CHECK(std::string(PhotoKeyLine(PhotoKey::NotReady)) == "Photo mode could not start here");
    for (PhotoEnd e : {PhotoEnd::MatchOver, PhotoEnd::Down, PhotoEnd::HostOff}) CHECK(*PhotoEndText(e));
    CHECK(!*PhotoEndText(PhotoEnd::Keep) && std::string(PhotoEndText(PhotoEnd::Down)) == "my fighter is down");
    CHECK(std::string(PhotoEndText(PhotoEnd::HostOff)).find("host") != std::string::npos && std::string(PhotoEndText(PhotoEnd::MatchOver)).find("match") != std::string::npos);
}

// A held body's own input: axes read zero, presses are dropped, releases pass; all of it passes when nothing holds it.
// Photo mode holds it the same way, but for the look axes while the game's own camera is the view.
static void TestHeldInput() {
    CHECK(HoldInput(true, true, false) == HeldInput::Zero && HoldInput(true, true, true) == HeldInput::Zero);
    CHECK(HoldInput(true, false, true) == HeldInput::Drop && HoldInput(true, false, false) == HeldInput::Pass);
    for (bool axis : {false, true}) for (bool pressed : {false, true}) CHECK(HoldInput(false, axis, pressed) == HeldInput::Pass);
    for (bool free : {false, true}) for (bool axis : {false, true}) for (bool look : {false, true}) for (bool pressed : {false, true})
        CHECK(PhotoInput(false, free, axis, look, pressed) == HeldInput::Pass);                            // closed: nothing is held
    CHECK(PhotoInput(true, false, true, true, false) == HeldInput::Pass);                                  // the look axes turn the game's camera
    CHECK(PhotoInput(true, true, true, true, false) == HeldInput::Zero);                                   // not while its free camera flies
    for (bool free : {false, true}) {
        CHECK(PhotoInput(true, free, true, false, false) == HeldInput::Zero);                              // walking and the arms
        CHECK(PhotoInput(true, free, false, false, true) == HeldInput::Drop && PhotoInput(true, free, false, false, false) == HeldInput::Pass);
        CHECK(PhotoInput(true, free, false, true, true) == HeldInput::Drop);                               // a button is never a look axis
    }
}

int main() {
    TestPhotoMode(); TestHeldInput();
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
