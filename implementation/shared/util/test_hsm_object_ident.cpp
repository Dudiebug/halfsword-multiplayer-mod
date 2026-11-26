// The object identity rules (util/hsm_object_ident.h): the junk values the 0.8.0-pre52..pre56 crash reports recorded where
// an object pointer or its class should have been, a cache entry whose function was freed and its address reused, and a
// cost table that must never name a freed function.
#include "util/hsm_object_ident.h"
#include <cstdio>
#include <unordered_map>
#include <vector>

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++g_fail; } } while (0)
using namespace hsm::util;

static void TestPlausibleAddress() {
    // Group A: what HitHook read as OtherActor (IsA +5 faulted on it): packed 32-bit pairs, odd or misaligned.
    const uint64_t otherActor[] = {0x23406, 0x1C300056AA9ull, 0x28200022589ull, 0x2BA00064ABDull, 0x28C3B6000366FCull, 0x77583, 0x48E8,
                                   0x1C200008375ull, 0x23AE3, 0x6193, 0x407FF,
                                   0x31A87, 0x1B14B, 0xEC4934370008F441ull, 0x179BE, 0x1F70001D1C5ull, 0xC368};
    for (uint64_t a : otherActor) CHECK(!PlausibleObjectAddress(a));
    // ... and what sat where the class of those objects should have been (IsA +0x20).
    const uint64_t classValue[] = {0x6C, 0xED6D09BE12566280ull, 0x73000000000000ull, 0x6C, 0x72657473756C63ull /* "cluster" */,
                                   0x6C006C0064ull /* UTF-16 "dll" */, 0x6429C43A6429C43Aull, 0x9F0000BF9F0000BFull};
    for (uint64_t a : classValue) CHECK(!PlausibleObjectAddress(a));
    CHECK(!PlausibleObjectAddress(uint64_t(0x10000)));      // Group B: a fighter's slot variable held this in four reports
    CHECK(!PlausibleObjectAddress(uint64_t(0xFFFFFFFF)));   // Group D: a class pointer in the object array scan
    CHECK(!PlausibleObjectAddress(uint64_t(0)));
    CHECK(!PlausibleObjectAddress(uint64_t(0x80000)));
    CHECK(!PlausibleObjectAddress(uint64_t(0xFFFF8)));
    // Real heap addresses of the reports' machines.
    CHECK(PlausibleObjectAddress(uint64_t(0x0000024FA1B2C3D0ull)));
    CHECK(PlausibleObjectAddress(uint64_t(0x1CD0CAC3700ull)));
    CHECK(PlausibleObjectAddress(uint64_t(0x7FFFFFFFFFF8ull)));
    CHECK(PlausibleObjectAddress(uint64_t(0x100000)));
    // The bounds and the alignment, one at a time.
    CHECK(!PlausibleObjectAddress(uint64_t(0x1CD0CAC3701ull)));
    CHECK(!PlausibleObjectAddress(uint64_t(0x1CD0CAC3704ull)));
    CHECK(!PlausibleObjectAddress(uint64_t(0x800000000000ull)));
    CHECK(!PlausibleObjectAddress(uint64_t(0xFFFF800000000000ull)));
    // What the rule cannot know: 0x6C0000 is 8-aligned and in range, but unmapped. The engine layer's object array and
    // vtable checks catch it; this rule must not be tightened to pretend it does.
    CHECK(PlausibleObjectAddress(uint64_t(0x6C0000)));
    const int x = 0;
    CHECK(PlausibleObjectAddress(static_cast<const void*>(&x)) == PlausibleObjectAddress(uint64_t(reinterpret_cast<uintptr_t>(&x))));
    CHECK(!PlausibleObjectAddress(static_cast<const void*>(nullptr)));
}

// A stand-in for the object array: slots hold object addresses, freed slots hold null; `reads` counts reads of an
// object's own memory (a freed object's must be none).
struct FakeObjects {
    std::vector<const void*> slot;
    std::vector<uint64_t> name;
    int reads = 0;
    int32_t Add(const void* addr, uint64_t n) { slot.push_back(addr); name.push_back(n); return int32_t(slot.size()) - 1; }
    const void* SlotHolds(int32_t i) const { return i >= 0 && size_t(i) < slot.size() ? slot[size_t(i)] : nullptr; }
    ObjectIdent Read(int32_t i) { ++reads; return {i, name[size_t(i)]}; }
    bool Current(const void* key, const ObjectIdent& stored) {
        return CacheEntryCurrent(stored, key, SlotHolds(stored.index), [&] { return Read(stored.index); });
    }
};

static void TestCacheEntryCurrent() {
    FakeObjects o;
    int a = 0, b = 0;
    const int32_t ia = o.Add(&a, NameKey(100, 0));
    const ObjectIdent stored{ia, NameKey(100, 0)};
    CHECK(o.Current(&a, stored));                            // the same function
    CHECK(!o.Current(&b, stored));                           // another address
    CHECK(!o.Current(nullptr, stored));
    CHECK(!o.Current(&a, ObjectIdent{}));                    // an entry with no identity is never current
    // The function was collected: its slot is empty. The memory is never read.
    o.slot[size_t(ia)] = nullptr; o.reads = 0;
    CHECK(!o.Current(&a, stored));
    CHECK(o.reads == 0);
    // Its address came back as another function in another slot: the stored slot no longer holds this key.
    o.Add(&a, NameKey(200, 0));
    CHECK(!o.Current(&a, stored));
    CHECK(o.reads == 0);
    // The slot was reused for another object with the same name (a block's function made again elsewhere): not this key,
    // and this key's memory is not read.
    o.slot[size_t(ia)] = &b; o.reads = 0;
    CHECK(!o.Current(&a, stored));
    CHECK(o.reads == 0);
    // The slot was reused for the same address with another name: the name tells it apart.
    o.slot[size_t(ia)] = &a; o.name[size_t(ia)] = NameKey(200, 0);
    CHECK(!o.Current(&a, stored));
    o.name[size_t(ia)] = NameKey(100, 1);                    // the same text, another number
    CHECK(!o.Current(&a, stored));
    o.name[size_t(ia)] = NameKey(100, 0);
    CHECK(o.Current(&a, stored));                            // and back to the original identity
    // The direct form for a function the engine just passed in: its slot and name against the stored ones.
    CHECK((ObjectIdent{ia, NameKey(100, 0)}) == stored);
    CHECK((ObjectIdent{ia, NameKey(101, 0)}) != stored);
    CHECK((ObjectIdent{ia + 1, NameKey(100, 0)}) != stored);
    CHECK(NameKey(1, 2) != NameKey(2, 1));
}

static void TestReportDue() {
    CHECK(!ReportDue(0));
    for (uint64_t i = 1; i <= 5; ++i) CHECK(ReportDue(i));
    CHECK(!ReportDue(6) && !ReportDue(999) && !ReportDue(1001));
    CHECK(ReportDue(1000) && ReportDue(2000));
}

int main() {
    TestPlausibleAddress();
    TestCacheEntryCurrent();
    TestReportDue();
    if (g_fail) { std::printf("%d failure(s)\n", g_fail); return 1; }
    std::printf("hsm_object_ident: ok\n");
    return 0;
}
