// Steam avatars to engine textures (MATCH_ROUNDS_UI_CHAT_PLAN 3.4, avatars): ISteamUtils::GetImageRGBA gives the
// pixels top row first, four bytes each (R G B A); the engine's ImportBufferAsTexture2D reads image files, so the
// pixels go into a 32-bit uncompressed BMP in memory: the 14-byte file header, the 40-byte BITMAPINFOHEADER, then
// the rows bottom-up, each pixel B G R A. Pure, no engine headers.
#pragma once
#include <cstddef>
#include <cstdint>
#include <vector>

namespace hsm {

inline constexpr uint32_t kBmpHeaderBytes = 14 + 40;
inline constexpr uint32_t kBmpMaxSide = 1024;      // larger than any avatar (Steam's large one is 184); bounds the size

// A row's bytes in a BMP: its pixels' bits rounded up to a whole 32-bit word.
inline constexpr uint32_t BmpRowStride(uint32_t width, uint32_t bitsPerPixel) { return (width * bitsPerPixel + 31) / 32 * 4; }

// Empty when there are no pixels, a side exceeds kBmpMaxSide, or `bytes` is not width * height * 4.
inline std::vector<uint8_t> RgbaToBmp(const uint8_t* rgba, size_t bytes, uint32_t width, uint32_t height) {
    if (!rgba || !width || !height || width > kBmpMaxSide || height > kBmpMaxSide || bytes != size_t(width) * height * 4) return {};
    const uint32_t stride = BmpRowStride(width, 32), pixels = stride * height;
    std::vector<uint8_t> b(kBmpHeaderBytes + size_t(pixels), 0);
    auto put32 = [&](size_t at, uint32_t v) { for (int i = 0; i < 4; ++i) b[at + i] = uint8_t(v >> (8 * i)); };
    auto put16 = [&](size_t at, uint32_t v) { b[at] = uint8_t(v); b[at + 1] = uint8_t(v >> 8); };
    b[0] = 'B'; b[1] = 'M';
    put32(2, kBmpHeaderBytes + pixels);    // file size
    put32(10, kBmpHeaderBytes);            // where the pixels start
    put32(14, 40);                         // BITMAPINFOHEADER
    put32(18, width);
    put32(22, height);                     // positive: bottom-up
    put16(26, 1);                          // planes
    put16(28, 32);                         // bits per pixel
    put32(30, 0);                          // BI_RGB
    put32(34, pixels);
    put32(38, 2835); put32(42, 2835);      // 72 dpi
    for (uint32_t row = 0; row < height; ++row) {
        const uint8_t* src = rgba + size_t(height - 1 - row) * width * 4;
        uint8_t* dst = b.data() + kBmpHeaderBytes + size_t(row) * stride;
        for (uint32_t x = 0; x < width; ++x, src += 4, dst += 4) { dst[0] = src[2]; dst[1] = src[1]; dst[2] = src[0]; dst[3] = src[3]; }
    }
    return b;
}

}  // namespace hsm
