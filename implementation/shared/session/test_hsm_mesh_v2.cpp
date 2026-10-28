// Mesh v2 pure parts (session/hsm_mesh.h, rev 22): direct sender checks, direct targets, first-copy-wins dedupe.
#include "session/hsm_mesh.h"
#include <cstdio>

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++g_fail; } } while (0)
using namespace hsm;

static MeshMsgFacts F(uint8_t actor, uint8_t victim = 0, bool strip = false) { MeshMsgFacts f; f.actor = actor; f.victim = victim; f.stripRequest = strip; return f; }

int main() {
    // ---- which types travel direct
    CHECK(MeshDirectType(MsgType::HitClaim) && MeshDirectType(MsgType::HitFx) && MeshDirectType(MsgType::SeverClaim));
    CHECK(MeshDirectType(MsgType::SeverNotice) && MeshDirectType(MsgType::StuckBlade) && MeshDirectType(MsgType::PaintFx));
    CHECK(MeshDirectType(MsgType::Equip) && MeshDirectType(MsgType::GrabState) && MeshDirectType(MsgType::PropState));
    CHECK(!MeshDirectType(MsgType::Pose) && !MeshDirectType(MsgType::Vitals));   // rev 19's streams, checked on their own path
    CHECK(!MeshDirectType(MsgType::Chat) && !MeshDirectType(MsgType::DeathReport) && !MeshDirectType(MsgType::PropLease));
    CHECK(!MeshDirectType(MsgType::MatchInfo) && !MeshDirectType(MsgType::Kill) && !MeshDirectType(MsgType::PingTable));

    // ---- sender checks. Slot 2 owns entity 3; this machine is slot 4 (entity 5).
    const uint8_t me = 5;
    CHECK(MeshSenderOk(MsgType::Pose, 2, F(3), me));
    CHECK(!MeshSenderOk(MsgType::Pose, 2, F(4), me));                      // someone else's fighter
    CHECK(!MeshSenderOk(MsgType::Pose, 2, F(kMaxFighters + 1), me));       // a bot: the host's
    CHECK(!MeshSenderOk(MsgType::Chat, 2, F(3), me));                      // not a direct type
    CHECK(!MeshSenderOk(MsgType::PaintFx, kHostSlot, F(1), me));           // the host is never a mesh peer
    CHECK(!MeshSenderOk(MsgType::PaintFx, kMaxFighters, F(kMaxFighters + 1), me));
    CHECK(MeshSenderOk(MsgType::PaintFx, 2, F(3), me));
    CHECK(!MeshSenderOk(MsgType::PaintFx, 2, F(5, 0, true), me));          // a strip request goes through the host
    CHECK(!MeshSenderOk(MsgType::PaintFx, 2, F(3, 0, true), me));
    CHECK(MeshSenderOk(MsgType::Equip, 2, F(3), me) && MeshSenderOk(MsgType::GrabState, 2, F(3), me));
    CHECK(MeshSenderOk(MsgType::SeverNotice, 2, F(3), me) && !MeshSenderOk(MsgType::SeverNotice, 2, F(5), me));
    CHECK(MeshSenderOk(MsgType::PropState, 2, F(3), me) && !MeshSenderOk(MsgType::PropState, 2, F(0), me));
    // blows: the attacker is the sender's, the victim real and not the attacker
    CHECK(MeshSenderOk(MsgType::HitClaim, 2, F(3, me), me));
    CHECK(!MeshSenderOk(MsgType::HitClaim, 2, F(3, 7), me));               // replayed by its owner only: not for us
    CHECK(!MeshSenderOk(MsgType::HitClaim, 2, F(4, me), me));              // an attacker the sender does not own
    CHECK(!MeshSenderOk(MsgType::HitClaim, 2, F(3, 3), me));               // self
    CHECK(!MeshSenderOk(MsgType::HitClaim, 2, F(3, 0), me));
    CHECK(!MeshSenderOk(MsgType::HitClaim, 2, F(3, uint8_t(kMaxWillies + 1)), me));
    CHECK(MeshSenderOk(MsgType::StuckBlade, 2, F(3, me), me) && !MeshSenderOk(MsgType::StuckBlade, 2, F(3, 7), me));
    CHECK(MeshSenderOk(MsgType::HitFx, 2, F(3, 7), me));                   // a third party's blow, shown cosmetically
    CHECK(MeshSenderOk(MsgType::HitFx, 2, F(3, kMaxFighters + 2), me));    // on a bot
    CHECK(!MeshSenderOk(MsgType::HitFx, 2, F(3, me), me));                 // ours comes as a HitClaim
    CHECK(MeshSenderOk(MsgType::SeverClaim, 2, F(3, me), me) && MeshSenderOk(MsgType::SeverClaim, 2, F(3, 7), me));
    CHECK(!MeshSenderOk(MsgType::SeverClaim, 2, F(3, 3), me));

    // ---- direct targets
    const std::vector<uint8_t> peers = {kHostSlot, 1, 3, 4, kMaxFighters};
    auto t = MeshDirectSlots(MsgType::HitClaim, 4, peers);                 // victim entity 4 = slot 3
    CHECK(t.size() == 1 && t[0] == 3);
    t = MeshDirectSlots(MsgType::StuckBlade, 1, peers);                    // the host's fighter: the host has its own copy
    CHECK(t.empty());
    t = MeshDirectSlots(MsgType::HitFx, 4, peers);
    CHECK(t.size() == 2 && t[0] == 1 && t[1] == 4);
    t = MeshDirectSlots(MsgType::HitFx, kMaxFighters + 1, peers);          // a bot: every peer
    CHECK(t.size() == 3);
    t = MeshDirectSlots(MsgType::PaintFx, 0, peers);
    CHECK(t.size() == 3 && t[0] == 1 && t[2] == 4);
    CHECK(MeshDirectSlots(MsgType::Chat, 0, peers).empty());
    CHECK(MeshDirectSlots(MsgType::HitClaim, 0, peers).empty());           // no victim: nobody

    // ---- MeshPeers::PeerOfSlot
    MeshPeers mp;
    PeerMapMsg pm; pm.peers = {{1, 101}, {3, 103}};
    mp.Apply(pm, 4, 104, 100);
    CHECK(mp.PeerOfSlot(1) == 0);                                          // named, not bound yet
    mp.Bind(101, 11); mp.Bind(103, 13);
    CHECK(mp.PeerOfSlot(1) == 11 && mp.PeerOfSlot(3) == 13 && mp.PeerOfSlot(2) == 0 && mp.PeerOfSlot(kHostSlot) == 0);
    CHECK(mp.Slots() == std::vector<uint8_t>({1, 3}));
    mp.ForgetPeer(11); CHECK(mp.PeerOfSlot(1) == 0 && mp.PeerOfSlot(3) == 13 && mp.Slots() == std::vector<uint8_t>({3}));

    // ---- dedupe: wide ids
    MeshDedupe d;
    CHECK(d.Accept(MeshStream::HitClaim, 3, 10));
    CHECK(!d.Accept(MeshStream::HitClaim, 3, 10));                         // the relayed copy
    CHECK(d.Accept(MeshStream::HitClaim, 4, 10));                          // another attacker
    CHECK(d.Accept(MeshStream::HitFx, 3, 10));                             // another stream
    CHECK(d.Accept(MeshStream::HitClaim, 3, 12) && d.Accept(MeshStream::HitClaim, 3, 11));   // out of order, both first copies
    CHECK(!d.Accept(MeshStream::HitClaim, 3, 11) && !d.Accept(MeshStream::HitClaim, 3, 12));
    CHECK(d.Accept(MeshStream::HitClaim, 3, 12 + kMeshDedupeWindow));      // a big jump clears the window
    CHECK(!d.Accept(MeshStream::HitClaim, 3, 12));                         // too old to tell: refused
    CHECK(d.Accept(MeshStream::SeverClaim, 3, 0xFFFFFFFFu) && d.Accept(MeshStream::SeverClaim, 3, 0));   // 32-bit wrap
    CHECK(!d.Accept(MeshStream::SeverClaim, 3, 0xFFFFFFFFu));
    // narrow ids wrap at 65536
    CHECK(d.Accept(MeshStream::PaintFx, 3, 65534) && d.Accept(MeshStream::PaintFx, 3, 65535) && d.Accept(MeshStream::PaintFx, 3, 0));
    CHECK(d.Accept(MeshStream::PaintFx, 3, 1) && !d.Accept(MeshStream::PaintFx, 3, 65535) && !d.Accept(MeshStream::PaintFx, 3, 0));
    CHECK(!d.Accept(MeshStream::PaintFx, 3, 65536 + 1));                   // the same 16-bit id
    CHECK(d.Accept(MeshStream::PaintFx, 3, 5));                            // gaps are fine (one counter over every kind)
    // a stuck blade's release carries the attach's seq: separate streams
    CHECK(d.Accept(MeshStream::StuckAttach, 3, 7) && d.Accept(MeshStream::StuckRelease, 3, 7));
    CHECK(!d.Accept(MeshStream::StuckAttach, 3, 7) && !d.Accept(MeshStream::StuckRelease, 3, 7));
    // forget: a rejoined machine numbers from 1 again
    CHECK(d.Accept(MeshStream::Equip, 3, 900));
    CHECK(!d.Accept(MeshStream::Equip, 3, 900 - kMeshDedupeWindow));
    const size_t before = d.Tracked();
    d.Forget(3);
    CHECK(d.Tracked() < before && d.Accept(MeshStream::Equip, 3, 1) && d.Accept(MeshStream::HitClaim, 3, 10));
    CHECK(!d.Accept(MeshStream::HitClaim, 4, 10));                         // other entities kept
    d.Clear(); CHECK(d.Tracked() == 0 && d.Accept(MeshStream::HitClaim, 4, 10));
    // window edge
    MeshDedupe e;
    CHECK(e.Accept(MeshStream::HitClaim, 2, 1000));
    CHECK(e.Accept(MeshStream::HitClaim, 2, 1000 - (kMeshDedupeWindow - 1)));   // the oldest id still in the window
    CHECK(!e.Accept(MeshStream::HitClaim, 2, 1000 - kMeshDedupeWindow));
    CHECK(!e.Accept(MeshStream::HitClaim, 2, 1000 - (kMeshDedupeWindow - 1)));

    if (g_fail) { std::printf("test_hsm_mesh_v2: %d failure(s)\n", g_fail); return 1; }
    std::printf("test_hsm_mesh_v2: all passed\n");
    return 0;
}
