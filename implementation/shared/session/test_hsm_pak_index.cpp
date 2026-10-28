// Pak index reader (session/hsm_pak_index.h): a synthetic v11 pak (footer, primary index, full directory index) parses to
// its map packages; every truncation, wrong version, encrypted index and out-of-range offset is refused, never over-read.
#include "session/hsm_pak_index.h"
#include <cstdio>
#include <string>
#include <utility>
#include <vector>

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++g_fail; } } while (0)
using namespace hsm;

namespace {
struct Buf {
    std::vector<uint8_t> b;
    void u8(uint8_t v) { b.push_back(v); }
    void u32(uint32_t v) { for (int i = 0; i < 4; ++i) b.push_back(uint8_t(v >> (8 * i))); }
    void u64(uint64_t v) { for (int i = 0; i < 8; ++i) b.push_back(uint8_t(v >> (8 * i))); }
    void zeros(size_t n) { b.insert(b.end(), n, 0); }
    void fstr(const std::string& s) { u32(uint32_t(s.size() + 1)); b.insert(b.end(), s.begin(), s.end()); u8(0); }
    void wstr(const std::string& s) { u32(uint32_t(-int32_t(s.size() + 1))); for (char c : s) { u8(uint8_t(c)); u8(0); } u8(0); u8(0); }
};
using Dirs = std::vector<std::pair<std::string, std::vector<std::string>>>;

struct Pak {
    std::vector<uint8_t> file;
    size_t primaryOff = 0, primarySize = 0, fdiOff = 0, fdiSize = 0;
};

Pak Build(const std::string& mount, const Dirs& dirs, uint32_t version = 11, bool encrypted = false, bool wide = false) {
    Pak p;
    Buf data; data.zeros(100);                                         // stand-in for the file data
    Buf fdi;
    fdi.u32(uint32_t(dirs.size()));
    for (const auto& d : dirs) {
        if (wide) fdi.wstr(d.first); else fdi.fstr(d.first);
        fdi.u32(uint32_t(d.second.size()));
        for (const std::string& f : d.second) { fdi.fstr(f); fdi.u32(0); }
    }
    auto primary = [&](uint64_t fdiOff) {
        Buf b; b.fstr(mount); b.u32(7); b.u64(0x1234);
        b.u32(0);                                                      // no path hash index
        b.u32(1); b.u64(fdiOff); b.u64(fdi.b.size()); b.zeros(20);     // full directory index
        b.u32(0); b.u32(0);                                            // encoded entries, non-encoded entries
        return b;
    };
    const size_t primarySize = primary(0).b.size();
    p.primaryOff = data.b.size(); p.primarySize = primarySize;
    p.fdiOff = p.primaryOff + primarySize; p.fdiSize = fdi.b.size();
    Buf f = data;
    const Buf pri = primary(p.fdiOff);
    f.b.insert(f.b.end(), pri.b.begin(), pri.b.end());
    f.b.insert(f.b.end(), fdi.b.begin(), fdi.b.end());
    f.zeros(16); f.u8(encrypted ? 1 : 0); f.u32(kPakMagic); f.u32(version);
    f.u64(p.primaryOff); f.u64(p.primarySize); f.zeros(20); f.zeros(160);
    p.file = f.b;
    return p;
}
std::vector<uint8_t> Tail(const Pak& p) {
    const size_t n = p.file.size() < kPakFooterReadBytes ? p.file.size() : kPakFooterReadBytes;
    return std::vector<uint8_t>(p.file.end() - long(n), p.file.end());
}
const Dirs kSample = {
    {"HalfswordUE5/Content/Maps/Custom/", {"Arena_Bog.umap", "Arena_Bog.uasset", "Arena_Bog_Lighting.umap", "Arena_Bog.uexp"}},
    {"HalfswordUE5/Content/Maps/Arenas/", {"Map_Arena_Yard.umap"}},
    {"HalfswordUE5/Content/Textures/", {"T_Bog.uasset"}},
};
}  // namespace

static void TestPackagePaths() {
    CHECK(PakFileToPackagePath("../../../HalfswordUE5/Content/Maps/Foo/Bar.umap") == "/Game/Maps/Foo/Bar");
    CHECK(PakFileToPackagePath("../../../HalfswordUE5/Content/Maps/Foo/Bar.UMAP") == "/Game/Maps/Foo/Bar");
    CHECK(PakFileToPackagePath("HalfswordUE5/Content/Bar.umap") == "/Game/Bar");
    CHECK(PakFileToPackagePath("..\\..\\..\\HalfswordUE5\\Content\\Maps\\Bar.umap") == "/Game/Maps/Bar");
    CHECK(PakFileToPackagePath("../../../Engine/Content/Maps/Entry.umap") == "/Engine/Maps/Entry");
    CHECK(PakFileToPackagePath("../../../HalfswordUE5/Plugins/BogMod/Content/Maps/Bog.umap") == "/BogMod/Maps/Bog");
    CHECK(PakFileToPackagePath("../../../HalfswordUE5/Plugins/Gfx/Sub/Content/Maps/Bog.umap") == "/Sub/Maps/Bog");
    CHECK(PakFileToPackagePath("../../../HalfswordUE5/Content/Maps/Bar.uasset").empty());           // not a map
    CHECK(PakFileToPackagePath("../../../HalfswordUE5/Content/Maps/Bar.umap.bak").empty());
    CHECK(PakFileToPackagePath("../../../Other/Content/Maps/Bar.umap").empty());                    // another project
    CHECK(PakFileToPackagePath("../../../HalfswordUE5/Config/Bar.umap").empty());                   // not under Content
    CHECK(PakFileToPackagePath("../../../HalfswordUE5/Plugins/BogMod/Maps/Bar.umap").empty());      // no Content folder
    CHECK(PakFileToPackagePath("../../../HalfswordUE5/Content/Maps/Bar Baz.umap").empty());         // a space
    CHECK(PakFileToPackagePath("../../../HalfswordUE5/Content/Maps/Ba.r.umap").empty());            // a dot
    CHECK(PakFileToPackagePath("../../../HalfswordUE5/Content/../Bar.umap").empty());               // a dotdot after the root
    CHECK(PakFileToPackagePath("../../../HalfswordUE5/Content/.umap").empty());
    CHECK(PakFileToPackagePath("Bar.umap").empty() && PakFileToPackagePath("").empty() && PakFileToPackagePath(".umap").empty());
    CHECK(PakFileToPackagePath("../../../HalfswordUE5/Content").empty());
}

static void TestParse() {
    const Pak p = Build("../../../", kSample);
    const std::vector<uint8_t> tail = Tail(p);
    PakFooter f;
    CHECK(ParsePakFooter(tail.data(), tail.size(), p.file.size(), f) == PakParse::Ok);
    CHECK(f.version == 11 && !f.encryptedIndex && f.indexOffset == p.primaryOff && f.indexSize == p.primarySize);
    PakPrimary pr;
    CHECK(ParsePakPrimaryIndex(p.file.data() + f.indexOffset, size_t(f.indexSize), p.file.size(), pr) == PakParse::Ok);
    CHECK(pr.mountPoint == "../../../" && pr.numEntries == 7 && pr.hasFullDir && pr.fdiOffset == p.fdiOff && pr.fdiSize == p.fdiSize);
    std::vector<std::string> maps;
    CHECK(ParsePakDirectoryIndex(p.file.data() + pr.fdiOffset, size_t(pr.fdiSize), pr.mountPoint, maps) == PakParse::Ok);
    CHECK(maps.size() == 3);
    CHECK(maps.size() == 3 && maps[0] == "/Game/Maps/Custom/Arena_Bog" && maps[1] == "/Game/Maps/Custom/Arena_Bog_Lighting" && maps[2] == "/Game/Maps/Arenas/Map_Arena_Yard");

    // Version 10 reads the same; the mount point may carry the project, the root directory is "/".
    const Pak p10 = Build("../../../HalfswordUE5/Content/", {{"/", {"Top.umap"}}, {"Maps/", {"A.umap", "A.umap"}}}, 10);
    const std::vector<uint8_t> t10 = Tail(p10);
    CHECK(ParsePakFooter(t10.data(), t10.size(), p10.file.size(), f) == PakParse::Ok && f.version == 10);
    CHECK(ParsePakPrimaryIndex(p10.file.data() + f.indexOffset, size_t(f.indexSize), p10.file.size(), pr) == PakParse::Ok);
    CHECK(ParsePakDirectoryIndex(p10.file.data() + pr.fdiOffset, size_t(pr.fdiSize), pr.mountPoint, maps) == PakParse::Ok);
    CHECK(maps.size() == 2 && maps[0] == "/Game/Top" && maps[1] == "/Game/Maps/A");   // duplicates collapse

    // UTF-16 directory names read the same.
    const Pak pw = Build("../../../", {{"HalfswordUE5/Content/Maps/", {"W.umap"}}}, 11, false, true);
    const std::vector<uint8_t> tw = Tail(pw);
    CHECK(ParsePakFooter(tw.data(), tw.size(), pw.file.size(), f) == PakParse::Ok);
    CHECK(ParsePakPrimaryIndex(pw.file.data() + f.indexOffset, size_t(f.indexSize), pw.file.size(), pr) == PakParse::Ok);
    CHECK(ParsePakDirectoryIndex(pw.file.data() + pr.fdiOffset, size_t(pr.fdiSize), pr.mountPoint, maps) == PakParse::Ok && maps.size() == 1 && maps[0] == "/Game/Maps/W");

    // A pak with no maps parses to an empty list.
    const Pak pe = Build("../../../", {{"HalfswordUE5/Content/Textures/", {"T.uasset"}}});
    const std::vector<uint8_t> te = Tail(pe);
    CHECK(ParsePakFooter(te.data(), te.size(), pe.file.size(), f) == PakParse::Ok);
    CHECK(ParsePakPrimaryIndex(pe.file.data() + f.indexOffset, size_t(f.indexSize), pe.file.size(), pr) == PakParse::Ok);
    CHECK(ParsePakDirectoryIndex(pe.file.data() + pr.fdiOffset, size_t(pr.fdiSize), pr.mountPoint, maps) == PakParse::Ok && maps.empty());
}

static void TestFooterRejections() {
    PakFooter f;
    const Pak p = Build("../../../", kSample);
    const std::vector<uint8_t> tail = Tail(p);
    // Every truncation of the tail is "not a pak" (the magic is gone) or malformed: never Ok.
    for (size_t n = 0; n < 204; ++n) CHECK(ParsePakFooter(tail.data() + (tail.size() - n), n, p.file.size(), f) != PakParse::Ok);
    CHECK(ParsePakFooter(nullptr, 0, 0, f) == PakParse::NotPak);
    // An unrelated file.
    std::vector<uint8_t> junk(600, 0x5A);
    CHECK(ParsePakFooter(junk.data(), junk.size(), 100000, f) == PakParse::NotPak);
    // Versions: older and newer than the layout this reads.
    for (uint32_t v : {1u, 3u, 8u, 9u, 12u, 13u}) {
        const Pak pv = Build("../../../", kSample, v);
        const std::vector<uint8_t> tv = Tail(pv);
        CHECK(ParsePakFooter(tv.data(), tv.size(), pv.file.size(), f) == PakParse::Unsupported);
    }
    // A version number that is not one (a magic by chance) is skipped, not trusted.
    { const Pak pv = Build("../../../", kSample, 4000); const std::vector<uint8_t> tv = Tail(pv); CHECK(ParsePakFooter(tv.data(), tv.size(), pv.file.size(), f) == PakParse::NotPak); }
    // Encrypted index (the stock game's own pak).
    { const Pak pe = Build("../../../", kSample, 11, true); const std::vector<uint8_t> te = Tail(pe); CHECK(ParsePakFooter(te.data(), te.size(), pe.file.size(), f) == PakParse::Encrypted && f.encryptedIndex); }
    // The index must lie inside the file; empty and oversized indexes are refused.
    { std::vector<uint8_t> t = tail; const size_t at = t.size() - 221 + 17 + 8;   // index offset field
      for (int i = 0; i < 8; ++i) t[at + size_t(i)] = 0xFF;
      CHECK(ParsePakFooter(t.data(), t.size(), p.file.size(), f) == PakParse::Malformed); }
    { std::vector<uint8_t> t = tail; const size_t at = t.size() - 221 + 17 + 16;  // index size field
      for (int i = 0; i < 8; ++i) t[at + size_t(i)] = 0;
      CHECK(ParsePakFooter(t.data(), t.size(), p.file.size(), f) == PakParse::Malformed);
      for (int i = 0; i < 8; ++i) t[at + size_t(i)] = i == 5 ? 0x10 : 0;          // 16 TiB: past the file
      CHECK(ParsePakFooter(t.data(), t.size(), p.file.size(), f) == PakParse::Malformed);
      CHECK(ParsePakFooter(t.data(), t.size(), uint64_t(1) << 50, f) == PakParse::TooBig); }
}

static void TestIndexRejections() {
    const Pak p = Build("../../../", kSample);
    PakPrimary pr;
    const uint8_t* pri = p.file.data() + p.primaryOff;
    // Every truncation of the primary index.
    for (size_t n = 0; n + 8 < p.primarySize; ++n) CHECK(ParsePakPrimaryIndex(pri, n, p.file.size(), pr) != PakParse::Ok);
    // A directory index outside the file / too big / a bad flag.
    { std::vector<uint8_t> b(pri, pri + p.primarySize); const size_t mp = 4 + 10;  // after the mount FString ("../../../" + NUL)
      const size_t fdiFlag = mp + 4 + 8 + 4;
      std::vector<uint8_t> c = b; c[fdiFlag] = 2; CHECK(ParsePakPrimaryIndex(c.data(), c.size(), p.file.size(), pr) == PakParse::Malformed);
      c = b; c[fdiFlag] = 0; CHECK(ParsePakPrimaryIndex(c.data(), c.size(), p.file.size(), pr) == PakParse::Unsupported);
      c = b; for (int i = 0; i < 8; ++i) c[fdiFlag + 4 + size_t(i)] = 0xFF; CHECK(ParsePakPrimaryIndex(c.data(), c.size(), p.file.size(), pr) == PakParse::Malformed);
      c = b; c[fdiFlag + 4 + 12] = 0x7F; CHECK(ParsePakPrimaryIndex(c.data(), c.size(), uint64_t(1) << 60, pr) != PakParse::Ok); }
    // A mount point longer than the cap.
    { Buf b; b.u32(uint32_t(kMaxPakPathChars + 1)); b.zeros(kMaxPakPathChars + 1); b.zeros(64);
      CHECK(ParsePakPrimaryIndex(b.b.data(), b.b.size(), 1u << 20, pr) == PakParse::Malformed); }
    // A mount point whose terminator is not NUL.
    { Buf b; b.u32(3); b.u8('a'); b.u8('b'); b.u8('c'); b.zeros(64);
      CHECK(ParsePakPrimaryIndex(b.b.data(), b.b.size(), 1u << 20, pr) == PakParse::Malformed); }

    // The directory index: every truncation, a negative count, trailing bytes, a count the bytes cannot hold.
    std::vector<std::string> maps;
    const uint8_t* fdi = p.file.data() + p.fdiOff;
    for (size_t n = 0; n < p.fdiSize; ++n) CHECK(ParsePakDirectoryIndex(fdi, n, "../../../", maps) != PakParse::Ok);
    { std::vector<uint8_t> b(fdi, fdi + p.fdiSize); b.push_back(0); CHECK(ParsePakDirectoryIndex(b.data(), b.size(), "../../../", maps) == PakParse::Malformed); }
    { std::vector<uint8_t> b(fdi, fdi + p.fdiSize); b[3] = 0x80; CHECK(ParsePakDirectoryIndex(b.data(), b.size(), "../../../", maps) == PakParse::Malformed); }
    { std::vector<uint8_t> b(fdi, fdi + p.fdiSize); b[3] = 0x7F; CHECK(ParsePakDirectoryIndex(b.data(), b.size(), "../../../", maps) == PakParse::Malformed); }
    CHECK(ParsePakDirectoryIndex(nullptr, 0, "", maps) == PakParse::Malformed);
    { Buf b; b.u32(0); CHECK(ParsePakDirectoryIndex(b.b.data(), b.b.size(), "", maps) == PakParse::Ok && maps.empty()); }
    { Buf b; b.u32(0xFFFFFFFFu); CHECK(ParsePakDirectoryIndex(b.b.data(), b.b.size(), "", maps) == PakParse::Malformed); }
    // The per-pak map cap.
    { Dirs many(1); many[0].first = "HalfswordUE5/Content/M/";
      for (size_t i = 0; i < kMaxPakMaps + 40; ++i) many[0].second.push_back("Map" + std::to_string(i) + ".umap");
      const Pak pm = Build("../../../", many);
      CHECK(ParsePakDirectoryIndex(pm.file.data() + pm.fdiOff, pm.fdiSize, "../../../", maps) == PakParse::Ok && maps.size() == kMaxPakMaps); }
}

int main() {
    TestPackagePaths();
    TestParse();
    TestFooterRejections();
    TestIndexRejections();
    if (g_fail) { std::printf("%d failure(s)\n", g_fail); return 1; }
    std::printf("ok\n");
    return 0;
}
