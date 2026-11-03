// Unit tests for hsm_loadout_change.h. Run via implementation/shared/run_tests.sh test_hsm_loadout_change.
#include "session/hsm_loadout_change.h"
#include "session/hsm_blob.h"
#include "match/hsm_match_impl.h"
#include <cstdio>
#include <cstring>
#include <string>

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++g_fail; } } while (0)

using namespace hsm;
using V = LoadoutVerdict;

static void TestRule() {
    const MatchPhase fought[] = {MatchPhase::Live, MatchPhase::SuddenDeath};
    const MatchPhase calm[] = {MatchPhase::Idle, MatchPhase::Countdown, MatchPhase::Results};
    for (MatchPhase p : fought) {
        CHECK(CanChangeLoadout(Gear::Own, p, true, false) == V::InFight);                    // alive in the fight: refused
        CHECK(CanChangeLoadout(Gear::Own, p, true, false, true) == V::AtNextRespawn);        // held down by the match: not fighting
        CHECK(CanChangeLoadout(Gear::Own, p, false, false) == V::AtNextRespawn);             // down for the round: the kit comes with the respawn
        CHECK(CanChangeLoadout(Gear::Own, p, true, true) == V::RespawnNow);                  // a round gap / wave break: nobody fights
        CHECK(CanChangeLoadout(Gear::Own, p, false, true) == V::AtNextRespawn);
    }
    for (MatchPhase p : calm) {
        CHECK(CanChangeLoadout(Gear::Own, p, true, false) == V::RespawnNow);
        CHECK(CanChangeLoadout(Gear::Own, p, false, false) == V::AtNextRespawn);
    }
    // The host's dealt gear is not the player's to change, in any phase.
    for (Gear g : {Gear::RandomTier, Gear::Mirror, Gear::Fists})
        for (MatchPhase p : {MatchPhase::Idle, MatchPhase::Countdown, MatchPhase::Live, MatchPhase::SuddenDeath, MatchPhase::Results})
            for (bool alive : {false, true}) CHECK(CanChangeLoadout(g, p, alive, true) == V::NotOwnGear);
    CHECK(LoadoutAllowed(V::RespawnNow) && LoadoutAllowed(V::AtNextRespawn) && !LoadoutAllowed(V::InFight) && !LoadoutAllowed(V::NotOwnGear));
    CHECK(std::string(LoadoutVerdictText(V::InFight)) == "Change loadout when you are out of the fight");
    CHECK(std::string(LoadoutVerdictText(V::NotOwnGear)).find("Own loadouts only") == 0);
    CHECK(std::string(LoadoutVerdictLog(V::InFight)) == "in the fight" && std::string(LoadoutVerdictLog(V::NotOwnGear)) == "gear is not own");
}

static void TestLimiter() {
    LoadoutChangeLimiter l;
    CHECK(l.Allow(0) && l.Allow(123456) && l.WaitMs(7) == 0);     // never used: free, even at tick 0
    l.Used(1000);
    CHECK(!l.Allow(1000) && !l.Allow(5999) && l.Allow(6000) && l.Allow(60000));
    CHECK(l.WaitMs(1000) == kLoadoutChangeGapMs && l.WaitMs(4000) == 2000 && l.WaitMs(6000) == 0);
    LoadoutChangeLimiter w; w.Used(0xFFFFFF00u);                  // the tick count wraps
    CHECK(!w.Allow(0xFFFFFFFFu) && !w.Allow(100) && w.Allow(0xFFFFFF00u + kLoadoutChangeGapMs));
}

static void TestWire() {
    std::vector<uint8_t> blob(20000);
    for (size_t i = 0; i < blob.size(); ++i) blob[i] = uint8_t(i * 31 + 7);
    std::vector<BlobChunk> chunks = SplitBlob(0x01000001u, blob);
    CHECK(chunks.size() == 3);
    BlobAssembler asmb; std::vector<uint8_t> got; BlobAssembler::Result r = BlobAssembler::Result::Incomplete;
    for (const BlobChunk& c : chunks) {
        LoadoutChangeMsg m; m.chunk = c;
        std::vector<uint8_t> b = Encode(m);
        MsgType t; CHECK(PeekType(b.data(), b.size(), t) && t == MsgType::LoadoutChange && uint8_t(t) == 61);
        LoadoutChangeMsg o; CHECK(Decode(b.data(), b.size(), o) && o.chunk.blobId == c.blobId && o.chunk.index == c.index && o.chunk.data == c.data);
        CHECK(b.size() == Encode(c).size());                                                  // the BlobChunk body, nothing more
        for (size_t cut = 0; cut < b.size(); cut += 1 + b.size() / 40) CHECK(!Decode(b.data(), cut, o));   // truncation
        LoadoutChangeMsg again; CHECK(Decode(b.data(), b.size(), again));                    // the failed cuts above left `o` half filled
        r = asmb.Add(again.chunk, 1000, got);
    }
    CHECK(r == BlobAssembler::Result::Complete && got == blob);                               // survives the round trip

    LoadoutChangeMsg m; m.chunk = chunks[0];
    std::vector<uint8_t> b = Encode(m); LoadoutChangeMsg o;
    std::vector<uint8_t> plain = Encode(m.chunk);                                              // a BlobChunk is not a LoadoutChange
    CHECK(!Decode(plain.data(), plain.size(), o));
    std::vector<uint8_t> other = b; other[3] = uint8_t(MsgType::Chat); CHECK(!Decode(other.data(), other.size(), o));
    std::vector<uint8_t> rev = b; rev[2] = uint8_t(kProtocolRevision + 1); CHECK(!Decode(rev.data(), rev.size(), o));
    std::vector<uint8_t> extra = b; extra.push_back(0); CHECK(!Decode(extra.data(), extra.size(), o));
    LoadoutChangeMsg big = m; big.chunk.total = uint32_t(kMaxBlobBytes + 1); CHECK(!Decode(Encode(big).data(), Encode(big).size(), o));   // over the 256 KiB cap
    LoadoutChangeMsg zero = m; zero.chunk.blobId = 0; CHECK(!Decode(Encode(zero).data(), Encode(zero).size(), o));
    LoadoutChangeMsg idx = m; idx.chunk.index = idx.chunk.count; CHECK(!Decode(Encode(idx).data(), Encode(idx).size(), o));
    CHECK(!Decode(nullptr, 0, o));
    const uint8_t tiny[3] = {0, 0, 0}; CHECK(!Decode(tiny, 3, o));
}

static void TestResult() {
    for (uint8_t v = 0; v < kLoadoutVerdictCount; ++v) {
        LoadoutChangeResult r; r.verdict = LoadoutVerdict(v);
        std::vector<uint8_t> b = Encode(r); LoadoutChangeResult o;
        MsgType t; CHECK(PeekType(b.data(), b.size(), t) && t == MsgType::LoadoutChange && b.size() == 5);
        CHECK(Decode(b.data(), b.size(), o) && o.verdict == LoadoutVerdict(v));
        for (size_t cut = 0; cut < b.size(); ++cut) CHECK(!Decode(b.data(), cut, o));
        std::vector<uint8_t> extra = b; extra.push_back(0); CHECK(!Decode(extra.data(), extra.size(), o));
        LoadoutChangeMsg chunk; CHECK(!Decode(b.data(), b.size(), chunk));                    // a verdict is no chunk
        CHECK(*LoadoutVerdictText(LoadoutVerdict(v)) && *LoadoutVerdictLog(LoadoutVerdict(v)));
    }
    std::vector<uint8_t> bad = Encode(LoadoutChangeResult{}); bad[4] = kLoadoutVerdictCount; LoadoutChangeResult o;
    CHECK(!Decode(bad.data(), bad.size(), o));
    bad[4] = 0xFF; CHECK(!Decode(bad.data(), bad.size(), o));
    BlobChunk c; c.blobId = 1; c.total = 1; c.count = 1; c.crc = 0; c.data = {1};
    LoadoutChangeMsg m; m.chunk = c; std::vector<uint8_t> cb = Encode(m); CHECK(!Decode(cb.data(), cb.size(), o));   // a chunk is no verdict
    CHECK(std::string(LoadoutVerdictText(LoadoutVerdict::TooSoon)).find("Wait") == 0);
}

// Integration (Wave 2): Riot and Juggernaut deal every loadout by role, so the sanitised lobby config forces the dealt gear and CHANGE LOADOUT
// is refused there in every phase; Team Duel keeps the player's own gear, so it follows the Duel rule.
static void TestRoleFormatsRefuse() {
    for (Format f : {Format::Riot, Format::Juggernaut}) {
        MatchConfig c; c.format = f; c.gear = Gear::Own;                                     // whatever the lobby said
        const Gear g = Sanitize(c).gear;
        CHECK(g == Gear::RandomTier);
        for (MatchPhase p : {MatchPhase::Idle, MatchPhase::Countdown, MatchPhase::Live, MatchPhase::SuddenDeath, MatchPhase::Results})
            for (bool alive : {false, true})
                for (bool gap : {false, true}) CHECK(CanChangeLoadout(g, p, alive, gap) == V::NotOwnGear);
    }
    MatchConfig td; td.format = Format::TeamDuel; td.gear = Gear::Own;
    CHECK(Sanitize(td).gear == Gear::Own);
    CHECK(CanChangeLoadout(Sanitize(td).gear, MatchPhase::Live, false, false) == V::AtNextRespawn);     // down for the round: the kit comes back with it
    CHECK(CanChangeLoadout(Sanitize(td).gear, MatchPhase::Live, true, true) == V::RespawnNow);          // the round gap
    CHECK(CanChangeLoadout(Sanitize(td).gear, MatchPhase::Live, true, false) == V::InFight);
}

int main() {
    TestRoleFormatsRefuse();
    TestRule();
    TestLimiter();
    TestWire();
    TestResult();
    if (g_fail) { std::printf("test_hsm_loadout_change: %d failure(s)\n", g_fail); return 1; }
    std::printf("test_hsm_loadout_change: ok\n");
    return 0;
}
