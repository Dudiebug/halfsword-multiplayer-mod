// Signature parsing and search, rel32 targets and the call scan the engine layer uses on an exe it has no table for.
#include "util/hsm_sigscan.h"
#include <cstdio>

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++g_fail; } } while (0)
using namespace hsm::util::sig;

static void TestParse() {
    Pattern p = Parse("48 8B 05 ?? ?? ?? ?? 48 8b");
    CHECK(p.size() == 9);
    CHECK(p.bytes[0] == 0x48 && p.bytes[1] == 0x8B && p.bytes[2] == 0x05 && p.bytes[8] == 0x8B);
    CHECK(p.mask[0] && p.mask[2] && !p.mask[3] && !p.mask[6] && p.mask[7]);
    CHECK(Parse("E8").size() == 1);
    CHECK(Parse("").empty());
    CHECK(Parse("?? 48").empty());          // the first byte must be fixed
    CHECK(Parse("48 8").empty());
    CHECK(Parse("48  8B").empty());         // one space only
    CHECK(Parse("48 8B ").empty());
    CHECK(Parse("4G").empty());
    CHECK(Parse("48,8B").empty());
    CHECK(Parse("48 ?").empty());
}

static void TestFind() {
    const uint8_t block[] = {0x90, 0x48, 0x8B, 0x05, 1, 2, 3, 4, 0xC3, 0x48, 0x8B, 0x0D, 5, 6, 7, 8, 0xC3, 0x48, 0x8B, 0x05, 9, 9, 9, 9, 0xCC};
    Hits h = Find(block, sizeof block, Parse("48 8B 05 ?? ?? ?? ?? C3"));
    CHECK(h.count == 1 && h.first == 1);                               // the third one ends in CC, not C3
    h = Find(block, sizeof block, Parse("48 8B 05 ?? ?? ?? ??"));
    CHECK(h.count == 2 && h.first == 1);                               // not unique
    h = Find(block, sizeof block, Parse("48 8B 05 ?? ?? ?? ??"), 5);
    CHECK(h.count == 2);
    h = Find(block, sizeof block, Parse("48 8B ?? ?? ?? ?? ?? C3"), 5);
    CHECK(h.count == 2 && h.first == 1);                               // 05 and 0D both match the wildcard
    CHECK(Find(block, sizeof block, Parse("48 8B 15")).count == 0);
    CHECK(Find(block, sizeof block, Parse("")).count == 0);
    CHECK(Find(block, 3, Parse("48 8B 05 ??")).count == 0);            // longer than the block
    CHECK(Find(block, sizeof block, Parse("48 8B 05 09 09 09 09 CC")).first == 17);   // the very end of the block
    CHECK(Find(block, sizeof block - 1, Parse("48 8B 05 09 09 09 09 CC")).count == 0);
    const uint8_t raw[] = {0x48, 0x8B, 0x0D, 5};
    Pattern e = Exact(raw, sizeof raw);
    CHECK(e.size() == 4 && e.mask[0] && e.mask[3] && e.bytes[2] == 0x0D);
    h = Find(block, sizeof block, e);
    CHECK(h.count == 1 && h.first == 9);
    CHECK(Exact(raw, 0).empty());
}

static void TestRel32() {
    uint8_t b[16] = {};
    const int32_t back = -0x20, fwd = 0x1000;
    std::memcpy(b + 3, &back, 4);
    CHECK(Rel32Target(b, 3, 7) == 7 - 0x20);
    std::memcpy(b + 3, &fwd, 4);
    CHECK(Rel32Target(b, 3, 7) == 7 + 0x1000);
}

static void TestCalls() {
    // 0..: a function that calls 0x40 (no prologue), then 0x60 (prologue) twice; 0x60: the prologue.
    uint8_t b[0x80] = {};
    auto call = [&](size_t at, size_t target) { b[at] = 0xE8; const int32_t r = int32_t(int64_t(target) - int64_t(at + 5)); std::memcpy(b + at + 1, &r, 4); };
    call(0x02, 0x40);
    call(0x10, 0x60);
    call(0x20, 0x60);
    const uint8_t prologue[] = {0x48, 0x89, 0x5C, 0x24, 0x08};
    std::memcpy(b + 0x60, prologue, sizeof prologue);
    std::vector<Call> calls = Calls(b, sizeof b, 0, 0x30);
    CHECK(calls.size() == 3 && calls[0].at == 0x02 && calls[0].target == 0x40 && calls[2].target == 0x60);
    size_t at = 0;
    CHECK(OnlyCallTo(b, sizeof b, 0, 0x30, prologue, sizeof prologue, &at) == 0x60 && at == 0x10);   // one callee, called twice
    CHECK(OnlyCallTo(b, sizeof b, 0, 0x10, prologue, sizeof prologue) == -1);   // the range ends before the call
    CHECK(OnlyCallTo(b, sizeof b, 0, 0x14, prologue, sizeof prologue) == -1);   // a call cut off by the range end
    std::memcpy(b + 0x40, prologue, sizeof prologue);
    CHECK(OnlyCallTo(b, sizeof b, 0, 0x30, prologue, sizeof prologue) == -1);   // two callees with the prologue
    call(0x30, 0x7E);                                                           // target too near the block's end
    CHECK(OnlyCallTo(b, sizeof b, 0x2E, 0x40, prologue, sizeof prologue) == -1);
    call(0x38, size_t(-0x100 + 0x3D));                                          // a call before the block
    CHECK(Calls(b, sizeof b, 0x38, 0x3D).size() == 1 && Calls(b, sizeof b, 0x38, 0x3D)[0].target < 0);
    CHECK(OnlyCallTo(b, sizeof b, 0x38, 0x3D, prologue, sizeof prologue) == -1);
    CHECK(Calls(b, sizeof b, 0x70, 0x1000).empty());                            // end past the block is clamped
    // A predicate: either the prologue or a jump (E9) at the target.
    uint8_t c[0x80] = {};
    auto callc = [&](size_t at, size_t target) { c[at] = 0xE8; const int32_t r = int32_t(int64_t(target) - int64_t(at + 5)); std::memcpy(c + at + 1, &r, 4); };
    callc(0x00, 0x40);
    callc(0x08, 0x50);
    c[0x50] = 0xE9;
    auto jumpOrPrologue = [&](size_t t) { return c[t] == 0xE9 || std::memcmp(c + t, prologue, sizeof prologue) == 0; };
    at = 99;
    CHECK(OnlyCallWhere(c, sizeof c, 0, 0x10, jumpOrPrologue, &at) == 0x50 && at == 0x08);
    std::memcpy(c + 0x40, prologue, sizeof prologue);
    CHECK(OnlyCallWhere(c, sizeof c, 0, 0x10, jumpOrPrologue) == -1);           // both accepted: ambiguous
    int offered = 0;
    callc(0x10, 0x7F + 0x20);                                                   // past the block: never offered
    CHECK(OnlyCallWhere(c, sizeof c, 0x10, 0x15, [&](size_t) { ++offered; return true; }) == -1 && offered == 0);
}

int main() {
    TestParse();
    TestFind();
    TestRel32();
    TestCalls();
    if (g_fail) { std::printf("test_hsm_sigscan: %d failure(s)\n", g_fail); return 1; }
    std::printf("test_hsm_sigscan: all passed\n");
    return 0;
}
