// The Loadouts row (#2112): the rule and the kept rolls (match/hsm_kept_kit.h), the config field and the lobby row.
#include "match/hsm_kept_kit.h"
#include "match/hsm_lobby_scene.h"
#include <cstdio>
#include <string>

using namespace hsm;

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++g_fail; } } while (0)

static LobbyLocal Lobby(Format f, Gear g, bool host = true) {
    LobbyLocal l;
    l.phase = LobbyPhase::InLobby; l.host = host;
    l.config = SelectFormat(l.config, f);
    l.config.gear = g;
    l.mapPick = "/Game/Maps/Arenas/Map_Arena_Pit.Map_Arena_Pit";
    LobbyPlayer me; me.name = "Me"; me.me = true; me.host = host;
    l.players.push_back(me);
    if (!host) { LobbyPlayer h; h.name = "Host"; h.host = true; l.players.push_back(h); }
    return l;
}
static bool Listed(const LobbyScene& s, LobbyRow id) { for (const SettingRow& r : s.settings) if (r.id == id) return true; return false; }

int main() {
    // In force only for a Tournament with a random loadout each, with the row on Keep.
    MatchConfig c = SelectFormat(MatchConfig{}, Format::Tournament);
    c.gear = Gear::RandomTier; c.keepKit = true;
    CHECK(KeepsKit(c));
    { MatchConfig d = c; d.keepKit = false; CHECK(!KeepsKit(d)); }
    { MatchConfig d = c; d.gear = Gear::Mirror; CHECK(!KeepsKit(d)); }
    { MatchConfig d = c; d.gear = Gear::Own; CHECK(!KeepsKit(d)); }
    { MatchConfig d = SelectFormat(c, Format::Duel); d.gear = Gear::RandomTier; d.keepKit = true; CHECK(!KeepsKit(d)); }
    CHECK(!MatchConfig{}.keepKit);

    // The kept rolls: the first roll per player is kept and given again; others roll their own; Clear starts afresh.
    {
        KeptKits k;
        int rolls = 0;
        auto roll = [&] { ++rolls; return std::vector<uint8_t>{uint8_t(rolls), 7}; };
        bool reused = true;
        const std::vector<uint8_t> a1 = k.For(3, roll, reused);
        CHECK(!reused && rolls == 1 && a1 == (std::vector<uint8_t>{1, 7}));
        const std::vector<uint8_t> a2 = k.For(3, roll, reused);
        CHECK(reused && rolls == 1 && a2 == a1);
        const std::vector<uint8_t> b1 = k.For(5, roll, reused);
        CHECK(!reused && rolls == 2 && b1 != a1 && k.size() == 2);
        CHECK(k.For(3, roll, reused) == a1 && reused && k.For(5, roll, reused) == b1 && reused && rolls == 2);
        k.Clear();
        CHECK(k.size() == 0);
        CHECK(k.For(3, roll, reused) != a1 && !reused && rolls == 3);
        // A failed roll (empty) is not kept: the next fight tries again.
        KeptKits e;
        int tries = 0;
        auto none = [&] { ++tries; return std::vector<uint8_t>{}; };
        CHECK(e.For(2, none, reused).empty() && !reused && e.size() == 0);
        CHECK(e.For(2, roll, reused).size() == 2 && !reused && e.size() == 1 && tries == 1);
    }

    // The stepper toggles it either way.
    {
        MatchConfig d = c;
        d = StepSetting(d, LobbyRow::KeepKit, 1);
        CHECK(!d.keepKit);
        d = StepSetting(d, LobbyRow::KeepKit, -1);
        CHECK(d.keepKit);
    }

    // The row: a Tournament with random loadouts, in GEAR, apart from `settings`; a member reads it greyed.
    CHECK(SectionOf(LobbyRow::KeepKit) == LobbySection::Gear);
    CHECK(size_t(LobbyRow::KeepKit) < size_t(LobbyRow::Scenario) && size_t(LobbyRow::Scenario) + 1 == size_t(LobbyRow::Sync));
    {
        LobbyLocal l = Lobby(Format::Tournament, Gear::RandomTier);
        LobbyScene s = BuildLobbyScene(l);
        CHECK(s.hasKeepKit && s.keepKit.id == LobbyRow::KeepKit && s.keepKit.label == "Loadouts" && s.keepKit.value == "New each fight");
        CHECK(s.keepKit.enabled && s.keepKit.hint.find("every fight") != std::string::npos && !Listed(s, LobbyRow::KeepKit));
        const size_t rows = s.settings.size();
        l.config.keepKit = true;
        s = BuildLobbyScene(l);
        CHECK(s.keepKit.value == "Keep for the bracket" && s.keepKit.hint.find("first fight") != std::string::npos);
        CHECK(s.settings.size() == rows);
    }
    {
        LobbyLocal l = Lobby(Format::Tournament, Gear::RandomTier, false);
        l.config.keepKit = true;
        const LobbyScene s = BuildLobbyScene(l);
        CHECK(s.hasKeepKit && s.keepKit.value == "Keep for the bracket" && !s.keepKit.enabled);
    }
    CHECK(!BuildLobbyScene(Lobby(Format::Tournament, Gear::Mirror)).hasKeepKit);
    CHECK(!BuildLobbyScene(Lobby(Format::Tournament, Gear::Own)).hasKeepKit);
    CHECK(!BuildLobbyScene(Lobby(Format::Duel, Gear::RandomTier)).hasKeepKit);
    {
        LobbyLocal l = Lobby(Format::Tournament, Gear::RandomTier);
        l.progression = true;
        CHECK(!BuildLobbyScene(l).hasKeepKit);
    }

    if (g_fail) { std::printf("%d failure(s)\n", g_fail); return 1; }
    std::printf("hsm_kept_kit: ok\n");
    return 0;
}
