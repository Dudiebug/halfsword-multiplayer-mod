// Unit tests for hsm_record.h: the .hsmrec chunk format and EdgeEvents. With HSM_REC_DUMP=<path> set it also writes a
// sample recording there and its chunk counts to <path>.counts; tools/check.sh reads both back with tools/hsmrec.py.
// Run via implementation/shared/run_tests.sh test_hsm_record.
#include "record/hsm_record.h"
#include <cstdio>
#include <cstdlib>
#include <map>
#include <string>
#include <vector>

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++g_fail; } } while (0)

using namespace hsm::rec;

static bool Same(const F3& a, const F3& b) { return a[0] == b[0] && a[1] == b[1] && a[2] == b[2]; }
static bool Same(const hsm::Quat16& a, const hsm::Quat16& b) { return a.x == b.x && a.y == b.y && a.z == b.z && a.w == b.w; }

static FighterTable SampleTable(uint16_t id, uint8_t flags) {
    FighterTable t;
    t.recId = id; t.flags = flags; t.name = "Willie_BP_C_" + std::to_string(id); t.cls = "Willie_BP_C";
    t.bones = {{"root", kNoParent, false}, {"pelvis", 0, true}, {"hand_r", 1, true}};
    return t;
}
static FighterFrame SampleFrame(uint16_t id, double ts) {
    FighterFrame f;
    f.ts = ts; f.dt = 1.f / 60; f.recId = id;
    f.bones.resize(3);
    for (size_t i = 0; i < 3; ++i) {
        BoneFrame& b = f.bones[i];
        b.pos = {float(ts), float(i), -1.5f}; b.rot = hsm::PackQuat(0.1f, 0.2f, 0.3f, 0.9f);
        b.hasBody = i > 0;
        if (b.hasBody) { b.vel = {1, 2, 3}; b.angVelDeg = {-90, 0, 45}; }
    }
    f.weapons[0].present = true; f.weapons[0].classHash = 0xDEADBEEF;
    f.weapons[0].pos = {10, 20, 30}; f.weapons[0].base = {1, 1, 1}; f.weapons[0].tip = {2, 2, 2};
    f.grip = 0x1403;
    return f;
}

static void TestHeader() {
    FileHeader h;
    h.buildKey = 24185754; h.modBuild = "0.8.0-pre14+abc"; h.level = "Arena_Main"; h.wallMs = 1793000000000ull; h.steadyMs = 12345.5;
    std::vector<uint8_t> b = EncodeHeader(h);
    FileHeader g;
    CHECK(DecodeHeader(b.data(), b.size(), g) == b.size());
    CHECK(g.buildKey == h.buildKey && g.modBuild == h.modBuild && g.level == h.level && g.wallMs == h.wallMs && g.steadyMs == h.steadyMs);
    b[0] = 'X';
    CHECK(DecodeHeader(b.data(), b.size(), g) == 0);
    b[0] = 'H'; b[6] = 2;   // a later version is not read as version 1
    CHECK(DecodeHeader(b.data(), b.size(), g) == 0);
    CHECK(DecodeHeader(b.data(), 10, g) == 0);
}

static void TestFighterTable() {
    FighterTable t = SampleTable(3, kFighterBot), g;
    std::vector<uint8_t> b = Encode(t);
    CHECK(Decode(b.data(), b.size(), g));
    CHECK(g.recId == 3 && g.flags == kFighterBot && g.name == t.name && g.cls == t.cls && g.bones.size() == 3);
    CHECK(g.bones[0].parent == kNoParent && !g.bones[0].hasBody && g.bones[2].name == "hand_r" && g.bones[2].parent == 1 && g.bones[2].hasBody);
    t.bones[2].parent = 7;   // a parent past the bone list
    b = Encode(t);
    CHECK(!Decode(b.data(), b.size(), g));
    b = Encode(SampleTable(3, 0));
    b.push_back(0);          // trailing bytes
    CHECK(!Decode(b.data(), b.size(), g));
    CHECK(!Decode(b.data(), b.size() - 3, g));
}

static void TestFighterFrame() {
    FighterFrame f = SampleFrame(2, 1000.25), g;
    std::vector<uint8_t> b = Encode(f);
    // ts 8, dt 4, id 2, count 2; bone 12+8+1 (+24 with a body); weapon 1+4+12+8+12*4, weapon 1; grip 2.
    CHECK(b.size() == 16 + 3 * 21 + 2 * 24 + 73 + 1 + 2);
    CHECK(Decode(b.data(), b.size(), g));
    CHECK(g.ts == 1000.25 && g.dt == f.dt && g.recId == 2 && g.bones.size() == 3 && g.grip == 0x1403);
    for (size_t i = 0; i < 3; ++i) {
        CHECK(Same(g.bones[i].pos, f.bones[i].pos) && Same(g.bones[i].rot, f.bones[i].rot) && g.bones[i].hasBody == f.bones[i].hasBody);
        CHECK(Same(g.bones[i].vel, f.bones[i].vel) && Same(g.bones[i].angVelDeg, f.bones[i].angVelDeg));
    }
    CHECK(g.weapons[0].present && g.weapons[0].classHash == 0xDEADBEEF && Same(g.weapons[0].tip, f.weapons[0].tip));
    CHECK(!g.weapons[1].present);
    CHECK(!Decode(b.data(), b.size() - 1, g));
    // A NaN position is refused (the recorder never writes one).
    b[16] = 0x00; b[17] = 0x00; b[18] = 0xC0; b[19] = 0x7F;
    CHECK(!Decode(b.data(), b.size(), g));
}

static void TestVitalsPropsEvents() {
    Vitals v;
    v.ts = 5; v.recId = 1; v.health = 80; v.consciousness = 0.5f; v.stamina = 30; v.bleeding = 2;
    v.partHealth = {100, 90, 80, 70, 60, 50, 40}; v.flags = kVitFallen; v.stateMask = 0x81; v.severMask = 4; v.dislocMask = 2;
    std::vector<uint8_t> b = Encode(v);
    Vitals vg;
    CHECK(Decode(b.data(), b.size(), vg));
    CHECK(vg.health == 80 && vg.partHealth.size() == 7 && vg.partHealth[6] == 40 && vg.flags == kVitFallen && vg.stateMask == 0x81 &&
          vg.severMask == 4 && vg.dislocMask == 2);

    PropTable t;
    t.propId = (1ull << 62) | 77; t.name = "BP_Sword_C_3"; t.cls = "BP_Sword_C"; t.bodies = {"Mesh", "Head"};
    b = Encode(t);
    PropTable tg;
    CHECK(Decode(b.data(), b.size(), tg) && tg.propId == t.propId && tg.bodies.size() == 2 && tg.bodies[1] == "Head");

    PropFrame f;
    f.ts = 7; f.propId = t.propId; f.body = 1; f.flags = kPropAtRest; f.pos = {1, 2, 3}; f.vel = {4, 5, 6};
    b = Encode(f);
    PropFrame fg;
    CHECK(b.size() == 8 + 8 + 2 + 12 + 8 + 24);
    CHECK(Decode(b.data(), b.size(), fg) && fg.propId == f.propId && fg.body == 1 && fg.flags == kPropAtRest && Same(fg.vel, f.vel));

    Event e;
    e.ts = 9; e.kind = Ev::Hit; e.subject = 1; e.other = 2; e.hand = 0; e.text = "head"; e.x = {1, 2, 3, 4};
    b = Encode(e);
    Event eg;
    CHECK(Decode(b.data(), b.size(), eg) && eg.kind == Ev::Hit && eg.other == 2 && eg.text == "head" && eg.x[3] == 4);
    b[8] = kEvLast + 1;   // unknown event kind
    CHECK(!Decode(b.data(), b.size(), eg));
    b[8] = uint8_t(Ev::Hit); b[8 + 1 + 2 + 2 + 8] = 2;   // hand 2
    CHECK(!Decode(b.data(), b.size(), eg));
    InputSample in;
    in.ts = 11; in.recId = 1; in.axes = {1, -1, 0.25f, 0, 0, 0, 0.5f, 0, 0, 0}; in.buttons = 0x5; in.controlYaw = 90; in.controlPitch = -10;
    b = Encode(in);
    CHECK(b.size() == 8 + 2 + 1 + 40 + 4 + 8);
    InputSample ig;
    CHECK(Decode(b.data(), b.size(), ig) && ig.axes.size() == 10 && ig.axes[2] == 0.25f && ig.buttons == 0x5 && ig.controlYaw == 90 &&
          ig.controlPitch == -10);
    b[10] = uint8_t(kMaxAxes + 1);   // too many axes
    CHECK(!Decode(b.data(), b.size(), ig));
    in.axes[0] = INFINITY;
    b = Encode(in);
    CHECK(!Decode(b.data(), b.size(), ig));

    CHECK(std::string(EvName(Ev::GrabOn)) == "GrabOn" && std::string(EvName(Ev::Marker)) == "Marker" && std::string(EvName(Ev(99))) == "?");
}

static void TestChunks() {
    std::vector<uint8_t> file;
    Append(file, Chunk::FighterTable, SampleTable(1, kFighterLocal));
    Append(file, Chunk::FighterFrame, SampleFrame(1, 16.5));
    size_t pos = 0;
    ChunkView c;
    CHECK(NextChunk(file.data(), file.size(), pos, c) == NextResult::Ok && c.kind == Chunk::FighterTable);
    FighterTable t;
    CHECK(Decode(c.data, c.size, t) && t.recId == 1);
    CHECK(NextChunk(file.data(), file.size(), pos, c) == NextResult::Ok && c.kind == Chunk::FighterFrame);
    CHECK(NextChunk(file.data(), file.size(), pos, c) == NextResult::End);

    // A crash cuts the last chunk short: everything before it still reads.
    for (size_t cut = 1; cut < 30; ++cut) {
        pos = 0;
        CHECK(NextChunk(file.data(), file.size() - cut, pos, c) == NextResult::Ok);
        CHECK(NextChunk(file.data(), file.size() - cut, pos, c) == NextResult::Truncated);
    }
    // One flipped payload byte fails the checksum.
    std::vector<uint8_t> bad = file;
    bad[7] ^= 0x10;
    pos = 0;
    CHECK(NextChunk(bad.data(), bad.size(), pos, c) == NextResult::Corrupt && pos == 0);
    // A length past the limit is corrupt, not an allocation.
    bad = file;
    bad[4] = 0x7F;
    pos = 0;
    CHECK(NextChunk(bad.data(), bad.size(), pos, c) == NextResult::Corrupt);
    // An unknown kind is still a valid chunk (readers skip it).
    std::vector<uint8_t> unk;
    AppendChunk(unk, Chunk(200), {1, 2, 3});
    pos = 0;
    CHECK(NextChunk(unk.data(), unk.size(), pos, c) == NextResult::Ok && uint8_t(c.kind) == 200 && c.size == 3 && pos == unk.size());
}

static std::vector<Ev> Kinds(const std::vector<Event>& v) {
    std::vector<Ev> k;
    for (const auto& e : v) k.push_back(e.kind);
    return k;
}

static void TestEdges() {
    FighterState a, b;
    std::vector<Event> out;
    EdgeEvents(a, b, 1, 4, out);
    CHECK(out.empty());

    // The right hand grabs fighter 2's head, then moves to the neck (off then on), then lets go.
    b.grabbing[0] = true; b.grabbedFighter[0] = 2; b.grabBone[0] = "head";
    EdgeEvents(a, b, 2, 4, out);
    CHECK(out.size() == 1 && out[0].kind == Ev::GrabOn && out[0].hand == 0 && out[0].other == 2 && out[0].text == "head" && out[0].subject == 4 && out[0].ts == 2);
    out.clear();
    FighterState c = b;
    c.grabBone[0] = "neck_01";
    EdgeEvents(b, c, 3, 4, out);
    CHECK((Kinds(out) == std::vector<Ev>{Ev::GrabOff, Ev::GrabOn}) && out[0].text == "head" && out[1].text == "neck_01");
    out.clear();
    EdgeEvents(c, a, 4, 4, out);
    CHECK(out.size() == 1 && out[0].kind == Ev::GrabOff && out[0].other == 2);
    out.clear();
    EdgeEvents(c, c, 5, 4, out);   // holding on: nothing
    CHECK(out.empty());

    // The left hand grabs a prop.
    b = a; b.grabbing[1] = true; b.grabbedProp[1] = 99;
    EdgeEvents(a, b, 6, 4, out);
    CHECK(out.size() == 1 && out[0].kind == Ev::GrabOn && out[0].hand == 1 && out[0].propId == 99 && out[0].other == 0);
    out.clear();

    // Pick up, swap to another weapon in the same hand (drop + pick up), drop.
    b = a; b.holding[0] = true; b.weaponProp[0] = 10; b.weaponClass[0] = 0xABCD1234;
    EdgeEvents(a, b, 7, 4, out);
    CHECK(out.size() == 1 && out[0].kind == Ev::PickUp && out[0].propId == 10 && ClassFromX(out[0]) == 0xABCD1234);
    out.clear();
    c = b; c.weaponProp[0] = 11; c.weaponClass[0] = 0x0000FFFF;
    EdgeEvents(b, c, 8, 4, out);
    CHECK((Kinds(out) == std::vector<Ev>{Ev::Drop, Ev::PickUp}) && out[0].propId == 10 && out[1].propId == 11 && ClassFromX(out[1]) == 0xFFFF);
    out.clear();
    // Two copies of one weapon class with no prop id: the class alone cannot tell them apart, so no event.
    b = a; b.holding[0] = true; b.weaponClass[0] = 5;
    EdgeEvents(b, b, 9, 4, out);
    CHECK(out.empty());
    EdgeEvents(b, a, 9, 4, out);
    CHECK(out.size() == 1 && out[0].kind == Ev::Drop && out[0].propId == 0 && ClassFromX(out[0]) == 5);
    out.clear();

    // Sheathing: the weapon leaves the hand for a slot (or while the sheathe runs): no Drop. Drawing: no PickUp. A plain
    // drop is still a Drop.
    b = a; b.holding[0] = true; b.weaponProp[0] = 20; b.weaponClass[0] = 7;
    c = a; c.sheathed[2] = 20;
    EdgeEvents(b, c, 9.5, 4, out);
    CHECK(out.empty());
    c = a; c.sheathing[0] = true;
    EdgeEvents(b, c, 9.5, 4, out);
    CHECK((Kinds(out) == std::vector<Ev>{Ev::Sheathe}));
    out.clear();
    c = a; c.sheathed[4] = 20;   // drawn from a slot: no PickUp
    EdgeEvents(c, b, 9.6, 4, out);
    CHECK(out.empty());
    b.drawing[0] = true;         // the draw running as the hand gets it: Draw only
    EdgeEvents(a, b, 9.7, 4, out);
    CHECK((Kinds(out) == std::vector<Ev>{Ev::Draw}) && out[0].propId == 20);
    out.clear();
    b.drawing[0] = false; c = a; c.sheathed[0] = 21;   // another weapon in a slot does not hide a real drop
    EdgeEvents(b, c, 9.8, 4, out);
    CHECK((Kinds(out) == std::vector<Ev>{Ev::Drop}) && out[0].propId == 20);
    out.clear();

    // Grip: one event with old and new packed values.
    b = a; b.grip = 0x7FFF;
    EdgeEvents(a, b, 10, 4, out);
    CHECK(out.size() == 1 && out[0].kind == Ev::GripChange && out[0].hand == kNoHand && out[0].x[0] == 0 && out[0].x[1] == 32767.f);
    out.clear();

    // Rising edges only for thrust, kick, sheathe and draw; both edges for parry and stuck.
    b = a; b.thrusting[0] = true; b.kicking[1] = true; b.sheathing[0] = true; b.drawing[1] = true; b.parrying[1] = true;
    b.stuck[0] = true; b.weaponProp[0] = 12;
    EdgeEvents(a, b, 11, 4, out);
    CHECK((Kinds(out) == std::vector<Ev>{Ev::Sheathe, Ev::Thrust, Ev::StuckOn, Ev::Draw, Ev::Kick, Ev::ParryOn}));
    CHECK(out[2].propId == 12 && out[2].hand == 0 && out[4].hand == 1);
    out.clear();
    EdgeEvents(b, a, 12, 4, out);
    CHECK((Kinds(out) == std::vector<Ev>{Ev::StuckOff, Ev::ParryOff}) && out[0].propId == 12);
    out.clear();

    // Being grabbed, by one fighter then another.
    b = a; b.beingGrabbed = true; b.grabbedBy = 3;
    EdgeEvents(a, b, 13, 4, out);
    CHECK(out.size() == 1 && out[0].kind == Ev::BeingGrabbedOn && out[0].other == 3);
    out.clear();
    c = b; c.grabbedBy = 5;
    EdgeEvents(b, c, 14, 4, out);
    CHECK((Kinds(out) == std::vector<Ev>{Ev::BeingGrabbedOff, Ev::BeingGrabbedOn}) && out[0].other == 3 && out[1].other == 5);
    out.clear();

    // Fall, get up, fall and die. Staying dead is no event; a respawn clears Fallen (GetUp) but has no event of its own.
    b = a; b.fallen = true;
    EdgeEvents(a, b, 15, 4, out);
    EdgeEvents(b, a, 16, 4, out);
    c = b; c.dead = true;
    EdgeEvents(b, c, 17, 4, out);
    EdgeEvents(c, c, 18, 4, out);
    EdgeEvents(c, a, 19, 4, out);
    CHECK((Kinds(out) == std::vector<Ev>{Ev::Fall, Ev::GetUp, Ev::Death, Ev::GetUp}));
}

// The sample file tools/check.sh hands to tools/hsmrec.py: two fighters for one second at 60 Hz, vitals at 15 Hz, one
// prop thrown and coming to rest, a scripted grab / pick-up / fall sequence, level start and end and a marker.
static void Dump(const char* path) {
    FileHeader h;
    h.buildKey = 24185754; h.modBuild = "test_hsm_record"; h.level = "Arena_Test"; h.wallMs = 1793000000000ull; h.steadyMs = 1000;
    std::vector<uint8_t> file = EncodeHeader(h);
    std::map<std::string, int> counts;
    auto add = [&](Chunk k, const auto& v, const std::string& name) { Append(file, k, v); ++counts[name]; };
    auto event = [&](const Event& e) { add(Chunk::Event, e, std::string("Event.") + EvName(e.kind)); ++counts["Event"]; };

    Event e;
    e.ts = 1000; e.kind = Ev::LevelStart; e.text = h.level;
    event(e);
    add(Chunk::FighterTable, SampleTable(1, kFighterLocal), "FighterTable");
    add(Chunk::FighterTable, SampleTable(2, kFighterBot), "FighterTable");
    PropTable pt;
    pt.propId = 42; pt.name = "BP_Stool_C_1"; pt.cls = "BP_Stool_C"; pt.bodies = {"StaticMeshComponent0"};
    add(Chunk::PropTable, pt, "PropTable");

    std::vector<FighterState> prev(3);
    for (int i = 0; i < 60; ++i) {
        const double ts = 1000 + i * 1000.0 / 60;
        for (uint16_t id = 1; id <= 2; ++id) {
            add(Chunk::FighterFrame, SampleFrame(id, ts), "FighterFrame");
            if (i % 4 == 0) {
                Vitals v;
                v.ts = ts; v.recId = id; v.health = 100.f - float(i); v.partHealth.assign(7, 100.f);
                add(Chunk::Vitals, v, "Vitals");
            }
            FighterState s;
            if (id == 1) {
                s.grabbing[0] = i >= 10 && i < 40; s.grabbedFighter[0] = 2; s.grabBone[0] = "head";
                s.holding[1] = i >= 20; s.weaponProp[1] = 42; s.weaponClass[1] = 7;
            } else {
                s.beingGrabbed = i >= 10 && i < 40; s.grabbedBy = 1;
                s.fallen = i >= 45;
            }
            std::vector<Event> evs;
            if (i > 0) EdgeEvents(prev[id], s, ts, id, evs);
            for (const auto& ev : evs) event(ev);
            prev[id] = s;
        }
        InputSample in;
        in.ts = ts; in.recId = 1; in.axes.assign(10, 0.f); in.axes[0] = 1; in.buttons = i >= 30 && i < 50 ? 2u : 0u;
        add(Chunk::Input, in, "Input");
        if (i == 30 || i == 50) {
            e = {}; e.ts = ts; e.kind = Ev::Input; e.subject = 1; e.text = "Grab Right"; e.x[0] = i == 30 ? 1.f : 0.f;
            event(e);
        }
        if (i < 10) {
            PropFrame pf;
            pf.ts = ts; pf.propId = 42; pf.pos = {0, 0, float(100 - i * 10)}; pf.vel = {0, 0, -600};
            if (i == 9) { pf.flags = kPropAtRest; pf.vel = {}; }
            add(Chunk::PropFrame, pf, "PropFrame");
        }
    }
    e = {}; e.ts = 1500; e.kind = Ev::Marker; e.text = "grab test";
    event(e);
    e = {}; e.ts = 2000; e.kind = Ev::LevelEnd; e.text = h.level;
    event(e);
    AppendChunk(file, Chunk(200), {1, 2, 3});   // an unknown kind the reader must skip
    ++counts["Unknown"];

    FILE* f = std::fopen(path, "wb");
    CHECK(f != nullptr);
    if (!f) return;
    CHECK(std::fwrite(file.data(), 1, file.size(), f) == file.size());
    std::fclose(f);
    const std::string cpath = std::string(path) + ".counts";
    f = std::fopen(cpath.c_str(), "wb");
    CHECK(f != nullptr);
    if (!f) return;
    for (const auto& [name, n] : counts) std::fprintf(f, "%s %d\n", name.c_str(), n);   // std::map: sorted by name
    std::fclose(f);
    std::printf("test_hsm_record: wrote %s (%zu bytes)\n", path, file.size());
}

static void TestVitalsEdges() {
    Vitals a, b;
    a.recId = b.recId = 3; b.ts = 50;
    a.partHealth = {100, 100, 100, 100, 100, 100, 100};
    b.partHealth = a.partHealth;
    std::vector<Event> out;
    VitalsEvents(a, b, out);
    CHECK(out.empty());
    b.partHealth[3] = 80; b.partHealth[0] = 99.8f; b.partHealth[5] = 120;   // a hit, noise, a heal
    VitalsEvents(a, b, out);
    CHECK(out.size() == 1);
    CHECK(out[0].kind == Ev::Damage && out[0].subject == 3 && out[0].ts == 50 && out[0].text == "arm R");
    CHECK(out[0].x[0] == 20 && out[0].x[1] == 80 && out[0].x[2] == 3);
    out.clear();
    a.severMask = 0b0010; b.severMask = 0b1011;
    VitalsEvents(a, a, out);
    CHECK(out.empty());
    b.partHealth = a.partHealth;
    VitalsEvents(a, b, out);
    CHECK(out.size() == 2 && out[0].kind == Ev::Sever && out[0].x[0] == 0 && out[1].x[0] == 3);
    out.clear();
    VitalsEvents(b, a, out);   // bits cleared (a respawn): nothing
    CHECK(out.empty());
    a.severMask = b.severMask;
    a.partHealth[2] = NAN;     // a broken read never makes an event
    VitalsEvents(a, b, out);
    CHECK(out.empty());
    a.partHealth.resize(2);    // a short vector compares what both have
    b.partHealth[1] = 0;
    VitalsEvents(a, b, out);
    CHECK(out.size() == 1 && out[0].text == "upper body");
}

int main() {
    TestHeader();
    TestFighterTable();
    TestFighterFrame();
    TestVitalsPropsEvents();
    TestChunks();
    TestEdges();
    TestVitalsEdges();
    if (const char* p = std::getenv("HSM_REC_DUMP")) Dump(p);
    if (g_fail == 0) std::printf("test_hsm_record: all passed\n");
    return g_fail != 0;
}
