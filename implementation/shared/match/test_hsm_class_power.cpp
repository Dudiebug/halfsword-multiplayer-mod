// The Class power row (#2125, #2144): the host's class power limit (match/hsm_class_power.h), the class checks that take it, the lobby row.
#include "match/hsm_class_power.h"
#include "match/hsm_class.h"
#include "match/hsm_online_rules.h"
#include "match/hsm_lobby_scene.h"
#include <cstdio>
#include <string>

using namespace hsm;

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++g_fail; } } while (0)

static LobbyLocal Lobby(bool budget, bool host = true) {
    LobbyLocal l;
    l.phase = LobbyPhase::InLobby; l.host = host; l.classBudget = budget;
    l.config = SelectFormat(l.config, Format::Duel);
    l.mapPick = "/Game/Maps/Arenas/Map_Arena_Pit.Map_Arena_Pit";
    LobbyPlayer me; me.name = "Me"; me.me = true; me.host = host;
    l.players.push_back(me);
    if (!host) { LobbyPlayer h; h.name = "Host"; h.host = true; l.players.push_back(h); }
    return l;
}
static bool Listed(const LobbyScene& s, LobbyRow id) { for (const SettingRow& r : s.settings) if (r.id == id) return true; return false; }
static ClassSheet Strong(double strength) { ClassSheet c; c.strength = strength; return c; }   // 0.6 power per 100% of strength

int main() {
    // The stops: +15 is the limit the mod always had, and what anything else reads as.
    CHECK(kClassPowerDefault == 15 && kClassPowerStops[0] == 15);
    CHECK(ClassPowerPoints(15) == kRuleMaxPoints);
    CHECK(ClassPowerPoints(50) == 0.50 && ClassPowerPoints(200) == 2.00);
    CHECK(ClassPowerPoints(99) == kRuleMaxPoints && ClassPowerPoints(255) == kRuleMaxPoints);         // a stale value: the usual limit
    for (int bad : {0, 14, 16, 49, 51, 255, -1}) CHECK(SnapClassPower(bad) == 15);
    for (uint8_t s : kClassPowerStops) CHECK(SnapClassPower(s) == s);
    // The stepper walks the stops and stops at either end; a stale value steps from +15.
    CHECK(StepClassPower(15, 1) == 30 && StepClassPower(30, 1) == 50 && StepClassPower(50, -1) == 30);
    CHECK(StepClassPower(15, -1) == 15 && StepClassPower(200, 1) == 200 && StepClassPower(120, 1) == 200);
    CHECK(StepClassPower(17, 1) == 30 && StepClassPower(17, -1) == 15 && StepClassPower(50, 0) == 50);

    // The checks: a +30 class is too strong at the usual limit, fits at +30 and above; Unrestricted has no limit.
    const ClassSheet c30 = Strong(1.5);
    CHECK(ClassPowerShown(ClassPoints(c30)) == 30);
    CHECK(!ClassProblems(c30).empty() && !ClassProblems(c30, false, ClassPowerPoints(15)).empty());
    CHECK(ClassProblems(c30, false, ClassPowerPoints(30)).empty() && ClassProblems(c30, false, ClassPowerPoints(50)).empty());
    CHECK(ClassProblems(c30, true, ClassPowerPoints(15)).empty());
    CHECK(!ClassProblems(Strong(1.85), false, ClassPowerPoints(50)).empty());               // +51 at +50: still over
    CHECK(!ClassProblems(Strong(9.0), false, ClassPowerPoints(200)).empty());               // the stat range still holds
    CHECK(ClassProblems(ClassSheet{}, false, ClassPowerPoints(15)).empty());
    // What the host and everyone else run on what a player sends.
    const std::string sent = EncodeClass(c30);
    CHECK(!GuestClassOnHost(sent, true, false) && GuestClassOnHost(sent, true, false, ClassPowerPoints(30)));
    CHECK(!GuestClassOnHost(sent, false, false, ClassPowerPoints(200)));                     // no classes in the lobby: none
    CHECK(!CheckClassValue(sent, "", false, true).empty() && CheckClassValue(sent, "", false, true, ClassPowerPoints(50)).empty());
    // The CLASSES tab's budget line says the lobby's limit.
    {
        const ClassBudgetView at15 = ClassBudget(c30, false), at50 = ClassBudget(c30, false, ClassPowerPoints(50));
        CHECK(at15.over && at15.text.find("OF +15") != std::string::npos && at15.text.find("OVER BY 15") != std::string::npos);
        CHECK(!at50.over && at50.text.find("OF +50") != std::string::npos && at50.text.find("20 LEFT") != std::string::npos);
    }

    // The config: +15 by default; Sanitize snaps anything else back to it; the stepper steps it.
    CHECK(MatchConfig{}.classPower == 15);
    {
        MatchConfig c = SelectFormat(MatchConfig{}, Format::Duel);
        c = StepSetting(c, LobbyRow::ClassPower, 1);
        CHECK(c.classPower == 30);
        c = StepSetting(c, LobbyRow::ClassPower, -1);
        CHECK(c.classPower == 15);
        c.classPower = 80; CHECK(Sanitize(c).classPower == 80);
        c.classPower = 81; CHECK(Sanitize(c).classPower == 15);
    }

    // The row: lobbies with a class budget only, in MATCH, apart from `settings`; a member reads it greyed.
    CHECK(SectionOf(LobbyRow::ClassPower) == LobbySection::Match);
    CHECK(size_t(LobbyRow::ClassPower) < size_t(LobbyRow::Scenario) && size_t(LobbyRow::Scenario) + 1 == size_t(LobbyRow::Sync));
    {
        LobbyLocal l = Lobby(true);
        LobbyScene s = BuildLobbyScene(l);
        CHECK(s.hasClassPower && s.classPower.id == LobbyRow::ClassPower && s.classPower.label == "Class power" && s.classPower.value == "+15");
        CHECK(s.classPower.enabled && s.classPower.hint.find("usual limit") != std::string::npos && !Listed(s, LobbyRow::ClassPower));
        const size_t rows = s.settings.size();
        l.config.classPower = 120;
        s = BuildLobbyScene(l);
        CHECK(s.classPower.value == "+120" && s.classPower.hint.find("+120") != std::string::npos && s.classPower.hint.find("removed") != std::string::npos);
        CHECK(s.settings.size() == rows);
        l.config.classPower = 99;                                                          // a stale value reads as +15
        CHECK(BuildLobbyScene(l).classPower.value == "+15");
    }
    {
        LobbyLocal l = Lobby(true, false);
        l.config.classPower = 50;
        const LobbyScene s = BuildLobbyScene(l);
        CHECK(s.hasClassPower && s.classPower.value == "+50" && !s.classPower.enabled);
    }
    CHECK(!BuildLobbyScene(Lobby(false)).hasClassPower);                                      // no classes, or Unrestricted

    if (g_fail) { std::printf("%d failure(s)\n", g_fail); return 1; }
    std::printf("hsm_class_power: ok\n");
    return 0;
}
