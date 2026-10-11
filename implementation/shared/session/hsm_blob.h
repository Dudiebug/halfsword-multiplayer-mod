// Loadout blob transfer: split into protocol BlobChunks and reassemble with strict validation.
// Game-independent, unit-tested. A blob is opaque bytes (the SaveGame holding one passport).
#pragma once
#include "hsm_protocol.h"
#include <cstdint>
#include <map>
#include <vector>

namespace hsm {

inline uint32_t Crc32(const uint8_t* d, size_t n) {
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 0; i < n; ++i) {
        c ^= d[i];
        for (int k = 0; k < 8; ++k) c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1u)));
    }
    return ~c;
}

// Returns an empty vector for an empty or oversized blob (nothing valid can be sent).
inline std::vector<BlobChunk> SplitBlob(uint32_t blobId, const std::vector<uint8_t>& blob) {
    std::vector<BlobChunk> out;
    if (blobId == 0 || blob.empty() || blob.size() > kMaxBlobBytes) return out;
    uint32_t crc = Crc32(blob.data(), blob.size());
    uint16_t count = uint16_t((blob.size() + kBlobChunkBytes - 1) / kBlobChunkBytes);
    for (uint16_t i = 0; i < count; ++i) {
        BlobChunk c; c.blobId = blobId; c.total = uint32_t(blob.size()); c.index = i; c.count = count; c.crc = crc;
        size_t begin = size_t(i) * kBlobChunkBytes, end = begin + kBlobChunkBytes < blob.size() ? begin + kBlobChunkBytes : blob.size();
        c.data.assign(blob.begin() + long(begin), blob.begin() + long(end));
        out.push_back(std::move(c));
    }
    return out;
}

class BlobAssembler {
public:
    enum class Result { Incomplete, Complete, Rejected };
    static constexpr uint64_t kTimeoutMs = 15000;
    static constexpr size_t kMaxPending = 16;

    // Chunks must already have passed protocol Decode (sizes/indices consistent per chunk).
    Result Add(const BlobChunk& c, uint64_t nowMs, std::vector<uint8_t>& out) {
        Expire(nowMs);
        auto it = pending_.find(c.blobId);
        if (it == pending_.end()) {
            if (pending_.size() >= kMaxPending) return Result::Rejected;
            Pending p; p.total = c.total; p.count = c.count; p.crc = c.crc; p.started = nowMs;
            p.data.assign(c.total, 0); p.have.assign(c.count, false);
            it = pending_.emplace(c.blobId, std::move(p)).first;
        }
        Pending& p = it->second;
        if (p.total != c.total || p.count != c.count || p.crc != c.crc || c.index >= p.count) { pending_.erase(it); return Result::Rejected; }
        size_t at = size_t(c.index) * kBlobChunkBytes;
        if (at + c.data.size() > p.total) { pending_.erase(it); return Result::Rejected; }
        if (p.have[c.index]) {                       // duplicate: identical bytes are tolerated, conflicts are not
            if (!std::equal(c.data.begin(), c.data.end(), p.data.begin() + long(at))) { pending_.erase(it); return Result::Rejected; }
            return Result::Incomplete;
        }
        std::copy(c.data.begin(), c.data.end(), p.data.begin() + long(at));
        p.have[c.index] = true; ++p.received;
        if (p.received < p.count) return Result::Incomplete;
        bool ok = Crc32(p.data.data(), p.data.size()) == p.crc;
        if (ok) out = std::move(p.data);
        pending_.erase(it);
        return ok ? Result::Complete : Result::Rejected;
    }

    void Expire(uint64_t nowMs) {
        for (auto it = pending_.begin(); it != pending_.end();) it = nowMs - it->second.started > kTimeoutMs ? pending_.erase(it) : std::next(it);
    }
    size_t pending() const { return pending_.size(); }

private:
    struct Pending { uint32_t total = 0, crc = 0; uint16_t count = 0, received = 0; uint64_t started = 0; std::vector<uint8_t> data; std::vector<bool> have; };
    std::map<uint32_t, Pending> pending_;
};

}  // namespace hsm
