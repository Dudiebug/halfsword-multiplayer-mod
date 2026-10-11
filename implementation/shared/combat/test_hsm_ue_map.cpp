// Unit tests for hsm_ue_map.h. Run via implementation/shared/run_tests.sh test_hsm_ue_map.
//
// The buffers are built from the UE 5.4 headers' layout of TMap<uint8, bool>, independently of the reader:
//   TSet { TSparseArray Elements; TInlineAllocator<1> Hash; int32 HashSize; }                    = 80 bytes
//   TSparseArray { TArray Data (ptr +0, num +8, max +12); TBitArray AllocationFlags (+16);
//                  int32 FirstFreeIndex (+48); int32 NumFreeIndices (+52); }                     = 56 bytes
//   TBitArray<TInlineAllocator<4>> { uint32 InlineData[4] (+16); uint32* SecondaryData (+32);
//                  int32 NumBits (+40); int32 MaxBits (+44); }
//   TSetElement<TPair<uint8,bool>> { uint8 Key; bool Value; int32 HashNextId; int32 HashIndex; } stride 12
// TInlineAllocator::GetAllocation returns the secondary (heap) pointer when it is set, else the inline words.
#include "combat/hsm_ue_map.h"
#include <cstdio>
#include <cstring>
#include <vector>

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++g_fail; } } while (0)

using namespace hsm::combat;

namespace {

struct Slot { bool used; uint8_t key; uint8_t value; };

// One test map: the 80-byte TMap plus the storage it points at.
struct Map {
    alignas(8) uint8_t bytes[kUeMapSize] = {};
    std::vector<uint8_t> data;
    std::vector<uint32_t> heapWords;

    template <class T> void Put(size_t off, T v) { std::memcpy(bytes + off, &v, sizeof v); }

    // heap: store the allocation flags in a heap block instead of the inline words.
    Map(const std::vector<Slot>& slots, bool heap = false) {
        const int32_t n = int32_t(slots.size());
        data.assign(slots.size() * 12 + 1, 0xCD);   // +1: never a zero-length block
        std::vector<uint32_t> words((slots.size() + 31) / 32 + 1, 0);
        for (size_t i = 0; i < slots.size(); ++i) {
            if (!slots[i].used) continue;
            data[i * 12] = slots[i].key;
            data[i * 12 + 1] = slots[i].value;
            words[i / 32] |= 1u << (i % 32);
        }
        Put<const uint8_t*>(0, data.data());
        Put<int32_t>(8, n);
        Put<int32_t>(12, n);
        int32_t maxBits = 128;
        if (heap) {
            heapWords = words;
            // Inline words hold a contradicting pattern: a reader that ignores the heap pointer reads every slot as used.
            for (size_t w = 0; w < 4; ++w) Put<uint32_t>(16 + w * 4, 0xFFFFFFFFu);
            Put<const uint32_t*>(32, heapWords.data());
            maxBits = int32_t(heapWords.size() * 32);
        } else {
            for (size_t w = 0; w < 4 && w < words.size(); ++w) Put<uint32_t>(16 + w * 4, words[w]);
            Put<const uint32_t*>(32, nullptr);
        }
        Put<int32_t>(40, n);
        Put<int32_t>(44, maxBits);
        Put<int32_t>(48, -1);
        Put<int32_t>(52, 0);
    }
};

// Readable over exactly the blocks a test owns; anything else (including a misread pointer) is unreadable.
struct Blocks {
    std::vector<std::pair<const uint8_t*, size_t>> ok;
    void Add(const void* p, size_t n) { ok.push_back({static_cast<const uint8_t*>(p), n}); }
    void Add(const Map& m) {
        Add(m.bytes, sizeof m.bytes);
        Add(m.data.data(), m.data.size());
        if (!m.heapWords.empty()) Add(m.heapWords.data(), m.heapWords.size() * 4);
    }
    bool operator()(const void* p, size_t n) const {
        if (!p || !n) return false;
        const uint8_t* b = static_cast<const uint8_t*>(p);
        for (auto& [base, size] : ok)
            if (b >= base && b + n <= base + size) return true;
        return false;
    }
};

MapRead Read(const Map& m, uint32_t& mask, uint32_t keyLimit = 15) {
    Blocks blocks; blocks.Add(m);
    return ReadByteBoolMap(m.bytes, blocks, keyLimit, mask);
}

void TestEmpty() {
    Map m({});
    uint32_t mask = 7;
    CHECK(Read(m, mask) == MapRead::Ok);
    CHECK(mask == 0);
}

// The first severed part: one element, inline flags. This is the case the old reader failed on in game.
void TestOneInline() {
    Map m({{true, 9, 1}});
    uint32_t mask = 0;
    CHECK(Read(m, mask) == MapRead::Ok);
    CHECK(mask == (1u << 9));
}

void TestSeveralInline() {
    Map m({{true, 3, 1}, {true, 5, 0}, {false, 0, 0}, {true, 14, 1}, {true, 0, 1}});
    uint32_t mask = 0;
    CHECK(Read(m, mask) == MapRead::Ok);
    CHECK(mask == ((1u << 3) | (1u << 14) | (1u << 0)));   // 5 maps to false; the free slot is skipped
}

// A free slot's bytes are the free-list link, not a pair; they must be skipped even when they look out of range.
void TestFreeSlotSkipped() {
    Map m({{true, 2, 1}, {false, 0, 0}});
    m.data[12] = 0xEE; m.data[13] = 0x77;
    uint32_t mask = 0;
    CHECK(Read(m, mask) == MapRead::Ok);
    CHECK(mask == (1u << 2));
}

void TestHeapFlags() {
    std::vector<Slot> slots;
    for (int i = 0; i < 140; ++i) slots.push_back({false, 0, 0});
    slots[1] = {true, 4, 1};
    slots[70] = {true, 7, 1};
    slots[139] = {true, 11, 1};
    slots[30] = {true, 12, 0};
    Map m(slots, true);
    uint32_t mask = 0;
    CHECK(Read(m, mask, 32) == MapRead::Ok);
    CHECK(mask == ((1u << 4) | (1u << 7) | (1u << 11)));
}

// Heap flags with few elements (a map that grew and shrank): the pointer, not MaxBits, picks the storage.
void TestHeapFlagsSmall() {
    Map m({{true, 6, 1}, {false, 0, 0}}, true);
    m.data[12] = 0xEE;   // would be a bad entry if the contradicting inline words were read
    uint32_t mask = 0;
    CHECK(Read(m, mask) == MapRead::Ok);
    CHECK(mask == (1u << 6));
}

void TestBadEntries() {
    uint32_t mask = 0;
    { Map m({{true, 15, 1}}); CHECK(Read(m, mask) == MapRead::BadEntry); CHECK(mask == 0); }
    { Map m({{true, 31, 1}}); CHECK(Read(m, mask, 32) == MapRead::Ok); CHECK(mask == (1u << 31)); }
    { Map m({{true, 32, 1}}); CHECK(Read(m, mask, 40) == MapRead::BadEntry); }   // never shifts past bit 31
    { Map m({{true, 1, 2}}); CHECK(Read(m, mask) == MapRead::BadEntry); }
    { Map m({{true, 1, 1}, {true, 200, 1}}); CHECK(Read(m, mask) == MapRead::BadEntry); CHECK(mask == 0); }
}

void TestBadLayout() {
    uint32_t mask = 0;
    { Map m({{true, 1, 1}}); m.Put<int32_t>(8, -1); CHECK(Read(m, mask) == MapRead::BadLayout); }
    { Map m({{true, 1, 1}}); m.Put<int32_t>(8, 257); m.Put<int32_t>(40, 257); m.Put<int32_t>(44, 288);
      CHECK(Read(m, mask) == MapRead::BadLayout); }
    { Map m({{true, 1, 1}}); m.Put<int32_t>(40, 2); CHECK(Read(m, mask) == MapRead::BadLayout); }      // NumBits != Num
    { Map m({{true, 1, 1}}); m.Put<int32_t>(44, 0); CHECK(Read(m, mask) == MapRead::BadLayout); }      // MaxBits < NumBits
    { Map m({{true, 1, 1}}); m.Put<int32_t>(12, 0); CHECK(Read(m, mask) == MapRead::BadLayout); }      // ArrayMax < Num
    // 256 slots is the cap, with heap flags as a map that size would have.
    { std::vector<Slot> s256(256, Slot{false, 0, 0}); s256[255] = {true, 3, 1};
      Map m(s256, true); CHECK(Read(m, mask) == MapRead::Ok); CHECK(mask == (1u << 3)); }
    { std::vector<Slot> s257(257, Slot{false, 0, 0}); s257[256] = {true, 3, 1};
      Map m(s257, true); CHECK(Read(m, mask) == MapRead::BadLayout); }
    // Inline words hold 128 bits; more than that without a heap block is impossible.
    std::vector<Slot> slots(129, Slot{true, 1, 1});
    Map big(slots);
    big.Put<int32_t>(44, 160);
    CHECK(Read(big, mask) == MapRead::BadLayout);
}

void TestUnreadable() {
    uint32_t mask = 0;
    { Map m({{true, 1, 1}}); Blocks only; only.Add(m.bytes, sizeof m.bytes);           // data block not readable
      CHECK(ReadByteBoolMap(m.bytes, only, 15, mask) == MapRead::Unreadable); }
    { Map m({{true, 1, 1}}, true); Blocks b; b.Add(m.bytes, sizeof m.bytes); b.Add(m.data.data(), m.data.size());
      CHECK(ReadByteBoolMap(m.bytes, b, 15, mask) == MapRead::Unreadable); }            // heap flags not readable
    { Map m({{true, 1, 1}}); Blocks b; b.Add(m.bytes, 56); b.Add(m.data.data(), m.data.size());   // map shorter than a TMap
      CHECK(ReadByteBoolMap(m.bytes, b, 15, mask) == MapRead::Unreadable); }
    { Blocks b; CHECK(ReadByteBoolMap(nullptr, b, 15, mask) == MapRead::Unreadable); }
}

// Negative control: a map laid out the way the pre-fix reader assumed (NumBits +32, MaxBits +36, no heap pointer
// slot) is not a UE 5.4 map and must not be read as one.
void TestOldLayoutRejected() {
    alignas(8) uint8_t bytes[kUeMapSize] = {};
    std::vector<uint8_t> data(12, 0);
    data[0] = 9; data[1] = 1;
    const uint8_t* dp = data.data();
    int32_t one = 1, maxBits = 128; uint32_t word = 1;
    std::memcpy(bytes, &dp, sizeof dp);
    std::memcpy(bytes + 8, &one, 4); std::memcpy(bytes + 12, &one, 4);
    std::memcpy(bytes + 16, &word, 4);
    std::memcpy(bytes + 32, &one, 4); std::memcpy(bytes + 36, &maxBits, 4);
    Blocks b; b.Add(bytes, sizeof bytes); b.Add(data.data(), data.size());
    uint32_t mask = 0;
    CHECK(ReadByteBoolMap(bytes, b, 15, mask) != MapRead::Ok);
    CHECK(mask == 0);
}

}  // namespace

int main() {
    TestEmpty();
    TestOneInline();
    TestSeveralInline();
    TestFreeSlotSkipped();
    TestHeapFlags();
    TestHeapFlagsSmall();
    TestBadEntries();
    TestBadLayout();
    TestUnreadable();
    TestOldLayoutRejected();
    if (g_fail) { std::printf("test_hsm_ue_map: %d failure(s)\n", g_fail); return 1; }
    std::printf("test_hsm_ue_map: ok\n");
    return 0;
}
