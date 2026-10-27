// Unit tests for hsm_version.h. Run via implementation/shared/run_tests.sh test_hsm_version.
#include "session/hsm_version.h"
#include <cstdio>

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++g_fail; } } while (0)
using namespace hsm;

static Version V(const char* s) { auto v = ParseVersion(s); return v ? *v : Version{-1, -1, -1, -1}; }

static void TestParse() {
    CHECK(V("v0.7.0-pre2").major == 0 && V("v0.7.0-pre2").minor == 7 && V("v0.7.0-pre2").pre == 2);
    CHECK(V("0.6.5").patch == 5 && !V("0.6.5").prerelease() && V("v0.6.5").patch == 5);
    CHECK(V("v10.20.30").major == 10 && V("v10.20.30").minor == 20);
    CHECK(FormatVersion(V("v0.7.0-pre2")) == "0.7.0-pre2" && FormatVersion(V("0.6.5")) == "0.6.5");
    for (const char* bad : {"", "v", "0.7", "0.7.", "0.7.0.1", "v0.7.0-pre", "v0.7.0-pre0", "v0.7.0-rc1", "v0.7.0-pre2x", "v0.7.0 ", " v0.7.0",
                            "vv0.7.0", "0.7.x", "-1.0.0", "v0.7.0-pre-2", "v0.7.0+build", "1234567.0.0", "v0..1", "V0.7.0"})
        CHECK(!ParseVersion(bad));
}

static void TestOrder() {
    CHECK(CompareVersion(V("0.7.0-pre2"), V("0.7.0-pre3")) < 0);
    CHECK(CompareVersion(V("0.7.0-pre3"), V("0.7.0")) < 0);
    CHECK(CompareVersion(V("0.7.0"), V("0.7.1-pre1")) < 0);
    CHECK(CompareVersion(V("0.7.1-pre1"), V("0.7.0")) > 0);
    CHECK(CompareVersion(V("0.6.5"), V("0.7.0-pre1")) < 0);
    CHECK(CompareVersion(V("0.9.9"), V("0.10.0")) < 0);   // numbers, not text
    CHECK(CompareVersion(V("1.0.0-pre1"), V("0.99.99")) > 0);
    CHECK(CompareVersion(V("0.7.0-pre2"), V("v0.7.0-pre2")) == 0 && CompareVersion(V("0.7.0"), V("0.7.0")) == 0);
    CHECK(CompareVersion(V("0.7.0-pre10"), V("0.7.0-pre9")) > 0);
}

static void TestDescribed() {
    CHECK(ParseDescribedVersion("v0.7.0-pre2") && ParseDescribedVersion("v0.7.0-pre2")->pre == 2);
    CHECK(ParseDescribedVersion("v0.7.0-pre2-3-gabc1234") && ParseDescribedVersion("v0.7.0-pre2-3-gabc1234")->pre == 2);
    CHECK(ParseDescribedVersion("v0.7.0-pre2-3-gabc1234-dirty") && ParseDescribedVersion("v0.7.0-pre2-3-gabc1234-dirty")->pre == 2);
    CHECK(ParseDescribedVersion("v0.7.0-12-gabc1234") && !ParseDescribedVersion("v0.7.0-12-gabc1234")->prerelease());
    CHECK(ParseDescribedVersion("v0.7.0-dirty") && ParseDescribedVersion("v0.7.0-dirty")->patch == 0);
    for (const char* bad : {"", "abc1234", "abc1234-dirty", "v0.7.0-x-gabc", "v0.7.0-3-g", "v0.7.0-3-gXYZ", "-g", "v0.7.0--gabc", "v0.7.0-pre2-gabc"})
        CHECK(!ParseDescribedVersion(bad));
}

static void TestRunning() {
    CHECK(RunningVersion("0.7.0", "")->pre == 0);
    CHECK(RunningVersion("0.7.0", "v0.7.0-pre2")->pre == 2);
    CHECK(RunningVersion("0.7.0", "v0.7.0-pre2-3-gabc1234")->pre == 2);
    CHECK(RunningVersion("0.7.0", "v0.4.0-pre1-308-gd475850")->pre == 0 && RunningVersion("0.7.0", "v0.4.0-pre1-308-gd475850")->minor == 7);   // an old tag says nothing
    CHECK(RunningVersion("0.7.0", "d475850")->pre == 0);
    CHECK(RunningVersion("0.7.0", "v0.7.0")->pre == 0);
    CHECK(!RunningVersion("", "v0.7.0-pre2") && !RunningVersion("junk", ""));
    // The release's own number (kModVersion "0.7.0-pre3"): a pre3 build is older than 0.7.0 and newer than pre2.
    CHECK(RunningVersion("0.7.0-pre3", "")->pre == 3);
    CHECK(RunningVersion("0.7.0-pre3", "v0.7.0-pre3-4-gabc1234-dirty")->pre == 3);
    CHECK(CompareVersion(*RunningVersion("0.7.0-pre3", ""), V("v0.7.0-pre2")) > 0 && CompareVersion(*RunningVersion("0.7.0-pre3", ""), V("v0.7.0")) < 0);
}

static const char kList[] = R"([
  {"url":"https://api.github.com/x/4","html_url":"https://github.com/Dudiebug/halfsword-multiplayer-mod/releases/tag/v0.7.0-pre3",
   "author":{"login":"a","html_url":"https://github.com/a"},"tag_name":"v0.7.0-pre3","draft":false,"prerelease":true,
   "body":"notes with \"quotes\", {braces} and [brackets] and \"tag_name\": \"v9.9.9\"","assets":[{"name":"dwmapi.dll","uploader":{"html_url":"https://github.com/u"}}]},
  {"html_url":"https://github.com/o/r/releases/tag/v0.7.0-pre2","tag_name":"v0.7.0-pre2","draft":false,"prerelease":true},
  {"html_url":"https://github.com/o/r/releases/tag/v0.6.5","tag_name":"v0.6.5","draft":false,"prerelease":false},
  {"html_url":"https://github.com/o/r/releases/tag/v0.6.4","tag_name":"v0.6.4","draft":false,"prerelease":false}
])";

static void TestScan() {
    const ReleaseList l = ParseReleases(kList);
    CHECK(l.complete && l.releases.size() == 4);
    CHECK(l.releases[0].tag == "v0.7.0-pre3" && l.releases[0].prerelease && !l.releases[0].draft);
    CHECK(l.releases[0].url == "https://github.com/Dudiebug/halfsword-multiplayer-mod/releases/tag/v0.7.0-pre3");   // not the author's link
    CHECK(l.releases[2].tag == "v0.6.5" && !l.releases[2].prerelease);
    CHECK(ParseReleases("[]").complete && ParseReleases("[]").releases.empty());
    CHECK(ParseReleases("  [ ]  \n").complete);
    // GitHub's error answers are objects, not lists
    CHECK(!ParseReleases(R"({"message":"API rate limit exceeded","documentation_url":"https://docs.github.com"})").complete);
    CHECK(ParseReleases(R"({"message":"Not Found"})").releases.empty());
    // an object without a tag is dropped; a value with escapes and \u is read
    const ReleaseList e = ParseReleases(R"([{"name":"x"},{"tag_name":"v1.0.0","html_url":"https:\/\/github.com\/o\/r\/releases\/tag\/v1.0.0"},{"tag_name":"v\u0031.0.1"}])");
    CHECK(e.complete && e.releases.size() == 2 && e.releases[0].url == "https://github.com/o/r/releases/tag/v1.0.0" && e.releases[1].tag == "v1.0.1");
    // true/false only mean something as the real words; a number or a string is ignored
    const ReleaseList f = ParseReleases(R"([{"tag_name":"v1.0.0","prerelease":"true","draft":1}])");
    CHECK(f.complete && f.releases.size() == 1 && !f.releases[0].prerelease && !f.releases[0].draft);
    // the same key deeper down does not count
    const ReleaseList d = ParseReleases(R"([{"x":{"tag_name":"v9.9.9","prerelease":true},"tag_name":"v1.0.0"}])");
    CHECK(d.complete && d.releases.size() == 1 && d.releases[0].tag == "v1.0.0" && !d.releases[0].prerelease);
    // a link that is not github.com is dropped, the release stays
    const ReleaseList u = ParseReleases(R"([{"tag_name":"v1.0.0","html_url":"https://evil.example/x"}])");
    CHECK(u.releases.size() == 1 && u.releases[0].url.empty());
    CHECK(!ValidReleaseUrl("https://github.com/") && !ValidReleaseUrl("http://github.com/o/r") && !ValidReleaseUrl("https://github.com/o r"));
    CHECK(ValidReleaseUrl("https://github.com/o/r/releases/tag/v1.0.0"));
}

static void TestMalformed() {
    for (const char* bad : {"", "   ", "x", "null", "\"str\"", "[", "[{", "[{\"tag_name\":\"v1.0.0\"", "[{\"tag_name\":\"v1.0.0}]", "[}]", "[{]}", "]", "[]]", "[] x",
                            "[{\"tag_name\":\"v\\q\"}]", "[{\"tag_name\":\"\\u12\"}]", "[{\"tag_name\":\"\\uZZZZ\"}]"})
        CHECK(!ParseReleases(bad).complete);
    // cut off mid-list: the releases read in full are kept
    const std::string cut(kList, std::string(kList).find("v0.6.5"));
    const ReleaseList c = ParseReleases(cut);
    CHECK(!c.complete && c.releases.size() == 2 && c.releases[1].tag == "v0.7.0-pre2");
    // deep nesting stops the scan
    CHECK(!ParseReleases("[" + std::string(40, '[') + std::string(40, ']') + "]").complete);
    CHECK(ParseReleases("[" + std::string(10, '[') + std::string(10, ']') + "]").complete);
    // the size cap
    std::string big = "[{\"tag_name\":\"v1.0.0\",\"body\":\"" + std::string(kMaxReleasesJson, 'x') + "\"}]";
    CHECK(big.size() > kMaxReleasesJson && !ParseReleases(big).complete && ParseReleases(big).releases.empty());
    std::string fits = "[{\"tag_name\":\"v1.0.0\",\"body\":\"" + std::string(kMaxReleasesJson - 100, 'x') + "\"}]";
    CHECK(fits.size() <= kMaxReleasesJson && ParseReleases(fits).complete && ParseReleases(fits).releases.size() == 1);
    // a tag or link past its limit is dropped, not truncated into something else
    const ReleaseList longTag = ParseReleases("[{\"tag_name\":\"v1.0.0" + std::string(80, '0') + "\"}]");
    CHECK(longTag.complete && longTag.releases.empty());
    // binary junk never crashes
    std::string junk;
    for (int i = 0; i < 4000; ++i) junk.push_back(char((i * 131 + 7) & 0xFF));
    ParseReleases(junk);
    ParseReleases("[" + junk + "]");
    ParseReleases("[{\"tag_name\":" + junk);
}

static void TestOffer() {
    const ReleaseList l = ParseReleases(kList);
    // on a prerelease: prereleases and full releases are offered
    auto o = NewestOffer(l.releases, V("0.7.0-pre2"));
    CHECK(o && o->tag == "v0.7.0-pre3" && o->url.find("v0.7.0-pre3") != std::string::npos);
    CHECK(!NewestOffer(l.releases, V("0.7.0-pre3")));
    // on a full release: only full releases
    CHECK(!NewestOffer(l.releases, V("0.6.5")));
    o = NewestOffer(l.releases, V("0.6.4"));
    CHECK(o && o->tag == "v0.6.5");
    // a newer build than anything listed
    CHECK(!NewestOffer(l.releases, V("0.7.0")) && !NewestOffer(l.releases, V("0.8.0-pre1")));
    // the full release outranks a later prerelease of the same number
    std::vector<ReleaseInfo> r{{"v0.7.0-pre3", "", true, false}, {"v0.7.0", "https://github.com/o/r/releases/tag/v0.7.0", false, false}};
    o = NewestOffer(r, V("0.7.0-pre2"));
    CHECK(o && o->tag == "v0.7.0");
    CHECK(!NewestOffer(r, V("0.7.0")));
    // the newest wins whatever the order, drafts never count, a prerelease tag flagged as a full release is still a prerelease
    r = {{"v0.7.2", "", false, true}, {"v0.7.1", "", false, false}, {"v0.7.3-pre1", "", false, false}, {"v0.7.1-pre9", "", true, false}};
    o = NewestOffer(r, V("0.7.0"));
    CHECK(o && o->tag == "v0.7.1");
    o = NewestOffer(r, V("0.7.0-pre1"));
    CHECK(o && o->tag == "v0.7.3-pre1");
    // a full release flagged prerelease on GitHub is held back from full-release players
    r = {{"v0.8.0", "", true, false}};
    CHECK(!NewestOffer(r, V("0.7.0")) && NewestOffer(r, V("0.7.0-pre1")));
    // unparsable tags are skipped
    r = {{"nightly", "", false, false}, {"v0.8.0-rc1", "", false, false}, {"release-0.9", "", false, false}};
    CHECK(!NewestOffer(r, V("0.7.0")) && !NewestOffer({}, V("0.7.0")));
}

int main() {
    TestParse();
    TestOrder();
    TestDescribed();
    TestRunning();
    TestScan();
    TestMalformed();
    TestOffer();
    if (g_fail) { std::printf("test_hsm_version: %d failure(s)\n", g_fail); return 1; }
    std::printf("test_hsm_version: ok\n");
    return 0;
}
