// The fighter class a spawn takes (match/hsm_fighter_class.h): the Zombies format's bot team is the game's zombie
// Willie (Willie_BP_Zombie_C, the rotten look baked in, no new fields), everything else the stock Willie_BP_C — on the
// host and on every client alike (#561), so a zombie bot looks the same on every machine.
#include "match/hsm_fighter_class.h"
#include <cstdio>
#include <cstring>
#include <initializer_list>

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++g_fail; } } while (0)
using namespace hsm;

// Criterion: only the Zombies format's bot team takes the zombie class. A human survivor (Zombies' other team), the
// human-looking bots of Survival, and every fighter of any other format all stay on the stock class.
static void TestZombiesOnlyForTheBotTeam() {
    CHECK(std::strcmp(FighterClassFor(true, true), "Willie_BP_Zombie_C") == 0);    // Zombies: the bot team (a zombie)
    CHECK(std::strcmp(FighterClassFor(true, false), "Willie_BP_C") == 0);          // Zombies: a human survivor
    CHECK(std::strcmp(FighterClassFor(false, true), "Willie_BP_C") == 0);          // Survival: its bots are human stock fighters
    CHECK(std::strcmp(FighterClassFor(false, false), "Willie_BP_C") == 0);         // any other format
}

// Criterion: an Infection player on the infected team gets the zombie body; a survivor does not.
static void TestInfectedPlayers() {
    CHECK(std::strcmp(FighterClassFor(false, false, true), "Willie_BP_Zombie_C") == 0);
    CHECK(std::strcmp(FighterClassFor(false, false, false), "Willie_BP_C") == 0);
    CHECK(IsZombieClass(FighterClassFor(false, false, true)) && !IsZombieClass(FighterClassFor(false, false, false)));
    CHECK(!IsZombieClass("Willie_BP_Zombie") && !IsZombieClass("Willie_BP_Zombie_C2") && !IsZombieClass(""));
}

// Criterion: whatever the facts, the answer is one of the game's two classes (an unknown name would never resolve).
static void TestAlwaysAKnownClass() {
    for (bool zombies : {false, true})
        for (bool botTeam : {false, true}) {
            const char* c = FighterClassFor(zombies, botTeam);
            CHECK(std::strcmp(c, "Willie_BP_Zombie_C") == 0 || std::strcmp(c, "Willie_BP_C") == 0);
        }
}

int main() {
    TestZombiesOnlyForTheBotTeam();
    TestAlwaysAKnownClass();
    TestInfectedPlayers();
    std::printf(g_fail ? "%d FAILED\n" : "all fighter class tests passed\n", g_fail);
    return g_fail ? 1 : 0;
}
