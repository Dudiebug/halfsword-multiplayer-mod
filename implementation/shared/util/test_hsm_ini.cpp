// Unit tests for hsm_ini.h (SetIniValue, GetIniValue). Run via implementation/shared/run_tests.sh test_hsm_ini.
#include "util/hsm_ini.h"
#include <cstdio>

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++g_fail; } } while (0)

using hsm::util::GetIniValue;
using hsm::util::SetIniValue;

static void TestReplace() {
    const std::string in = "; note\n[net]\nrole=steam\nhelp_improve=0\nname=Pl\xC3\xA4yer\n[other]\nhelp_improve=9\n";
    CHECK(SetIniValue(in, "net", "help_improve", "1") == "; note\n[net]\nrole=steam\nhelp_improve=1\nname=Pl\xC3\xA4yer\n[other]\nhelp_improve=9\n");
    CHECK(SetIniValue(in, "NET", "HELP_IMPROVE", "1").find("HELP_IMPROVE=1") != std::string::npos);   // names compare without case
    CHECK(SetIniValue("[net]\n  help_improve = 0  \n", "net", "help_improve", "1") == "[net]\nhelp_improve=1\n");
}

static void TestAddToSection() {
    const std::string in = "[net]\nrole=steam\n; trailing comment\n\n[other]\nx=1\n";
    CHECK(SetIniValue(in, "net", "help_improve", "1") == "[net]\nrole=steam\nhelp_improve=1\n; trailing comment\n\n[other]\nx=1\n");
    CHECK(SetIniValue("[net]\n[other]\nx=1\n", "net", "k", "v") == "[net]\nk=v\n[other]\nx=1\n");   // an empty section
    CHECK(SetIniValue("[net]\nrole=steam", "net", "k", "v") == "[net]\nrole=steam\nk=v\n");        // no final line break
    CHECK(SetIniValue("[other]\nk=1\n[net]\nrole=steam\n", "net", "k", "v") == "[other]\nk=1\n[net]\nrole=steam\nk=v\n");   // not another section's
}

static void TestAddSection() {
    CHECK(SetIniValue("", "net", "k", "v") == "[net]\nk=v\n");
    CHECK(SetIniValue("[other]\nx=1\n", "net", "k", "v") == "[other]\nx=1\n[net]\nk=v\n");
    CHECK(SetIniValue("[other]\nx=1", "net", "k", "v") == "[other]\nx=1\n[net]\nk=v\n");
}

static void TestLineEndings() {
    CHECK(SetIniValue("[net]\r\nrole=steam\r\nk=0\r\n", "net", "k", "1") == "[net]\r\nrole=steam\r\nk=1\r\n");
    CHECK(SetIniValue("[net]\r\nrole=steam\r\n", "net", "k", "1") == "[net]\r\nrole=steam\r\nk=1\r\n");
    CHECK(SetIniValue("[a]\r\nx=1\r\n", "net", "k", "1") == "[a]\r\nx=1\r\n[net]\r\nk=1\r\n");
}

static void TestCommentedKeyIsNotTheKey() {
    CHECK(SetIniValue("[net]\n; help_improve=1\nrole=steam\n", "net", "help_improve", "0") == "[net]\n; help_improve=1\nrole=steam\nhelp_improve=0\n");
    CHECK(SetIniValue("[net]\nk=0\nk=5\n", "net", "k", "1") == "[net]\nk=1\nk=5\n");   // the first one is the one read
}

static void TestRefusals() {
    const std::string in = "[net]\nrole=steam\n";
    CHECK(SetIniValue(in, "net", "", "1") == in);
    CHECK(SetIniValue(in, "net", "a\nb", "1") == in);
    CHECK(SetIniValue(in, "net", "a=b", "1") == in);
    CHECK(SetIniValue(in, "net", "k", "1\n[evil]\nx=1") == in);
    CHECK(SetIniValue(in, "", "k", "1") == in);
    CHECK(SetIniValue(in, "ne]t", "k", "1") == in);
}

// GetIniValue: the bytes of a value, found the way SetIniValue finds it.
static void TestGet() {
    const std::string in = "; note\n[other]\nnickname=wrong\n[net]\nrole=steam\n  Nickname =  Zo\xC3\xAB the 3rd \t\r\n; nickname=comment\nlobby_name=\nmore=1\n";
    std::string v = "keep";
    CHECK(GetIniValue(in, "net", "nickname", v) && v == "Zo\xC3\xAB the 3rd");                // UTF-8 kept, ends trimmed, case ignored
    CHECK(GetIniValue(in, "NET", "ROLE", v) && v == "steam");
    CHECK(GetIniValue(in, "net", "lobby_name", v) && v.empty());                              // present and empty
    v = "keep";
    CHECK(!GetIniValue(in, "net", "missing", v) && v == "keep");                              // not there: untouched
    CHECK(!GetIniValue(in, "nope", "role", v) && v == "keep");
    CHECK(!GetIniValue("", "net", "role", v));
    CHECK(GetIniValue(in, "other", "nickname", v) && v == "wrong");                           // each section its own
    CHECK(!GetIniValue("[net]\n; nickname=comment\n# nickname=other\n", "net", "nickname", v));   // comments are not keys
    CHECK(GetIniValue("[net]\nk=1\nk=2\n", "net", "k", v) && v == "1");                       // the first one
    CHECK(GetIniValue("[net]\r\nk=a=b\r\n", "net", "k", v) && v == "a=b");                    // only the first = splits
    CHECK(GetIniValue("[net]\nk=v", "net", "k", v) && v == "v");                              // no final line break
    CHECK(GetIniValue("\xEF\xBB\xBF[net]\nk=bom\n", "net", "k", v) && v == "bom");            // a UTF-8 byte-order mark
    CHECK(!GetIniValue("[net]\nk\n", "net", "k", v));                                          // no =: not a key
    // What SetIniValue writes is what GetIniValue reads.
    const std::string saved = SetIniValue("[net]\nrole=steam\n", "net", "nickname", "Zo\xC3\xAB");
    CHECK(GetIniValue(saved, "net", "nickname", v) && v == "Zo\xC3\xAB");
}

int main() {
    TestReplace();
    TestAddToSection();
    TestAddSection();
    TestLineEndings();
    TestCommentedKeyIsNotTheKey();
    TestRefusals();
    TestGet();
    if (g_fail) { std::printf("%d ini checks failed\n", g_fail); return 1; }
    std::printf("all ini tests passed\n");
    return 0;
}
