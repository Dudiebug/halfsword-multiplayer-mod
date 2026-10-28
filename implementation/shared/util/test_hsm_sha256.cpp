// SHA-256 (util/hsm_sha256.h): the FIPS 180-4 test vectors, chunked feeding, hex round trip.
#include "util/hsm_sha256.h"
#include <cstdio>
#include <cstring>
#include <vector>

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++g_fail; } } while (0)
using namespace hsm;

static std::string HexOf(const char* s) { return Sha256Hex(Sha256Of(reinterpret_cast<const uint8_t*>(s), std::strlen(s))); }

int main() {
    CHECK(HexOf("") == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    CHECK(HexOf("abc") == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    CHECK(HexOf("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq") == "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
    // Million 'a' (the third FIPS vector), fed in odd-sized pieces: block boundaries at every phase.
    {
        Sha256 s;
        std::vector<uint8_t> chunk(997, 'a');
        size_t left = 1000000;
        while (left) { const size_t n = left < chunk.size() ? left : chunk.size(); s.Update(chunk.data(), n); left -= n; }
        CHECK(Sha256Hex(s.Final()) == "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
    }
    // Lengths 50..70 straddle the padding boundary: one-shot equals byte-at-a-time.
    for (size_t n = 50; n <= 70; ++n) {
        std::vector<uint8_t> v(n);
        for (size_t i = 0; i < n; ++i) v[i] = uint8_t(i * 7 + 3);
        Sha256 s;
        for (uint8_t b : v) s.Update(&b, 1);
        CHECK(s.Final() == Sha256Of(v.data(), v.size()));
    }
    // Final resets: the object hashes a second message from scratch.
    {
        Sha256 s;
        const uint8_t a[3] = {'a', 'b', 'c'};
        s.Update(a, 3); (void)s.Final();
        s.Update(a, 3);
        CHECK(Sha256Hex(s.Final()) == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    }
    // Hex round trip and rejections.
    {
        Sha256Digest d = Sha256Of(reinterpret_cast<const uint8_t*>("abc"), 3), o{};
        CHECK(Sha256FromHex(Sha256Hex(d), o) && o == d);
        std::string up = Sha256Hex(d); for (char& c : up) if (c >= 'a' && c <= 'f') c = char(c - 32);
        CHECK(Sha256FromHex(up, o) && o == d);
        CHECK(!Sha256FromHex("", o));
        CHECK(!Sha256FromHex(Sha256Hex(d).substr(1), o));
        CHECK(!Sha256FromHex(Sha256Hex(d) + "0", o));
        std::string bad = Sha256Hex(d); bad[10] = 'g'; CHECK(!Sha256FromHex(bad, o));
        CHECK(Sha256IsZero(Sha256Digest{}) && !Sha256IsZero(d));
    }
    if (g_fail) { std::printf("%d failure(s)\n", g_fail); return 1; }
    std::printf("ok\n");
    return 0;
}
