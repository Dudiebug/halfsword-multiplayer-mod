// Unit tests for CommandLineIgnoresHsmFlags (hsm_cmdline.h). Run via implementation/shared/run_tests.sh test_hsm_cmdline.
#include "session/hsm_cmdline.h"
#include <cstdio>

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++g_fail; } } while (0)
using hsm::CommandLineIgnoresHsmFlags;

int main() {
    CHECK(CommandLineIgnoresHsmFlags("+connect_lobby 109775241234567890"));
    CHECK(CommandLineIgnoresHsmFlags("\"C:/Games/Half Sword.exe\" -hsmrole=host +connect_lobby 1"));
    CHECK(CommandLineIgnoresHsmFlags("-hsmrole=host +connect_lobby"));       // token at the very end
    CHECK(CommandLineIgnoresHsmFlags("x +CONNECT_LOBBY 5"));                 // case
    CHECK(CommandLineIgnoresHsmFlags("a\t+connect_lobby\t5"));               // tab
    CHECK(!CommandLineIgnoresHsmFlags(""));
    CHECK(!CommandLineIgnoresHsmFlags("-hsmrole=host -hsmport=27778"));
    CHECK(!CommandLineIgnoresHsmFlags("-hsmname=+connect_lobby"));           // inside another argument
    CHECK(!CommandLineIgnoresHsmFlags("x+connect_lobby 1"));                 // no boundary before
    CHECK(!CommandLineIgnoresHsmFlags("+connect_lobbyX 1"));                 // no boundary after
    CHECK(!CommandLineIgnoresHsmFlags("+connect"));
    CHECK(!CommandLineIgnoresHsmFlags("connect_lobby"));
    if (g_fail) { std::printf("%d failure(s)\n", g_fail); return 1; }
    std::printf("test_hsm_cmdline: ok\n");
    return 0;
}
