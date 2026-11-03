// A small, bounded reader of a PE image's headers: just enough to say "this file is a 64-bit Windows DLL that exports
// these names". The self-update (session/hsm_update.h) runs it on a downloaded dwmapi.dll before it replaces the
// running one, so a file that is not the mod's proxy DLL (an HTML error page, a 32-bit build, a DLL without the dwmapi
// entry points the game imports) is never installed. Every offset is checked against the buffer; anything off gives a
// verdict, never a read outside it. Not a loader and not a signature check: the SHA-256 is what proves the bytes.
#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace hsm {

enum class PeVerdict : uint8_t {
    Ok,
    TooSmall,        // shorter than a DOS header
    NotMz,           // no "MZ"
    Truncated,       // a header, table or string runs past the end of the file
    NotPe,           // no "PE\0\0", or a section count / export count that is nonsense
    WrongMachine,    // not x64 (0x8664)
    NotPe64,         // optional header is not PE32+
    NotDll,          // the DLL flag is not set
    NoExports,       // no export directory
    MissingExport,   // an entry point from `required` is not exported by name
};

inline const char* PeVerdictText(PeVerdict v) {
    switch (v) {
        case PeVerdict::Ok: return "ok";
        case PeVerdict::TooSmall: return "the file is too small to be a DLL";
        case PeVerdict::NotMz: return "the file is not a Windows program";
        case PeVerdict::Truncated: return "the DLL is cut off";
        case PeVerdict::NotPe: return "the file is not a valid DLL";
        case PeVerdict::WrongMachine: return "the DLL is not for 64-bit Windows";
        case PeVerdict::NotPe64: return "the DLL is not a 64-bit image";
        case PeVerdict::NotDll: return "the file is not a DLL";
        case PeVerdict::NoExports: return "the DLL exports nothing";
        case PeVerdict::MissingExport: return "the DLL lacks the dwmapi entry points";
    }
    return "the DLL is not valid";
}

// The entry points the game executable imports from dwmapi.dll (tools/check_dll.py `game_imports`): without them the
// game does not start, so an update without them must never replace a working proxy.
inline const std::vector<std::string>& DwmapiRequiredExports() {
    static const std::vector<std::string> v = {"DwmFlush", "DwmGetCompositionTimingInfo", "DwmIsCompositionEnabled", "DwmSetWindowAttribute"};
    return v;
}

inline constexpr uint16_t kPeMachineX64 = 0x8664;
inline constexpr size_t kPeMaxSections = 96;
inline constexpr uint32_t kPeMaxExportNames = 4096;
inline constexpr size_t kPeMaxExportNameLen = 128;

namespace pe_detail {
struct Img {
    const uint8_t* d;
    size_t n;
    bool Has(uint64_t off, uint64_t len) const { return off <= n && len <= n - off; }
    uint16_t U16(size_t o) const { return uint16_t(d[o] | d[o + 1] << 8); }
    uint32_t U32(size_t o) const { return uint32_t(d[o]) | uint32_t(d[o + 1]) << 8 | uint32_t(d[o + 2]) << 16 | uint32_t(d[o + 3]) << 24; }
};
struct Section { uint32_t vsize, va, rawSize, rawPtr; };

// File offset of `len` bytes at relative address `rva`, or false when they are not inside one section's file data.
inline bool RvaToOff(const Img& img, const std::vector<Section>& secs, uint32_t rva, uint32_t len, size_t& off) {
    for (const Section& s : secs) {
        if (rva < s.va) continue;
        const uint64_t rel = uint64_t(rva) - s.va;
        if (rel >= s.rawSize || len > s.rawSize - rel) continue;
        const uint64_t o = uint64_t(s.rawPtr) + rel;
        if (!img.Has(o, len)) return false;
        off = size_t(o);
        return true;
    }
    return false;
}
}  // namespace pe_detail

inline PeVerdict CheckProxyDll(const uint8_t* data, size_t n, const std::vector<std::string>& required) {
    using namespace pe_detail;
    if (!data || n < 0x40) return PeVerdict::TooSmall;
    const Img img{data, n};
    if (data[0] != 'M' || data[1] != 'Z') return PeVerdict::NotMz;
    const uint32_t lfanew = img.U32(0x3C);
    if (!img.Has(lfanew, 24)) return PeVerdict::Truncated;
    const size_t pe = lfanew;
    if (data[pe] != 'P' || data[pe + 1] != 'E' || data[pe + 2] != 0 || data[pe + 3] != 0) return PeVerdict::NotPe;
    if (img.U16(pe + 4) != kPeMachineX64) return PeVerdict::WrongMachine;
    const uint16_t nsec = img.U16(pe + 6), optSize = img.U16(pe + 20), chars = img.U16(pe + 22);
    if (!img.Has(pe + 24, optSize)) return PeVerdict::Truncated;
    if (optSize < 2 || img.U16(pe + 24) != 0x20B) return PeVerdict::NotPe64;
    if (!(chars & 0x2000)) return PeVerdict::NotDll;
    if (nsec == 0 || nsec > kPeMaxSections) return PeVerdict::NotPe;
    if (optSize < 120) return PeVerdict::NoExports;   // too short to hold the export directory entry
    const uint32_t exportRva = img.U32(pe + 24 + 112), exportSize = img.U32(pe + 24 + 116);
    if (exportRva == 0 || exportSize < 40) return PeVerdict::NoExports;
    const size_t tab = pe + 24 + optSize;
    if (!img.Has(tab, size_t(nsec) * 40)) return PeVerdict::Truncated;
    std::vector<Section> secs;
    for (size_t i = 0; i < nsec; ++i) {
        const size_t o = tab + i * 40;
        secs.push_back({img.U32(o + 8), img.U32(o + 12), img.U32(o + 16), img.U32(o + 20)});
    }
    size_t dir = 0;
    if (!RvaToOff(img, secs, exportRva, 40, dir)) return PeVerdict::Truncated;
    const uint32_t nNames = img.U32(dir + 24), namesRva = img.U32(dir + 32);
    if (nNames == 0) return PeVerdict::NoExports;
    if (nNames > kPeMaxExportNames) return PeVerdict::NotPe;
    size_t names = 0;
    if (!RvaToOff(img, secs, namesRva, nNames * 4, names)) return PeVerdict::Truncated;
    std::vector<bool> found(required.size(), false);
    for (uint32_t k = 0; k < nNames; ++k) {
        size_t s = 0;
        if (!RvaToOff(img, secs, img.U32(names + size_t(k) * 4), 1, s)) return PeVerdict::Truncated;
        size_t len = 0;
        while (s + len < n && data[s + len] != 0 && len <= kPeMaxExportNameLen) ++len;
        if (s + len >= n) return PeVerdict::Truncated;
        if (len > kPeMaxExportNameLen) continue;   // not a name we look for
        const std::string_view name(reinterpret_cast<const char*>(data + s), len);
        for (size_t r = 0; r < required.size(); ++r) if (name == required[r]) found[r] = true;
    }
    for (bool f : found) if (!f) return PeVerdict::MissingExport;
    return PeVerdict::Ok;
}

}  // namespace hsm
