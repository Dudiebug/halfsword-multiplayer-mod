// Unit tests for hsm_pe.h on a tiny synthetic PE. Run via implementation/shared/run_tests.sh test_hsm_pe.
#include "session/hsm_pe.h"
#include <cstdio>
#include <cstring>

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++g_fail; } } while (0)
using namespace hsm;

namespace {
void P16(std::vector<uint8_t>& b, size_t o, uint32_t v) { b[o] = uint8_t(v); b[o + 1] = uint8_t(v >> 8); }
void P32(std::vector<uint8_t>& b, size_t o, uint32_t v) { P16(b, o, v); P16(b, o + 2, v >> 16); }

// One PE32+ DLL: DOS header, PE header at 0x80, optional header of 240 bytes, one section .edata at RVA 0x1000 whose
// file data starts at 0x200. The export directory sits at its start; names follow.
std::vector<uint8_t> MakePe(const std::vector<std::string>& names, uint16_t machine = 0x8664, uint16_t chars = 0x2022, uint16_t magic = 0x20B) {
    std::vector<uint8_t> b(0x200 + 0x400, 0);
    b[0] = 'M'; b[1] = 'Z';
    P32(b, 0x3C, 0x80);
    const size_t pe = 0x80;
    b[pe] = 'P'; b[pe + 1] = 'E';
    P16(b, pe + 4, machine);
    P16(b, pe + 6, 1);          // sections
    P16(b, pe + 20, 240);       // optional header size
    P16(b, pe + 22, chars);
    P16(b, pe + 24, magic);
    P32(b, pe + 24 + 112, 0x1000);   // export directory RVA
    P32(b, pe + 24 + 116, 0x200);    // size
    const size_t sec = pe + 24 + 240;
    std::memcpy(&b[sec], ".edata", 6);
    P32(b, sec + 8, 0x400);     // virtual size
    P32(b, sec + 12, 0x1000);   // virtual address
    P32(b, sec + 16, 0x400);    // raw size
    P32(b, sec + 20, 0x200);    // raw pointer
    const size_t dir = 0x200;
    P32(b, dir + 24, uint32_t(names.size()));
    P32(b, dir + 32, 0x1000 + 0x40);      // address of names table
    size_t str = dir + 0x40 + names.size() * 4;
    for (size_t i = 0; i < names.size(); ++i) {
        P32(b, dir + 0x40 + i * 4, uint32_t(0x1000 + (str - dir)));
        std::memcpy(&b[str], names[i].c_str(), names[i].size() + 1);
        str += names[i].size() + 1;
    }
    return b;
}
PeVerdict Check(const std::vector<uint8_t>& b, const std::vector<std::string>& req = DwmapiRequiredExports()) { return CheckProxyDll(b.data(), b.size(), req); }
const std::vector<std::string> kAll = {"DwmFlush", "DwmGetCompositionTimingInfo", "DwmIsCompositionEnabled", "DwmSetWindowAttribute", "DwmExtra"};
}  // namespace

static void TestGood() {
    CHECK(Check(MakePe(kAll)) == PeVerdict::Ok);
    CHECK(Check(MakePe({"DwmIsCompositionEnabled"}), {"DwmIsCompositionEnabled"}) == PeVerdict::Ok);
    CHECK(Check(MakePe(kAll), {}) == PeVerdict::Ok);   // nothing required
}

static void TestNotPe() {
    CHECK(CheckProxyDll(nullptr, 0, {}) == PeVerdict::TooSmall);
    CHECK(Check(std::vector<uint8_t>(10, 0)) == PeVerdict::TooSmall);
    std::string html = "<!DOCTYPE html><html><body>Not Found. This is an error page and not a DLL at all, sorry</body></html>";
    CHECK(CheckProxyDll(reinterpret_cast<const uint8_t*>(html.data()), html.size(), {}) == PeVerdict::NotMz);
    auto b = MakePe(kAll);
    b[0x80] = 'X';
    CHECK(Check(b) == PeVerdict::NotPe);
    b = MakePe(kAll);
    P32(b, 0x3C, 0xFFFFFFF0u);   // e_lfanew far outside the file
    CHECK(Check(b) == PeVerdict::Truncated);
}

static void TestHeaders() {
    CHECK(Check(MakePe(kAll, 0x14C)) == PeVerdict::WrongMachine);     // x86
    CHECK(Check(MakePe(kAll, 0xAA64)) == PeVerdict::WrongMachine);    // arm64
    CHECK(Check(MakePe(kAll, 0x8664, 0x0022)) == PeVerdict::NotDll);  // an exe
    CHECK(Check(MakePe(kAll, 0x8664, 0x2022, 0x10B)) == PeVerdict::NotPe64);
    auto b = MakePe(kAll);
    P32(b, 0x80 + 24 + 112, 0);
    CHECK(Check(b) == PeVerdict::NoExports);
    b = MakePe(kAll);
    P16(b, 0x80 + 6, 0);
    CHECK(Check(b) == PeVerdict::NotPe);
    P16(b, 0x80 + 6, 5000);
    CHECK(Check(b) == PeVerdict::NotPe);
    b = MakePe(kAll);
    P16(b, 0x80 + 20, 64);   // optional header too short for an export directory entry
    CHECK(Check(b) == PeVerdict::NoExports);
    b = MakePe(kAll);
    P32(b, 0x200 + 24, 0);   // zero names
    CHECK(Check(b) == PeVerdict::NoExports);
    P32(b, 0x200 + 24, 100000);
    CHECK(Check(b) == PeVerdict::NotPe);
}

static void TestExports() {
    CHECK(Check(MakePe({"DwmFlush", "DwmGetCompositionTimingInfo", "DwmIsCompositionEnabled"})) == PeVerdict::MissingExport);
    CHECK(Check(MakePe({"dwmflush", "DwmGetCompositionTimingInfo", "DwmIsCompositionEnabled", "DwmSetWindowAttribute"})) == PeVerdict::MissingExport);   // case matters
    CHECK(Check(MakePe({"DwmFlushX", "DwmGetCompositionTimingInfo", "DwmIsCompositionEnabled", "DwmSetWindowAttribute"})) == PeVerdict::MissingExport);
    CHECK(Check(MakePe({"Other"})) == PeVerdict::MissingExport);
}

static void TestTruncated() {
    const auto full = MakePe(kAll);
    // Every prefix that stops before the last export name ends is refused, and none reads out of bounds (ASan runs this).
    size_t endOfStrings = 0x200 + 0x40 + kAll.size() * 4;
    for (const std::string& n : kAll) endOfStrings += n.size() + 1;
    CHECK(endOfStrings < full.size());
    for (size_t n = 0; n < endOfStrings; ++n) CHECK(CheckProxyDll(full.data(), n, DwmapiRequiredExports()) != PeVerdict::Ok);
    CHECK(CheckProxyDll(full.data(), endOfStrings, DwmapiRequiredExports()) == PeVerdict::Ok);
    CHECK(CheckProxyDll(full.data(), 0x200 + 0x40, DwmapiRequiredExports()) == PeVerdict::Truncated);
    // A section that claims more raw data than the file holds.
    auto b = full;
    P32(b, 0x80 + 24 + 240 + 16, 0x4000);
    CHECK(CheckProxyDll(b.data(), 0x230, DwmapiRequiredExports()) == PeVerdict::Truncated);
    // A name RVA outside every section.
    b = full;
    P32(b, 0x200 + 0x40, 0x90000);
    CHECK(Check(b) == PeVerdict::Truncated);
    // A name with no terminator before the end of the file.
    b = full;
    b.resize(0x200 + 0x40 + kAll.size() * 4 + 2);
    CHECK(Check(b) == PeVerdict::Truncated);
}

int main() {
    TestGood();
    TestNotPe();
    TestHeaders();
    TestExports();
    TestTruncated();
    if (g_fail) { std::printf("test_hsm_pe: %d failure(s)\n", g_fail); return 1; }
    std::printf("test_hsm_pe: ok\n");
    return 0;
}
