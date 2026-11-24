// Byte signatures with wildcards, and the call targets inside a function: how the engine layer (ue/engine.cpp) finds
// an address in a game exe its table (kBuilds) does not know. The signatures come from tools/gen_signatures.py
// (ue/signatures.h); tests/test_signatures.py runs them against every archived exe.
//
// Everything here works on a plain byte block (an exe's .text in memory, or a test buffer), so nothing reads outside it.
#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string_view>
#include <vector>

namespace hsm::util::sig {

// mask[i] == 0: any byte matches position i.
struct Pattern {
    std::vector<uint8_t> bytes, mask;
    bool empty() const { return bytes.empty(); }
    size_t size() const { return bytes.size(); }
};

inline int HexDigit(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

// "48 8B 05 ?? ?? ?? ??": two hex digits or "??" per byte, separated by single spaces. The first byte must be fixed
// (the search anchors on it). Anything else gives an empty pattern, which matches nowhere.
inline Pattern Parse(std::string_view text) {
    Pattern p;
    for (size_t i = 0; i < text.size();) {
        if (i + 2 > text.size()) return {};
        if (text[i] == '?' && text[i + 1] == '?') {
            p.bytes.push_back(0); p.mask.push_back(0);
        } else {
            const int hi = HexDigit(text[i]), lo = HexDigit(text[i + 1]);
            if (hi < 0 || lo < 0) return {};
            p.bytes.push_back(uint8_t(hi * 16 + lo)); p.mask.push_back(1);
        }
        i += 2;
        if (i < text.size()) {
            if (text[i] != ' ' || i + 1 == text.size()) return {};
            ++i;
        }
    }
    if (p.empty() || !p.mask[0]) return {};
    return p;
}

// Every byte fixed: a raw prologue.
inline Pattern Exact(const uint8_t* bytes, size_t size) {
    Pattern p;
    p.bytes.assign(bytes, bytes + size);
    p.mask.assign(size, 1);
    return p;
}

inline bool MatchAt(const uint8_t* at, const Pattern& p) {
    for (size_t i = 0; i < p.size(); ++i)
        if (p.mask[i] && at[i] != p.bytes[i]) return false;
    return true;
}

struct Hits {
    size_t count = 0;   // matches seen, at most the search's limit
    size_t first = 0;   // offset of the first match in the block
};

// Matches of p in [block, block + size), counting up to `limit` (2 is enough to tell "unique" from "not unique").
inline Hits Find(const uint8_t* block, size_t size, const Pattern& p, size_t limit = 2) {
    Hits h;
    if (p.empty() || size < p.size()) return h;
    const uint8_t* const last = block + (size - p.size());
    for (const uint8_t* at = block; at <= last && h.count < limit;) {
        at = static_cast<const uint8_t*>(std::memchr(at, p.bytes[0], size_t(last - at) + 1));
        if (!at) break;
        if (MatchAt(at, p)) {
            if (h.count++ == 0) h.first = size_t(at - block);
        }
        ++at;
    }
    return h;
}

// The address a rel32 field refers to: the end of its instruction plus the signed 32-bit value at `field`.
// Offsets are within the same block; the result can lie outside it (the caller checks).
inline int64_t Rel32Target(const uint8_t* block, size_t field, size_t instructionEnd) {
    int32_t rel;
    std::memcpy(&rel, block + field, sizeof rel);
    return int64_t(instructionEnd) + rel;
}

struct Call {
    size_t at;        // offset of the E8 byte
    int64_t target;   // offset the call goes to
};

// Every "E8 rel32" in [begin, end) of a block, by raw bytes (a function's bounds come from its unwind entry). A byte
// E8 inside another instruction shows up too, so callers keep only targets that pass a further check (a prologue).
inline std::vector<Call> Calls(const uint8_t* block, size_t blockSize, size_t begin, size_t end) {
    std::vector<Call> out;
    if (end > blockSize) end = blockSize;
    for (size_t i = begin; i + 5 <= end; ++i)
        if (block[i] == 0xE8) out.push_back({i, Rel32Target(block, i + 1, i + 5)});
    return out;
}

// The only callee in [begin, end) that `accept(target)` takes (targets outside the block are never offered); -1 when
// there is none or more than one. callAt gets the first call to it.
template <class Accept>
int64_t OnlyCallWhere(const uint8_t* block, size_t blockSize, size_t begin, size_t end, Accept accept, size_t* callAt = nullptr) {
    int64_t found = -1;
    size_t foundAt = 0;
    for (const Call& c : Calls(block, blockSize, begin, end)) {
        if (c.target < 0 || size_t(c.target) >= blockSize || !accept(size_t(c.target))) continue;
        if (found >= 0 && found != c.target) return -1;   // two different callees: ambiguous
        if (found < 0) { found = c.target; foundAt = c.at; }
    }
    if (found >= 0 && callAt) *callAt = foundAt;
    return found;
}

// The only callee in [begin, end) that starts with `prologue`.
inline int64_t OnlyCallTo(const uint8_t* block, size_t blockSize, size_t begin, size_t end, const uint8_t* prologue,
                          size_t prologueSize, size_t* callAt = nullptr) {
    return OnlyCallWhere(block, blockSize, begin, end, [&](size_t t) {
        return t + prologueSize <= blockSize && std::memcmp(block + t, prologue, prologueSize) == 0;
    }, callAt);
}

}  // namespace hsm::util::sig
