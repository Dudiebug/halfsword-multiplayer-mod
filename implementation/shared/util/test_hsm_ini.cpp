// Unit tests for hsm_ini.h (SetIniValue). Run via implementation/shared/run_tests.sh test_hsm_ini.
#include "util/hsm_ini.h"
#include <cstdio>

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++g_fail; } } while (0)

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

int main() {
    TestReplace();
    TestAddToSection();
    TestAddSection();
    TestLineEndings();
    TestCommentedKeyIsNotTheKey();
    TestRefusals();
    if (g_fail) { std::printf("%d ini checks failed\n", g_fail); return 1; }
    std::printf("all ini tests passed\n");
    return 0;
}
