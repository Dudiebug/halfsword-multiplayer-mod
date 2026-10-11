// AC-3b: N-fighter session decisions. Host spawns each member once per fight level, only after Ready (or the
// ready timeout) and a loadout (or the blob grace); leaves despawn; menus spawn nothing. Clients create a puppet
// for every announced non-own entity once its loadout arrived, replace it when the loadout changes, and forget
// everything on a level change. Blob ids encode the slot and keep only the newest version.
#include "session/hsm_director.h"
#include <cstdio>

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++g_fail; } } while (0)
using namespace hsm;
using V = std::vector<uint8_t>;

static Spawn Sp(uint8_t entity, uint32_t blob) { Spawn s; s.entity = entity; s.ownerSlot = uint8_t(entity - 1); s.blobId = blob; return s; }

static void TestBlobIds() {
    CHECK(MakeBlobId(0, 1) == 0x01000001u && MakeBlobId(5, 7) == 0x06000007u);
    CHECK(MakeBlobId(2, 0) != 0 && (MakeBlobId(2, 0) & 0xFFFFFFu) == 1);            // version 0 never yields id 0 / version 0
    // Spec revision (Phase 3): bots relay loadouts too, so every entity slot 0..15 has blob ids.
    CHECK(BlobSlot(MakeBlobId(3, 9)) == 3 && BlobSlot(0) == -1 && BlobSlot(0x07000001u) == 6 && BlobSlot(MakeBlobId(15, 1)) == 15 && BlobSlot(0x11000001u) == -1);
}

static void TestBlobStore() {
    BlobStore s;
    CHECK(s.Put(MakeBlobId(1, 1), {1, 2}) && s.Get(MakeBlobId(1, 1)) && *s.Get(MakeBlobId(1, 1)) == V({1, 2}));
    CHECK(s.Put(MakeBlobId(1, 2), {3}) && !s.Get(MakeBlobId(1, 1)) && s.LatestFor(1) == MakeBlobId(1, 2));   // newer replaces
    CHECK(!s.Put(MakeBlobId(1, 1), {9}) && s.LatestFor(1) == MakeBlobId(1, 2));                               // stale refused
    CHECK(!s.Put(0, {1}) && !s.Put(MakeBlobId(2, 1), {}));                                                     // bad id / empty
    CHECK(s.Put(MakeBlobId(2, 1), {4}) && s.size() == 2);
    s.DropSlot(1); CHECK(s.size() == 1 && !s.Get(MakeBlobId(1, 2)));
    // A Spawn may name a version older than the newest one received for that slot: the newest satisfies it.
    CHECK(s.Put(MakeBlobId(3, 2), {7}));
    CHECK(s.AtLeast(MakeBlobId(3, 1)) && *s.AtLeast(MakeBlobId(3, 1)) == V({7}));
    CHECK(s.AtLeast(MakeBlobId(3, 2)) && !s.AtLeast(MakeBlobId(3, 3)) && !s.AtLeast(MakeBlobId(4, 1)) && !s.AtLeast(0));
}

static void TestHostSpawnsAfterReadyAndBlob() {
    HostDirector d;
    d.OnJoin(0, 0); d.OnJoin(1, 0); d.OnJoin(2, 0);
    CHECK(d.Due(100).empty());                                    // menu: nothing
    d.OnLevel(true, 1000, 0);
    CHECK(d.Due(1000) == V({0}));                                 // host's own fighter is always ready
    d.MarkSpawned(Sp(1, 0));
    CHECK(d.Due(1001).empty());
    d.OnReady(1, 0, 1100);
    CHECK(d.Due(1200).empty());                                   // ready but no loadout yet, inside grace
    d.OnBlob(1, MakeBlobId(1, 1));
    CHECK(d.Due(1200) == V({1}) && d.BlobFor(1) == MakeBlobId(1, 1));
    d.MarkSpawned(Sp(2, MakeBlobId(1, 1)));
    CHECK(d.Due(1300).empty());
    d.OnReady(2, 0, 1300);
    CHECK(d.Due(1300 + HostDirector::kBlobGraceMs - 1).empty());
    CHECK(d.Due(1300 + HostDirector::kBlobGraceMs) == V({2}));   // grace over: default loadout
    auto live = d.LiveSpawns();
    CHECK(live.size() == 2 && live[0].entity == 1 && live[1].entity == 2);
}

static void TestReadyTimeoutAndLevelReset() {
    HostDirector d;
    d.OnJoin(0, 0); d.OnJoin(3, 0); d.OnBlob(3, MakeBlobId(3, 1));
    d.OnLevel(true, 5000, 0);
    d.MarkSpawned(Sp(1, 0));
    CHECK(d.Due(5000 + HostDirector::kReadyTimeoutMs - 1).empty());
    CHECK(d.Due(5000 + HostDirector::kReadyTimeoutMs) == V({3}));   // never reported Ready
    d.MarkSpawned(Sp(4, MakeBlobId(3, 1)));
    d.OnLevel(true, 90000, 0);                                          // next level: everything respawns, Ready again
    CHECK(!d.Live(1) && !d.Live(4) && d.Due(90000) == V({0}));
    d.OnReady(3, 0, 90010); CHECK(d.Due(90010) == V({0, 3}));
    d.OnLevel(false, 99000, 0); CHECK(d.Due(99000).empty() && !d.fight());
}

static void TestLeaveAndLateJoin() {
    HostDirector d;
    d.OnJoin(0, 0); d.OnJoin(1, 0);
    d.OnLevel(true, 0, 0); d.OnReady(1, 0, 0); d.OnBlob(1, MakeBlobId(1, 1));
    d.MarkSpawned(Sp(1, 0)); d.MarkSpawned(Sp(2, MakeBlobId(1, 1)));
    CHECK(d.OnLeave(1) == 2 && !d.Live(2));
    CHECK(d.OnLeave(1) == 0);                                        // second leave: nothing live
    d.OnJoin(4, 50000);                                              // joins mid-fight: waits for its own Ready
    d.OnBlob(4, MakeBlobId(4, 1));
    CHECK(d.Due(50001).empty());
    d.OnReady(4, 0, 50100); CHECK(d.Due(50100) == V({4}));
    d.OnReady(9, 0, 1); d.OnBlob(9, 5);                                 // unknown slot: ignored
    CHECK(d.Due(50100) == V({4}));
    Spawn bot = Sp(9, MakeBlobId(8, 1)); bot.kind = EntityKind::Bot;     // bots are announced like fighters
    d.MarkSpawned(bot); CHECK(d.Live(9) && d.LiveSpawns().back().entity == 9);
    d.Forget(9); CHECK(!d.Live(9) && d.LiveSpawns().size() == 1);      // cleared wave: late joiners are not told
    CHECK(d.Due(50100) == V({4}));                                       // forgetting a bot never schedules a fighter
}

// A machine that restarts and rejoins its reserved slot mid-fight (owner sync, session.cpp OnHello): its old fighter is
// gone everywhere, so the host forgets it; the rejoiner's Ready is then answered by a fresh spawn, not the stale Spawn.
static void TestRejoinRespawnsAfterForget() {
    HostDirector d;
    d.OnJoin(0, 0); d.OnJoin(1, 0);
    d.OnLevel(true, 0, 0); d.OnReady(1, 0, 0); d.OnBlob(1, MakeBlobId(1, 1));
    d.MarkSpawned(Sp(1, 0)); d.MarkSpawned(Sp(2, MakeBlobId(1, 1)));
    d.OnJoin(1, 60000);                                              // the same slot again, its fighter still live
    d.OnBlob(1, MakeBlobId(1, 2)); d.OnReady(1, 0, 60100);
    CHECK(d.Due(60100).empty() && d.LiveSpawns().size() == 2);      // kept live: never spawned again
    d.Forget(2);
    CHECK(!d.Live(2) && d.LiveSpawns().size() == 1);                // Ready is not answered with the stale Spawn
    CHECK(d.Due(60100) == V({1}) && d.BlobFor(1) == MakeBlobId(1, 2));
    d.MarkSpawned(Sp(2, MakeBlobId(1, 2)));
    CHECK(d.Due(60101).empty() && d.Live(2) && d.Live(1));          // the host's fighter is untouched
}

static void TestEarlyReadySurvivesLevelLoad() {
    HostDirector d;
    d.OnJoin(0, 0); d.OnJoin(1, 0); d.OnJoin(2, 0);
    d.OnBlob(1, MakeBlobId(1, 1)); d.OnBlob(2, MakeBlobId(2, 1));
    d.OnReady(1, 7, 100);                                            // client 1 loaded travel #7 before the host did
    d.OnReady(2, 6, 100);                                            // client 2's Ready is from the previous travel
    d.OnLevel(true, 200, 7);
    CHECK(d.Due(200) == V({0, 1}));                                  // slot 1 keeps its Ready, slot 2 must re-report
    d.OnReady(2, 7, 300); CHECK(d.Due(300) == V({0, 1, 2}));
    d.OnLevel(true, 400, 8);                                         // a later level forgets both
    CHECK(d.Due(400) == V({0}));
    d.OnLevel(true, 500, 0); d.OnReady(1, 0, 0);                     // seq 0 (unknown travel) never carries over
    d.OnLevel(true, 600, 0); CHECK(d.Due(600) == V({0}));
}

static void TestClientWorld() {
    ClientWorld w; w.SetMyEntity(2);
    auto have = [](uint32_t id) { return id == MakeBlobId(0, 1); };
    CHECK(!w.OnSpawn(Sp(1, MakeBlobId(0, 1))));
    CHECK(!w.OnSpawn(Sp(2, MakeBlobId(0, 1))));                     // own fighter: never a puppet, even with its loadout here
    CHECK(!w.OnSpawn(Sp(3, MakeBlobId(2, 1))));                     // loadout not here yet
    CHECK(!w.OnSpawn(Sp(4, 0)));                                    // default loadout: no blob needed
    auto p = w.PendingPuppets(have);
    CHECK(p.size() == 2 && p[0].entity == 1 && p[1].entity == 4);
    w.MarkCreated(1); w.MarkCreated(4);
    CHECK(w.PendingPuppets(have).empty());
    CHECK(!w.OnSpawn(Sp(1, MakeBlobId(0, 1))));                     // same loadout re-announced: keep puppet
    CHECK(w.PendingPuppets(have).empty());
    CHECK(w.OnSpawn(Sp(4, MakeBlobId(0, 1))));                      // new loadout: destroy + recreate
    CHECK(w.PendingPuppets(have).size() == 1);
    CHECK(w.OnDespawn(1));                                          // created puppet: destroy
    CHECK(!w.OnDespawn(1));                                         // already gone
    CHECK(w.Known(3) && !w.OnDespawn(3));                           // announced but never created: nothing to destroy
    CHECK(!w.Known(1) && !w.Known(3));
    w.OnLevel(); CHECK(w.size() == 0 && w.myEntity() == 2);
}

int main() {
    TestBlobIds(); TestBlobStore(); TestHostSpawnsAfterReadyAndBlob(); TestReadyTimeoutAndLevelReset(); TestLeaveAndLateJoin(); TestRejoinRespawnsAfterForget(); TestEarlyReadySurvivesLevelLoad(); TestClientWorld();
    std::printf(g_fail ? "%d FAILED\n" : "all director tests passed\n", g_fail);
    return g_fail ? 1 : 0;
}
