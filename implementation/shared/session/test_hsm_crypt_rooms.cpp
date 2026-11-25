// Unit tests for hsm_crypt_rooms.h (the shared Crypts run's wire, protocol rev 50). Run via implementation/shared/run_tests.sh test_hsm_crypt_rooms.
#include "session/hsm_crypt_rooms.h"
#include "session/hsm_budget.h"
#include <cstdio>

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++g_fail; } } while (0)

using namespace hsm;
using namespace hsm::crypt;

static CryptRoomMsg Room(uint16_t seq, uint16_t parent, uint8_t gen) {
    CryptRoomMsg m; m.seq = seq; m.catalog = 1; m.x = 1000.5f; m.y = -2000.25f; m.z = 30; m.yaw = 90; m.generation = gen; m.parentSeq = parent;
    m.tier = 2; m.startEntrance = parent ? 0 : kNoEntrance; m.flags = parent ? 0 : kRoomEntrance;
    return m;
}

static void TestRoomWire() {
    const CryptRoomMsg m = Room(3, 2, 1);
    auto b = Encode(m); CryptRoomMsg y;
    CHECK(Decode(b.data(), b.size(), y) && y.seq == 3 && y.catalog == 1 && y.x == 1000.5f && y.y == -2000.25f && y.z == 30 && y.yaw == 90
          && y.generation == 1 && y.parentSeq == 2 && y.tier == 2 && y.startEntrance == 0 && y.flags == 0);
    for (size_t cut = 0; cut < b.size(); ++cut) CHECK(!Decode(b.data(), cut, y));
    { auto x = b; x.push_back(0); CHECK(!Decode(x.data(), x.size(), y)); }
    { auto x = b; x[3] = uint8_t(MsgType::CryptDoor); CHECK(!Decode(x.data(), x.size(), y)); }
    auto bad = [&](auto edit) { CryptRoomMsg z = m; edit(z); auto e = Encode(z); CryptRoomMsg keep; keep.seq = 77; return !Decode(e.data(), e.size(), keep) && keep.seq == 77; };
    CHECK(bad([](CryptRoomMsg& z) { z.seq = 0; }));
    CHECK(bad([](CryptRoomMsg& z) { z.seq = uint16_t(kMaxRooms + 1); }));
    CHECK(bad([](CryptRoomMsg& z) { z.catalog = uint8_t(crypts::kRoomCount); }));
    CHECK(bad([](CryptRoomMsg& z) { z.x = 1e9f; }));
    CHECK(bad([](CryptRoomMsg& z) { z.yaw = 720; }));
    CHECK(bad([](CryptRoomMsg& z) { z.generation = kMaxGeneration + 1; }));
    CHECK(bad([](CryptRoomMsg& z) { z.tier = kMaxTier + 1; }));
    CHECK(bad([](CryptRoomMsg& z) { z.flags = 8; }));
    CHECK(bad([](CryptRoomMsg& z) { z.parentSeq = 3; }));                      // a parent comes first
    CHECK(bad([](CryptRoomMsg& z) { z.startEntrance = 9; }));                  // not one of the room's entrances
    CHECK(bad([](CryptRoomMsg& z) { z.parentSeq = 0; z.startEntrance = 0; }));  // a root has no joint
    CHECK(bad([](CryptRoomMsg& z) { z.parentSeq = 0; z.startEntrance = kNoEntrance; z.x = NAN; }));
    { CryptRoomMsg root = Room(1, 0, 0); auto e = Encode(root); CHECK(Decode(e.data(), e.size(), y) && y.parentSeq == 0 && y.startEntrance == kNoEntrance); }
    CHECK(BudgetClassOf(MsgType::CryptRoom) == BudgetClass::Control && BudgetClassOf(MsgType::CryptDoor) == BudgetClass::Control
          && BudgetClassOf(MsgType::CryptRoomsBuilt) == BudgetClass::Control);
    CHECK(kProtocolRevision == 52 && kLastDecodedMsgType == 77 && kLastReservedMsgType == 77);
    CHECK(uint8_t(MsgType::CryptRoom) == 73 && uint8_t(MsgType::CryptDoor) == 74 && uint8_t(MsgType::CryptRoomsBuilt) == 75);
}

static void TestDoorAndBuilt() {
    CryptDoorMsg d; d.seq = 4; d.kind = 1; d.x = 5; d.y = 6; d.z = 7; d.yaw = -45;
    auto b = Encode(d); CryptDoorMsg y;
    CHECK(Decode(b.data(), b.size(), y) && y.seq == 4 && y.kind == 1 && y.x == 5 && y.yaw == -45 && y.state == kDoorClosed);
    { CryptDoorMsg z = d; z.state = kDoorGone; auto e = Encode(z); CHECK(Decode(e.data(), e.size(), y) && y.state == kDoorGone); }
    { CryptDoorMsg z = d; z.state = 3; auto e = Encode(z); CHECK(!Decode(e.data(), e.size(), y)); }
    for (size_t cut = 0; cut < b.size(); ++cut) CHECK(!Decode(b.data(), cut, y));
    { CryptDoorMsg z = d; z.kind = 3; auto e = Encode(z); CHECK(!Decode(e.data(), e.size(), y)); }
    { CryptDoorMsg z = d; z.seq = 0; auto e = Encode(z); CHECK(!Decode(e.data(), e.size(), y)); }
    { CryptDoorMsg z = d; z.z = INFINITY; auto e = Encode(z); CHECK(!Decode(e.data(), e.size(), y)); }
    CryptRoomsBuiltMsg r; r.count = 12; auto rb = Encode(r); CryptRoomsBuiltMsg ry;
    CHECK(Decode(rb.data(), rb.size(), ry) && ry.count == 12);
    for (size_t cut = 0; cut < rb.size(); ++cut) CHECK(!Decode(rb.data(), cut, ry));
    { CryptRoomsBuiltMsg z; z.count = uint16_t(kMaxRooms + 1); auto e = Encode(z); CHECK(!Decode(e.data(), e.size(), ry)); }
    { CryptRoomsBuiltMsg z; z.count = 0; auto e = Encode(z); CHECK(Decode(e.data(), e.size(), ry) && ry.count == 0); }
}

static void TestList() {
    RoomList host;
    const CryptRoomMsg* a = host.Add(Room(0, 0, 0)); CHECK(a && a->seq == 1);
    const CryptRoomMsg* b = host.Add(Room(0, 1, 1)); CHECK(b && b->seq == 2);
    host.Add(Room(0, 2, 2));
    CHECK(host.Size() == 3 && host.Known() == 3 && host.EntranceCover() == 2);   // gen <= 1: seq 1 and 2
    // a client gets them out of order: it can build only the contiguous prefix
    RoomList cl;
    const auto all = host.All();
    CHECK(cl.Receive(all[2]) && cl.Known() == 0);
    CHECK(!cl.Receive(all[2]));                                                  // a repeat
    CHECK(cl.Receive(all[0]) && cl.Known() == 1);
    CHECK(cl.Receive(all[1]) && cl.Known() == 3);
    CHECK(!cl.Receive(CryptRoomMsg{}));                                          // seq 0
    RoomList full; for (size_t i = 0; i < kMaxRooms; ++i) CHECK(full.Add(Room(0, uint16_t(i), 1)));
    CHECK(full.Add(Room(0, 1, 1)) == nullptr);
    CHECK(RoomList().EntranceCover() == 0);
    cl.Clear(); CHECK(cl.Size() == 0);
}

static void TestBuilt() {
    BuiltSet s; CHECK(s.Prefix() == 0);
    s.Mark(2); CHECK(s.Prefix() == 0 && s.Has(2));
    s.Mark(1); CHECK(s.Prefix() == 2);
    s.Mark(0); s.Mark(3); CHECK(s.Prefix() == 3);
    s.Clear(); CHECK(s.Prefix() == 0);
}

static void TestReady() {
    ReadyTracker t;
    CHECK(t.Decide(2, 5, 0) == Hold::Spawn);                    // not armed: not a shared run
    t.Arm(1000);
    CHECK(t.Decide(2, 5, 1000) == Hold::Wait);                  // nothing reported
    CHECK(t.Decide(2, 0, 1000) == Hold::Wait);                  // no rooms known on the host yet: not covered
    t.Report(2, 4); CHECK(t.Decide(2, 5, 2000) == Hold::Wait);
    t.Report(2, 3); CHECK(t.Reported(2) == 4);                  // the count only grows
    t.Report(2, 5); CHECK(t.Decide(2, 5, 3000) == Hold::Spawn && t.IsReady(2));
    CHECK(t.Decide(2, 9, 4000) == Hold::Spawn);                 // latched: later rooms never take the body away
    CHECK(t.Decide(3, 5, 1000 + kReadyTimeoutMs - 1) == Hold::Wait);
    CHECK(t.Decide(3, 5, 1000 + kReadyTimeoutMs) == Hold::TimedOut);
    CHECK(t.Decide(3, 5, 1000 + kReadyTimeoutMs + 5) == Hold::Spawn);   // timed out once, then it is let through
    t.Forget(3); CHECK(!t.IsReady(3) && t.Reported(3) == 0);
    t.Disarm(); CHECK(!t.Armed() && t.Decide(9, 5, 0) == Hold::Spawn);
}

static void TestJoint() {
    const CryptRoomMsg m = Room(2, 1, 1);
    const crypts::Pose j = JointOf(m);
    const crypts::Pose e = crypts::ToWorld({m.x, m.y, m.z, m.yaw}, crypts::kRooms[m.catalog].entrances[0]);
    CHECK(std::fabs(j.x - e.x) < 1e-9 && std::fabs(j.y - e.y) < 1e-9 && std::fabs(j.yaw - e.yaw) < 1e-9);
    const CryptRoomMsg root = Room(1, 0, 0);
    const crypts::Pose r = JointOf(root);
    CHECK(r.x == root.x && r.y == root.y);                      // a root's "joint" is its origin
}

int main() {
    TestRoomWire(); TestDoorAndBuilt(); TestList(); TestBuilt(); TestReady(); TestJoint();
    if (g_fail) { std::printf("%d failure(s)\n", g_fail); return 1; }
    std::printf("test_hsm_crypt_rooms: ok\n");
    return 0;
}
