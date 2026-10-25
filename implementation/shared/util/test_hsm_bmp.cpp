// Unit tests for hsm_bmp.h (MATCH_ROUNDS_UI_CHAT_PLAN 3.4, avatars): Steam's RGBA avatar to a 32-bit BMP for the
// engine's ImportBufferAsTexture2D. Run via implementation/shared/run_tests.sh test_hsm_bmp.
#include "util/hsm_bmp.h"
#include <cstdio>
#include <vector>

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++g_fail; } } while (0)

using hsm::BmpRowStride;
using hsm::RgbaToBmp;

static uint32_t U32(const std::vector<uint8_t>& b, size_t at) {
    return uint32_t(b[at]) | uint32_t(b[at + 1]) << 8 | uint32_t(b[at + 2]) << 16 | uint32_t(b[at + 3]) << 24;
}
static uint32_t U16(const std::vector<uint8_t>& b, size_t at) { return uint32_t(b[at]) | uint32_t(b[at + 1]) << 8; }

// Pixel (x, y) of a test image, y = 0 the top row as Steam gives it: every channel distinct.
static std::vector<uint8_t> Image(uint32_t w, uint32_t h) {
    std::vector<uint8_t> v;
    for (uint32_t y = 0; y < h; ++y)
        for (uint32_t x = 0; x < w; ++x) {
            uint8_t base = uint8_t(16 * y + 4 * x);
            v.insert(v.end(), {uint8_t(base + 1), uint8_t(base + 2), uint8_t(base + 3), uint8_t(200 + y)});   // R G B A
        }
    return v;
}

static void TestHeader() {
    std::vector<uint8_t> rgba = Image(2, 3);
    std::vector<uint8_t> b = RgbaToBmp(rgba.data(), rgba.size(), 2, 3);
    CHECK(b.size() == 54 + 2 * 3 * 4);
    if (b.size() != 54 + 2 * 3 * 4) return;
    CHECK(b[0] == 'B' && b[1] == 'M');
    CHECK(U32(b, 2) == 78);                    // file size
    CHECK(U32(b, 6) == 0);                     // reserved
    CHECK(U32(b, 10) == 54);                   // pixels follow the two headers
    CHECK(U32(b, 14) == 40);                   // BITMAPINFOHEADER
    CHECK(U32(b, 18) == 2);                    // width
    CHECK(U32(b, 22) == 3);                    // height, positive: bottom-up rows
    CHECK(U16(b, 26) == 1);                    // planes
    CHECK(U16(b, 28) == 32);                   // bits per pixel
    CHECK(U32(b, 30) == 0);                    // BI_RGB, uncompressed
    CHECK(U32(b, 34) == 24);                   // pixel bytes
    CHECK(U32(b, 38) == 2835 && U32(b, 42) == 2835);   // 72 dpi
    CHECK(U32(b, 46) == 0 && U32(b, 50) == 0); // no palette
}

static void TestLittleEndian() {
    std::vector<uint8_t> rgba(300 * 260 * 4, 0);
    std::vector<uint8_t> b = RgbaToBmp(rgba.data(), rgba.size(), 300, 260);
    CHECK(b.size() == 54 + 300 * 260 * 4);
    if (b.size() < 54) return;
    CHECK(b[18] == 0x2C && b[19] == 0x01 && b[20] == 0 && b[21] == 0);   // 300
    CHECK(b[22] == 0x04 && b[23] == 0x01 && b[24] == 0 && b[25] == 0);   // 260
    CHECK(U32(b, 2) == 54 + 312000 && U32(b, 34) == 312000);
}

static void TestRowStride() {
    CHECK(BmpRowStride(1, 32) == 4 && BmpRowStride(3, 32) == 12 && BmpRowStride(64, 32) == 256);   // 32 bpp: no padding
    CHECK(BmpRowStride(1, 24) == 4);           // 3 bytes, padded to 4
    CHECK(BmpRowStride(2, 24) == 8);           // 6 bytes, padded to 8
    CHECK(BmpRowStride(3, 24) == 12);          // 9 bytes, padded to 12
    CHECK(BmpRowStride(4, 24) == 12);          // 12 bytes exactly
    CHECK(BmpRowStride(5, 24) == 16);          // 15 bytes, padded to 16
    CHECK(BmpRowStride(1, 8) == 4 && BmpRowStride(4, 8) == 4 && BmpRowStride(5, 8) == 8);
    CHECK(BmpRowStride(0, 32) == 0);
}

// The file's first row is the image's last; each pixel is B G R A.
static void TestBottomUpRows() {
    const uint32_t w = 3, h = 4;
    std::vector<uint8_t> rgba = Image(w, h);
    std::vector<uint8_t> b = RgbaToBmp(rgba.data(), rgba.size(), w, h);
    CHECK(b.size() == 54 + w * h * 4);
    if (b.size() != 54 + w * h * 4) return;
    for (uint32_t row = 0; row < h; ++row)
        for (uint32_t x = 0; x < w; ++x) {
            uint32_t y = h - 1 - row;                          // the image row this file row holds
            size_t at = 54 + size_t(row) * BmpRowStride(w, 32) + x * 4;
            const uint8_t* src = &rgba[(size_t(y) * w + x) * 4];
            CHECK(b[at] == src[2] && b[at + 1] == src[1] && b[at + 2] == src[0] && b[at + 3] == src[3]);
        }
    CHECK(b[54] == 16 * 3 + 3 && b[54 + 3] == 203);            // the bottom-left pixel's blue and alpha first
    CHECK(b[b.size() - 4] == 4 * 2 + 3 && b[b.size() - 1] == 200);   // the top-right pixel last
}

static void TestRefused() {
    std::vector<uint8_t> rgba = Image(4, 4);
    CHECK(RgbaToBmp(rgba.data(), rgba.size() - 1, 4, 4).empty());   // one byte short
    CHECK(RgbaToBmp(rgba.data(), rgba.size(), 4, 3).empty());       // more bytes than the size says
    CHECK(RgbaToBmp(rgba.data(), rgba.size(), 3, 4).empty());
    CHECK(RgbaToBmp(rgba.data(), 0, 0, 4).empty());
    CHECK(RgbaToBmp(rgba.data(), 0, 4, 0).empty());
    CHECK(RgbaToBmp(nullptr, rgba.size(), 4, 4).empty());
    std::vector<uint8_t> wide(size_t(hsm::kBmpMaxSide + 1) * 4, 0);
    CHECK(RgbaToBmp(wide.data(), wide.size(), hsm::kBmpMaxSide + 1, 1).empty());   // Steam's largest avatar is 184
    CHECK(RgbaToBmp(wide.data(), wide.size(), 1, hsm::kBmpMaxSide + 1).empty());
    std::vector<uint8_t> edge(size_t(hsm::kBmpMaxSide) * 4, 0);
    CHECK(RgbaToBmp(edge.data(), edge.size(), hsm::kBmpMaxSide, 1).size() == 54 + edge.size());
    CHECK(RgbaToBmp(edge.data(), edge.size(), 1, hsm::kBmpMaxSide).size() == 54 + edge.size());
    std::vector<uint8_t> one = {9, 8, 7, 6};
    std::vector<uint8_t> b = RgbaToBmp(one.data(), one.size(), 1, 1);
    CHECK(b.size() == 58 && b[54] == 7 && b[55] == 8 && b[56] == 9 && b[57] == 6);
}

int main() {
    TestHeader();
    TestLittleEndian();
    TestRowStride();
    TestBottomUpRows();
    TestRefused();
    if (g_fail) { std::printf("test_hsm_bmp: %d failure(s)\n", g_fail); return 1; }
    std::printf("test_hsm_bmp: ok\n");
    return 0;
}
