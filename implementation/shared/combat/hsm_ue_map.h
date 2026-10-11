// Reads a UE 5.4 TMap<uint8, bool> (e.g. Willie_BP's "Dismembered Parts Map", Willie_BP.cpp:1016) from raw memory
// into a bit mask: bit k set when key k maps to true. Pure, no engine headers; the caller supplies the memory check.
#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace hsm::combat {

enum class MapRead { Ok, Unreadable, BadLayout, BadEntry };

constexpr size_t kUeMapSize = 80;   // sizeof(TMap<uint8, bool>), checked against the reflected property size

// Readable(const void* p, size_t n) -> true when n bytes at p may be read.
// Layout (UE 5.4 Containers/Set.h, SparseArray.h, BitArray.h, ContainerAllocationPolicies.h):
//   +0  TArray Data { Element* ptr; int32 Num (+8); int32 Max (+12) }           Element stride 12: key +0, value +1
//   +16 TBitArray AllocationFlags { uint32 InlineWords[4]; uint32* Heap (+32); int32 NumBits (+40); int32 MaxBits (+44) }
//   +48 FirstFreeIndex, +52 NumFreeIndices, +56 Hash (16), +72 HashSize; 80 bytes in all.
// The flag words live in the heap block when its pointer is set, else inline (TInlineAllocator::GetAllocation).
// A set flag marks a used slot; a free slot's bytes are the free list and are never read as a pair.
template <class Readable>
MapRead ReadByteBoolMap(const uint8_t* map, Readable&& readable, uint32_t keyLimit, uint32_t& mask) {
    constexpr size_t kNumOffset = 8, kMaxOffset = 12, kInlineOffset = 16, kHeapOffset = 32;
    constexpr size_t kNumBitsOffset = 40, kMaxBitsOffset = 44;
    constexpr size_t kElementStride = 12;
    constexpr int32_t kMaxSlots = 256, kInlineBits = 128;
    mask = 0;
    if (!readable(map, kUeMapSize)) return MapRead::Unreadable;
    auto i32 = [map](size_t off) { int32_t v; std::memcpy(&v, map + off, sizeof v); return v; };
    const uint8_t* data; std::memcpy(&data, map, sizeof data);
    const uint8_t* heap; std::memcpy(&heap, map + kHeapOffset, sizeof heap);
    const int32_t num = i32(kNumOffset), max = i32(kMaxOffset), numBits = i32(kNumBitsOffset), maxBits = i32(kMaxBitsOffset);
    if (num < 0 || num > kMaxSlots || max < num || numBits != num || maxBits < numBits) return MapRead::BadLayout;
    if (!heap && numBits > kInlineBits) return MapRead::BadLayout;
    if (num == 0) return MapRead::Ok;
    const uint8_t* words = heap ? heap : map + kInlineOffset;
    const size_t wordCount = (size_t(num) + 31) / 32;
    if (!readable(words, wordCount * 4) || !readable(data, size_t(num) * kElementStride)) return MapRead::Unreadable;
    uint32_t out = 0;
    for (int32_t i = 0; i < num; ++i) {
        uint32_t w; std::memcpy(&w, words + (size_t(i) >> 5) * 4, sizeof w);
        if (((w >> (size_t(i) & 31u)) & 1u) == 0) continue;
        const uint8_t key = data[size_t(i) * kElementStride], value = data[size_t(i) * kElementStride + 1];
        if (key >= keyLimit || key >= 32 || value > 1) return MapRead::BadEntry;
        if (value) out |= 1u << key;
    }
    mask = out;
    return MapRead::Ok;
}

}  // namespace hsm::combat
