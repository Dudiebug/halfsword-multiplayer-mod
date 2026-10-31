// Owner authority rules: fighter entity e belongs to roster slot e-1, everything else (bots) to the host; a machine
// may stream and claim hits only with what it owns; the host relays a stream to every other client; a hit claim is
// replayed by the victim's owner and shown cosmetically by everyone else, and is dropped when spoofed, self-inflicted
// or aimed at a fighter nobody owns any more.
#include "session/hsm_authority.h"
#include <algorithm>
#include <cstdio>

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++g_fail; } } while (0)
using namespace hsm;
using V = std::vector<uint8_t>;

static void TestOwnerSlot() {
    CHECK(OwnerSlotOf(1) == 0);                      // the host's own fighter
    CHECK(OwnerSlotOf(2) == 1);
    CHECK(OwnerSlotOf(uint8_t(kMaxFighters)) == uint8_t(kMaxFighters - 1));
    CHECK(OwnerSlotOf(uint8_t(kMaxFighters + 1)) == kHostSlot);   // first bot
    CHECK(OwnerSlotOf(uint8_t(kMaxWillies)) == kHostSlot);        // last bot
    CHECK(OwnerSlotOf(0) == 0xFF);
    CHECK(OwnerSlotOf(uint8_t(kMaxWillies + 1)) == 0xFF);
}

static void TestMayStream() {
    CHECK(MayStream(2, 3));
    CHECK(!MayStream(2, 2) && !MayStream(2, 4) && !MayStream(2, 1));
    CHECK(MayStream(kHostSlot, 1));
    CHECK(MayStream(kHostSlot, uint8_t(kMaxFighters + 1)) && MayStream(kHostSlot, uint8_t(kMaxWillies)));
    CHECK(!MayStream(kHostSlot, 2));
    CHECK(!MayStream(1, uint8_t(kMaxFighters + 1)));              // a client never owns a bot
    CHECK(!MayStream(uint8_t(kMaxFighters), uint8_t(kMaxFighters + 1)));   // no such slot
    // A hand-off: entity 3 (slot 2's) is the host's for now, entity 1 (the host's) slot 3's.
    Holders held; held.slot[3] = kHostSlot; held.slot[1] = 3;
    Holders other; other.slot[4] = kHostSlot;
    CHECK(MayStream(kHostSlot, 3, held) && !MayStream(2, 3, held) && MayStream(2, 3, other));
    CHECK(OwnerSlotOf(3, held) == kHostSlot && OwnerSlotOf(3, other) == 2 && OwnerSlotOf(1, held) == 3 && !MayStream(kHostSlot, 1, held));
    const ClaimRoute h = RouteClaim(kHostSlot, 3, 4, {kHostSlot, 2, 3}, held);    // handed 3 hits client 4: slot 3 replays it
    CHECK(h.accept && h.replaySlot == 3 && !h.hostFx);
    const ClaimRoute v = RouteClaim(3, 4, 3, {kHostSlot, 2, 3}, held);            // client 4 hits handed 3: the host replays it
    CHECK(v.accept && v.replaySlot == kHostSlot);
    CHECK(!RouteClaim(2, 3, 4, {kHostSlot, 2, 3}, held).accept);                  // slot 2 claims with 3 it does not have now
    Holders hostHeld; hostHeld.slot[1] = 3;
    const ClaimRoute b = RouteClaim(2, 3, 1, {kHostSlot, 2, 3}, hostHeld);        // client 3 hits the host's fighter slot 3 holds
    CHECK(b.accept && b.replaySlot == 3 && b.hostFx);
    CHECK(!MayStream(0xFF, 0));
    CHECK(!MayStream(kHostSlot, 0));
}

static void TestRelayTargets() {
    CHECK((RelayTargets(2, V{ 0, 1, 2, 3 }) == V{ 1, 3 }));       // never back to the sender, never to the host
    CHECK((RelayTargets(kHostSlot, V{ 1, 2, 3 }) == V{ 1, 2, 3 }));
    CHECK(RelayTargets(1, V{ 1 }).empty());
}

static void TestRouteClaim() {
    const V peers{ 1, 2, 3 };                                     // slots of the connected clients
    ClaimRoute r = RouteClaim(1, 2, 3, peers);                    // client 1 (entity 2) hits client 2's fighter (3)
    CHECK(r.accept && r.replaySlot == 2 && (r.fxSlots == V{ 3 }) && r.hostFx);
    r = RouteClaim(1, 2, 1, peers);                               // ... hits the host's fighter
    CHECK(r.accept && r.replaySlot == kHostSlot && (r.fxSlots == V{ 2, 3 }) && !r.hostFx);
    r = RouteClaim(1, 2, uint8_t(kMaxFighters + 2), peers);       // ... hits a bot: the host replays it
    CHECK(r.accept && r.replaySlot == kHostSlot && (r.fxSlots == V{ 2, 3 }) && !r.hostFx);
    r = RouteClaim(kHostSlot, uint8_t(kMaxFighters + 1), 2, peers);   // a host bot hits client 1
    CHECK(r.accept && r.replaySlot == 1 && (r.fxSlots == V{ 2, 3 }) && !r.hostFx);
    r = RouteClaim(kHostSlot, 1, 4, peers);                       // the host hits client 3
    CHECK(r.accept && r.replaySlot == 3 && (r.fxSlots == V{ 1, 2 }) && !r.hostFx);
    CHECK(!RouteClaim(1, 3, 4, peers).accept);                    // spoofed attacker (entity 3 is slot 2's)
    CHECK(!RouteClaim(1, 2, 2, peers).accept);                    // self-inflicted
    CHECK(!RouteClaim(1, 2, 5, peers).accept);                    // slot 4 is not connected
    CHECK(!RouteClaim(1, 2, 0, peers).accept);
    CHECK(!RouteClaim(1, 2, uint8_t(kMaxWillies + 1), peers).accept);
    r = RouteClaim(1, 2, 5, peers);
    CHECK(r.replaySlot == 0xFF && r.fxSlots.empty() && !r.hostFx);   // a rejected route names nobody
}

int main() {
    TestOwnerSlot();
    TestMayStream();
    TestRelayTargets();
    TestRouteClaim();
    std::printf(g_fail ? "%d FAILED\n" : "all authority tests passed\n", g_fail);
    return g_fail ? 1 : 0;
}
