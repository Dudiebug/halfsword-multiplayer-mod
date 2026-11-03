// Unit tests for hsm_modpack.h. Run via implementation/shared/run_tests.sh test_hsm_modpack.
#include "session/hsm_modpack.h"
#include <cstdio>

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++g_fail; } } while (0)
using namespace hsm;

static const std::string kH1(64, 'a'), kH2(64, 'b');

static std::string Pak(const std::string& file, const std::string& size, const std::string& sha, const std::string& extra = "") {
    return "[pak]\nfile=" + file + "\nsize=" + size + "\nsha256=" + sha + "\n" + extra;
}
static const std::string kHead = "[pack]\nname=ArenaPack\nversion=1.2\n";

static PackManifest Good() {
    PackManifest m;
    CHECK(ParsePackManifest(kHead + Pak("Map_P.pak", "1000", kH1) + Pak("Sword_P.pak", "2000", kH2), m) == PackParse::Ok);
    return m;
}
static PackParse P(const std::string& text) { PackManifest m; return ParsePackManifest(text, m); }

static void TestParse() {
    PackManifest m = Good();
    CHECK(m.name == "ArenaPack" && m.version == "1.2" && m.paks.size() == 2);
    CHECK(m.paks[0].file == "Map_P.pak" && m.paks[0].size == 1000 && Sha256Hex(m.paks[0].sha) == kH1);
    // comments, blank lines, CRLF, spaces around keys, section case, optional notes
    CHECK(ParsePackManifest("; hi\r\n# there\r\n\r\n[PACK]\r\n Name = ArenaPack \r\nVersion=1.2\r\nformat=1\r\n[Pak]\r\nfile=a.pak\r\nsize=5\r\nsha256=" + kH1 +
                            "\r\nauthor=Jo\r\nsource=https://example.org/x\r\nlicense=CC-BY-4.0; ok\r\n", m) == PackParse::Ok);
    CHECK(m.paks[0].author == "Jo" && m.paks[0].source == "https://example.org/x" && m.paks[0].license == "CC-BY-4.0; ok");
    CHECK(P(kHead + Pak("a.pak", "5", kH1, "unknown=whatever\n")) == PackParse::Ok);   // unknown keys are ignored
    CHECK(P(kHead + Pak("a.pak", "5", kH1, "source=\n")) == PackParse::Ok);
}

static void TestInvalid() {
    CHECK(P("") == PackParse::MissingPack);
    CHECK(P(kHead) == PackParse::NoPaks);
    CHECK(P("[pack]\nversion=1\n" + Pak("a.pak", "5", kH1)) == PackParse::BadName);
    CHECK(P("[pack]\nname=X\n" + Pak("a.pak", "5", kH1)) == PackParse::BadVersion);
    for (const char* name : {"", "../x", "a b", ".hidden", "a/b", "a\\b", "x:y", "ünï", "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"})
        CHECK(P(std::string("[pack]\nname=") + name + "\nversion=1\n" + Pak("a.pak", "5", kH1)) == PackParse::BadName);
    for (const char* ver : {"", "1 2", "1/2", "12345678901234567"})
        CHECK(P(std::string("[pack]\nname=X\nversion=") + ver + "\n" + Pak("a.pak", "5", kH1)) == PackParse::BadVersion);
    CHECK(P("[pack]\nname=X\nversion=1\nformat=2\n" + Pak("a.pak", "5", kH1)) == PackParse::BadFormat);
    // file names: only A-Za-z0-9._- and .pak, no paths
    for (const char* f : {"a.txt", "a.PAK", "../a.pak", "dir/a.pak", "dir\\a.pak", "C:a.pak", "a b.pak", ".pak", "x.pak.exe", ".hidden.pak", "a*.pak", "ä.pak", ""})
        CHECK(P(kHead + Pak(f, "5", kH1)) == PackParse::BadFile);
    CHECK(P(kHead + Pak(std::string(61, 'a') + ".pak", "5", kH1)) == PackParse::BadFile);
    CHECK(P(kHead + Pak(std::string(60, 'a') + ".pak", "5", kH1)) == PackParse::Ok);
    for (const char* sz : {"0", "-1", "12x", "", "99999999999999999999", "70000000000"})
        CHECK(P(kHead + Pak("a.pak", sz, kH1)) == PackParse::BadSize);
    for (const std::string& h : {std::string(63, 'a'), std::string(65, 'a'), std::string(64, 'g'), std::string(64, '0'), std::string("")})
        CHECK(P(kHead + Pak("a.pak", "5", h)) == PackParse::BadHash);
    CHECK(P(kHead + Pak("a.pak", "5", kH1) + Pak("B.PAK", "6", kH2)) == PackParse::BadFile);   // .PAK is not a .pak name
    CHECK(P(kHead + Pak("a.pak", "5", kH1) + Pak("a.pak", "6", kH2)) == PackParse::DuplicateFile);
    CHECK(P(kHead + Pak("Ab.pak", "5", kH1) + Pak("aB.pak", "6", kH2)) == PackParse::DuplicateFile);   // case-insensitive file systems
    CHECK(P(kHead + "[pak]\nsize=5\nsha256=" + kH1 + "\n") == PackParse::BadFile);
    CHECK(P(kHead + "[pak]\nfile=a.pak\nsha256=" + kH1 + "\n") == PackParse::BadSize);
    CHECK(P(kHead + "[pak]\nfile=a.pak\nsize=5\n") == PackParse::BadHash);
    CHECK(P(kHead + Pak("a.pak", "5", kH1, "source=http://example.org\n")) == PackParse::BadSource);
    CHECK(P(kHead + Pak("a.pak", "5", kH1, "source=ftp://example.org\n")) == PackParse::BadSource);
    CHECK(P(kHead + Pak("a.pak", "5", kH1, "license=bad\x01text\n")) == PackParse::BadText);
    CHECK(P(kHead + Pak("a.pak", "5", kH1, "author=" + std::string(121, 'x') + "\n")) == PackParse::BadText);
    CHECK(P(kHead + Pak("a.pak", "5", kH1, "author=" + std::string(120, 'x') + "\n")) == PackParse::Ok);
    // structure
    CHECK(P(kHead + "[pack]\nname=Y\n" + Pak("a.pak", "5", kH1)) == PackParse::BadSection);
    CHECK(P(kHead + "[other]\n" + Pak("a.pak", "5", kH1)) == PackParse::BadSection);
    CHECK(P(kHead + "[pak\nfile=a.pak\n") == PackParse::BadSection);
    CHECK(P("name=X\n" + kHead + Pak("a.pak", "5", kH1)) == PackParse::BadLine);
    CHECK(P(kHead + "justtext\n" + Pak("a.pak", "5", kH1)) == PackParse::BadLine);
    CHECK(P(kHead + std::string(401, 'x') + "\n") == PackParse::BadLine);
    CHECK(P(Pak("a.pak", "5", kH1) + kHead) == PackParse::Ok);   // section order does not matter
    // counts and size
    std::string many = kHead;
    for (int i = 0; i < 32; ++i) many += Pak("p" + std::to_string(i) + ".pak", "5", kH1);
    CHECK(P(many) == PackParse::Ok);
    many += Pak("p32.pak", "5", kH1);
    CHECK(P(many) == PackParse::TooManyPaks);
    CHECK(P(kHead + std::string(kMaxPackManifestBytes, '\n')) == PackParse::TooBig);
    CHECK(PackParseText(PackParse::BadHash)[0] != '\0');
}

// The text tools/make_pack_manifest.py writes for one pak holding "hsmpack golden\n" (tests/test_make_pack_manifest.py has the
// same text): the strict reader here must accept what the tool writes.
static void TestGolden() {
    const std::string golden =
        "[pack]\nname=GoldenPack\nversion=1.0\nformat=1\n\n"
        "[pak]\nfile=Golden_P.pak\nsize=15\nsha256=16329c9cb641107a2365c0b35bef828aab626376b3f4dd2788ce815319f25a93\n";
    PackManifest m;
    CHECK(ParsePackManifest(golden, m) == PackParse::Ok);
    CHECK(m.name == "GoldenPack" && m.version == "1.0" && m.paks.size() == 1 && m.paks[0].file == "Golden_P.pak" && m.paks[0].size == 15);
    const uint8_t data[] = "hsmpack golden\n";
    CHECK(m.paks[0].sha == Sha256Of(data, 15));
}

static void TestIdentity() {
    PackManifest a = Good(), b = Good();
    CHECK(PackIdentity(a) == PackIdentity(b) && PackHashPrefix(a).size() == kPackHashChars);
    // file order, licence notes, and case of the file names do not matter
    std::swap(b.paks[0], b.paks[1]);
    b.paks[0].license = "something"; b.paks[1].source = "https://x";
    CHECK(PackIdentity(a) == PackIdentity(b));
    // name, version, a size, a hash, a file name do
    for (int k = 0; k < 5; ++k) {
        b = Good();
        if (k == 0) b.name = "Other";
        if (k == 1) b.version = "1.3";
        if (k == 2) b.paks[0].size += 1;
        if (k == 3) b.paks[1].sha[0] ^= 1;
        if (k == 4) b.paks[0].file = "Map2_P.pak";
        CHECK(PackIdentity(a) != PackIdentity(b));
    }
    // a pak list that is a prefix of the other's differs
    b = Good(); b.paks.pop_back();
    CHECK(PackIdentity(a) != PackIdentity(b));
}

static void TestLobbyKeys() {
    const PackManifest m = Good();
    const LobbyPack p = LobbyPackOf(m);
    CHECK(p.any() && p.name == "ArenaPack" && p.version == "1.2" && p.hash == PackHashPrefix(m));
    CHECK(ReadLobbyPack(p.name, p.version, p.hash).any());
    CHECK(!ReadLobbyPack("", "1", p.hash).any());
    CHECK(!ReadLobbyPack("../x", "1", p.hash).any());
    CHECK(!ReadLobbyPack("X", "", p.hash).any());
    CHECK(!ReadLobbyPack("X", "1", p.hash.substr(1)).any());
    CHECK(!ReadLobbyPack("X", "1", "ABCDEF0123456789").any());   // upper case is not what the host writes
    CHECK(!ReadLobbyPack("X", "1", "zzzzzzzzzzzzzzzz").any());
    CHECK(!ReadLobbyPack(std::string(33, 'x'), "1", p.hash).any());
}

static ModPak Local(const std::string& name, uint64_t size, const std::string& hex, bool hashed = true) {
    ModPak p; p.name = name; p.size = size; p.hashed = hashed;
    if (hashed) Sha256FromHex(hex, p.sha);
    return p;
}
static LocalPack Installed(const PackManifest& m) { LocalPack l; l.folder = m.name; l.valid = true; l.manifest = m; return l; }

static void TestCheck() {
    const PackManifest m = Good();
    const LobbyPack lobby = LobbyPackOf(m);
    const std::vector<LocalPack> locals{Installed(m)};
    const std::vector<ModPak> paks{Local("Map_P.pak", 1000, kH1), Local("sword_p.pak", 2000, kH2), Local("Other_P.pak", 5, kH1)};
    CHECK(CheckPack(LobbyPack{}, locals, paks, true).verdict == PackVerdict::NoPack);
    CHECK(CheckPack(lobby, locals, paks, true).verdict == PackVerdict::Ok);   // file names compare without case
    // not installed
    CHECK(CheckPack(lobby, {}, paks, true).verdict == PackVerdict::NotInstalled);
    CHECK(CheckPack(lobby, {}, paks, false).verdict == PackVerdict::Pending);
    LocalPack bad; bad.folder = "arenapack"; bad.valid = false; bad.error = PackParse::BadHash;
    CHECK(CheckPack(lobby, {bad}, paks, true).verdict == PackVerdict::BadManifest);
    // another version or another list
    PackManifest v2 = m; v2.version = "1.3";
    CHECK(CheckPack(lobby, {Installed(v2)}, paks, true).verdict == PackVerdict::DifferentPack);
    PackManifest l2 = m; l2.paks.pop_back();
    CHECK(CheckPack(lobby, {Installed(l2)}, paks, true).verdict == PackVerdict::DifferentPack);
    // a pak missing / of another size / of another hash
    CHECK(CheckPack(lobby, locals, {Local("Map_P.pak", 1000, kH1)}, true).verdict == PackVerdict::MissingPak);
    PackCheck c = CheckPack(lobby, locals, {Local("Map_P.pak", 1000, kH1)}, true);
    CHECK(c.file == "Sword_P.pak");
    c = CheckPack(lobby, locals, {Local("Map_P.pak", 1000, kH1), Local("Sword_P.pak", 2001, kH2)}, true);
    CHECK(c.verdict == PackVerdict::DifferentPak && c.file == "Sword_P.pak");
    c = CheckPack(lobby, locals, {Local("Map_P.pak", 1000, kH2), Local("Sword_P.pak", 2000, kH2)}, true);
    CHECK(c.verdict == PackVerdict::DifferentPak && c.file == "Map_P.pak");
    // waiting: the list is not complete, or a hash of the right size is still pending; a different size needs no hash
    CHECK(CheckPack(lobby, locals, {Local("Map_P.pak", 1000, kH1)}, false).verdict == PackVerdict::Pending);
    CHECK(CheckPack(lobby, locals, {Local("Map_P.pak", 1000, kH1), Local("Sword_P.pak", 2000, "", false)}, true).verdict == PackVerdict::Pending);
    CHECK(CheckPack(lobby, locals, {Local("Map_P.pak", 1000, kH1), Local("Sword_P.pak", 7, "", false)}, true).verdict == PackVerdict::DifferentPak);
    // the same pak in two folders: one right copy is enough
    CHECK(CheckPack(lobby, locals, {Local("Map_P.pak", 1000, kH2), Local("Map_P.pak", 1000, kH1), Local("Sword_P.pak", 2000, kH2)}, true).verdict == PackVerdict::Ok);
    // lines
    CHECK(PackJoinLine(lobby, CheckPack(lobby, {}, paks, true)) ==
          "This lobby uses the pack ArenaPack 1.2: install it in ~mods/ArenaPack, see the pack's page.");
    CHECK(PackJoinLine(lobby, c).find("your Map_P.pak is a different file") != std::string::npos);
    CHECK(PackJoinLine(lobby, PackCheck{PackVerdict::Ok, ""}).empty() && PackJoinLine(lobby, PackCheck{}).empty());
    CHECK(!PackJoinLine(lobby, PackCheck{PackVerdict::Pending, ""}).empty());
    CHECK(PackRowText(lobby, PackVerdict::Ok) == "pack ArenaPack");
    CHECK(PackRowText(lobby, PackVerdict::NotInstalled) == "pack ArenaPack (not installed)");
    CHECK(PackRowText(lobby, PackVerdict::MissingPak) == "pack ArenaPack (different)");
    CHECK(PackRowText(LobbyPack{}, PackVerdict::Ok).empty());
    CHECK(PackRowReason(PackVerdict::NotInstalled) == "Install the pack" && PackRowReason(PackVerdict::BadManifest) == "Install the pack");
    CHECK(PackRowReason(PackVerdict::DifferentPak) == "Your pack differs" && PackRowReason(PackVerdict::MissingPak) == "Your pack differs");
    CHECK(PackRowReason(PackVerdict::DifferentPack) == "Your pack differs" && !PackRowReason(PackVerdict::Pending).empty());
    CHECK(PackRowReason(PackVerdict::Ok).empty() && PackRowReason(PackVerdict::NoPack).empty());
}

static void TestHostChoice() {
    const PackManifest a = Good();
    PackManifest b = Good(); b.name = "Second";
    LocalPack bad; bad.folder = "Broken"; bad.valid = false; bad.error = PackParse::NoPaks;
    const std::vector<LocalPack> one{Installed(a)}, mixed{Installed(a), bad}, two{Installed(a), Installed(b)};
    std::string why;
    CHECK(ChooseHostPack({}, "", why) == nullptr && why.empty());
    const LocalPack* p = ChooseHostPack(mixed, "", why);   // one valid pack, one broken: the valid one
    CHECK(p && p->folder == "ArenaPack");
    CHECK(ChooseHostPack(two, "", why) == nullptr && !why.empty());   // several: say which
    p = ChooseHostPack(two, "second", why);
    CHECK(p && p->folder == "Second" && why.empty());
    CHECK(ChooseHostPack(one, "Missing", why) == nullptr && !why.empty());
    CHECK(ChooseHostPack(mixed, "Broken", why) == nullptr && why.find("no paks") != std::string::npos);
    CHECK(ChooseHostPack(one, "off", why) == nullptr);
    CHECK(ChooseHostPack(one, "OFF", why) == nullptr);
}

int main() {
    TestParse();
    TestInvalid();
    TestGolden();
    TestIdentity();
    TestLobbyKeys();
    TestCheck();
    TestHostChoice();
    if (g_fail) { std::printf("test_hsm_modpack: %d failure(s)\n", g_fail); return 1; }
    std::printf("test_hsm_modpack: ok\n");
    return 0;
}
