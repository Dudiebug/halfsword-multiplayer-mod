// Half Sword Enhancer (github.com/lambor590/Half-Sword-Enhancer): how HSMMod recognises it in the game process (pure,
// no engine or Win32 types; enhancer.cpp does the scanning). It is the one mod the lobby rules name: a host blocks or
// allows it with one switch (session/hsm_mod_rules.h ModRule::blockHse), and a Classes lobby that allows it checks its
// settings instead (match/hsm_online_rules.h).
//
// What 0.6.0 ships (checked 2026-11-02): HSEnhancer.dll (version resource ProductName "Half Sword Enhancer", exports
// HSE_Initialize), loaded by its winmm.dll proxy ("Half Sword Enhancer Loader") or by a UE4SS mod's main.dll, which
// LoadLibrary's HSEnhancer.dll. Old releases also named the DLL HS-Enhancer.dll. A module counts when any one signal
// matches: its file name, the export, or the product name in its version resource (the last two survive a rename).
#pragma once
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace hsm {

inline constexpr const char* kEnhancerProduct = "Half Sword Enhancer";
inline constexpr const char* kEnhancerExport = "HSE_Initialize";
inline constexpr const char* kEnhancerModName = "HSEnhancer";   // its UE4SS mod folder, and the mod report's name for the loaded DLL

namespace enhancer_detail {
inline char Lower(char c) { return c >= 'A' && c <= 'Z' ? char(c + 32) : c; }
inline bool EqualNoCase(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) if (Lower(a[i]) != Lower(b[i])) return false;
    return true;
}
}  // namespace enhancer_detail

// A module's file name (no folder) the Enhancer ships under, any case.
inline bool IsEnhancerFileName(std::string_view name) {
    using enhancer_detail::EqualNoCase;
    return EqualNoCase(name, "HSEnhancer.dll") || EqualNoCase(name, "HS-Enhancer.dll");
}

// The mod report's name for it (a UE4SS mod folder, or the Module item for the loaded DLL), any case.
inline bool IsEnhancerModName(std::string_view name) { return enhancer_detail::EqualNoCase(name, kEnhancerModName); }

// The file name part of a Windows path ("C:\a\b\HSEnhancer.dll" -> "HSEnhancer.dll").
inline std::string_view FileNameOf(std::string_view path) {
    const size_t cut = path.find_last_of("\\/");
    return cut == std::string_view::npos ? path : path.substr(cut + 1);
}

// A raw version resource (RT_VERSION, UTF-16LE strings) that names the Enhancer: "Half Sword Enhancer" appears in its
// ProductName or FileDescription, in any case. Bounded by n; a null or short block is no match.
inline bool VersionNamesEnhancer(const uint8_t* p, size_t n) {
    const std::string_view want = kEnhancerProduct;
    const size_t bytes = want.size() * 2;
    if (!p || n < bytes) return false;
    for (size_t off = 0; off + bytes <= n; off += 2) {   // UTF-16 code units are 2-byte aligned in the block
        size_t i = 0;
        while (i < want.size() && p[off + 2 * i + 1] == 0 && enhancer_detail::Lower(char(p[off + 2 * i])) == enhancer_detail::Lower(want[i])) ++i;
        if (i == want.size()) return true;
    }
    return false;
}

// What one module shows; Matches: any signal.
struct EnhancerSignals {
    bool fileName = false;   // HSEnhancer.dll / HS-Enhancer.dll
    bool exportName = false; // exports HSE_Initialize
    bool version = false;    // its version resource names "Half Sword Enhancer"
    bool Matches() const { return fileName || exportName || version; }
};

}  // namespace hsm
