// AC-2: loadout blobs up to 256 KiB are chunked and reassembled exactly; corrupt, oversized, duplicate-conflict,
// out-of-range and timed-out transfers are rejected. Chunks go through the real wire encode/decode.
#include "session/hsm_blob.h"
#include <algorithm>
#include <cstdio>
#include <random>

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++g_fail; } } while (0)
using namespace hsm;

static std::vector<uint8_t> Bytes(size_t n, unsigned seed) {
    std::mt19937 rng(seed); std::vector<uint8_t> v(n); for (uint8_t& b : v) b = uint8_t(rng()); return v;
}
static BlobChunk Wire(const BlobChunk& c) {     // through the real protocol encoder/decoder
    auto bytes = Encode(c); BlobChunk o; bool ok = Decode(bytes.data(), bytes.size(), o); CHECK(ok); return o;
}

static void TestCrcKnownVector() {
    const char* s = "123456789";
    CHECK(Crc32(reinterpret_cast<const uint8_t*>(s), 9) == 0xCBF43926u);   // standard CRC-32 check value
}

static void TestRoundTripSizes() {
    for (size_t n : {size_t(1), kBlobChunkBytes - 1, kBlobChunkBytes, kBlobChunkBytes + 1, size_t(100000), kMaxBlobBytes}) {
        auto blob = Bytes(n, unsigned(n));
        auto chunks = SplitBlob(42, blob);
        CHECK(chunks.size() == (n + kBlobChunkBytes - 1) / kBlobChunkBytes);
        std::reverse(chunks.begin(), chunks.end());                         // arrival order does not matter
        BlobAssembler a; std::vector<uint8_t> out; BlobAssembler::Result r = BlobAssembler::Result::Incomplete;
        for (const BlobChunk& c : chunks) r = a.Add(Wire(c), 0, out);
        CHECK(r == BlobAssembler::Result::Complete && out == blob && a.pending() == 0);
    }
}

static void TestRejections() {
    CHECK(SplitBlob(1, {}).empty());
    CHECK(SplitBlob(1, Bytes(kMaxBlobBytes + 1, 1)).empty());               // oversized never sent
    CHECK(SplitBlob(0, Bytes(10, 1)).empty());                              // id 0 reserved

    auto blob = Bytes(20000, 7); auto chunks = SplitBlob(5, blob);
    {   // corrupted byte -> CRC mismatch
        BlobAssembler a; std::vector<uint8_t> out; auto bad = chunks; bad[1].data[3] ^= 0xFF;
        a.Add(bad[0], 0, out); a.Add(bad[1], 0, out);
        CHECK(a.Add(bad[2], 0, out) == BlobAssembler::Result::Rejected && a.pending() == 0);
    }
    {   // conflicting duplicate chunk
        BlobAssembler a; std::vector<uint8_t> out; auto dup = chunks[0]; dup.data[0] ^= 1;
        CHECK(a.Add(chunks[0], 0, out) == BlobAssembler::Result::Incomplete);
        CHECK(a.Add(chunks[0], 0, out) == BlobAssembler::Result::Incomplete);   // identical duplicate tolerated
        CHECK(a.Add(dup, 0, out) == BlobAssembler::Result::Rejected);
    }
    {   // header mismatch inside one transfer
        BlobAssembler a; std::vector<uint8_t> out; auto other = chunks[1]; other.crc ^= 1;
        a.Add(chunks[0], 0, out);
        CHECK(a.Add(other, 0, out) == BlobAssembler::Result::Rejected);
    }
    {   // timeout drops a stalled transfer
        BlobAssembler a; std::vector<uint8_t> out;
        a.Add(chunks[0], 1000, out);
        CHECK(a.pending() == 1);
        a.Expire(1000 + BlobAssembler::kTimeoutMs + 1);
        CHECK(a.pending() == 0);
    }
    {   // pending limit
        BlobAssembler a; std::vector<uint8_t> out;
        for (uint32_t id = 1; id <= BlobAssembler::kMaxPending; ++id) { auto c = SplitBlob(id, blob)[0]; CHECK(a.Add(c, 0, out) == BlobAssembler::Result::Incomplete); }
        auto extra = SplitBlob(999, blob)[0];
        CHECK(a.Add(extra, 0, out) == BlobAssembler::Result::Rejected);
    }
    {   // malformed chunks never decode: wrong count, index past count, short last chunk
        auto c = chunks[0]; c.count = 9; auto b = Encode(c); BlobChunk o; CHECK(!Decode(b.data(), b.size(), o));
        c = chunks[2]; c.index = 3; b = Encode(c); CHECK(!Decode(b.data(), b.size(), o));
        c = chunks[2]; c.data.pop_back(); b = Encode(c); CHECK(!Decode(b.data(), b.size(), o));
        c = chunks[0]; c.total = uint32_t(kMaxBlobBytes + 1); b = Encode(c); CHECK(!Decode(b.data(), b.size(), o));
    }
}

int main() {
    TestCrcKnownVector(); TestRoundTripSizes(); TestRejections();
    std::printf(g_fail ? "%d FAILED\n" : "all blob tests passed\n", g_fail);
    return g_fail ? 1 : 0;
}
