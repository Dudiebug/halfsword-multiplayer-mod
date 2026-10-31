// The asset names inside a serialized passport blob, for the log: lets a tester see what weapon a Weapons-row roll
// produced without parsing the save's property layout. Pure byte scan, so it cannot fault on any blob.
#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace hsm {

// The distinct printable runs (at least minLen characters) of `blob` that name a Blueprint class or asset path
// ("BP_..." or "/Game/..."), in first-seen order, at most maxNames, each cut to maxLen characters.
inline std::vector<std::string> BlobAssetNames(const std::vector<uint8_t>& blob, size_t maxNames = 6, size_t minLen = 6, size_t maxLen = 72) {
    std::vector<std::string> out;
    auto flush = [&](std::string& run) {
        size_t at = run.find("BP_");
        const size_t g = run.find("/Game/");
        if (g != std::string::npos && (at == std::string::npos || g < at)) at = g;
        if (at != std::string::npos && run.size() - at >= minLen) {
            std::string name = run.substr(at, maxLen);
            bool seen = false;
            for (const std::string& n : out) seen = seen || n == name;
            if (!seen && out.size() < maxNames) out.push_back(std::move(name));
        }
        run.clear();
    };
    std::string run;
    for (uint8_t b : blob) {
        if (b >= 0x20 && b < 0x7F) run.push_back(char(b));
        else flush(run);
    }
    flush(run);
    return out;
}

}  // namespace hsm
