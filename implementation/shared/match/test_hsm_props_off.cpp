// The Props row (#1252): what Props off clears (session/hsm_ground_clear.h PropsOffClears), the lobby row and its stepper.
#include "match/hsm_lobby_scene.h"
#include "session/hsm_ground_clear.h"
#include <cstdio>
#include <string>

using namespace hsm;

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++g_fail; } } while (0)

static LobbyLocal Lobby(Format f, bool host = true) {
    LobbyLocal l;
    l.phase = LobbyPhase::InLobby; l.host = host;
    l.config = SelectFormat(l.config, f);
    l.mapPick = "/Game/Maps/Arenas/Map_Arena_Pit.Map_Arena_Pit";
    LobbyPlayer me; me.name = "Me"; me.me = true; me.host = host;
    l.players.push_back(me);
    if (!host) { LobbyPlayer h; h.name = "Host"; h.host = true; l.players.push_back(h); }
    return l;
}
static bool Listed(const LobbyScene& s, LobbyRow id) { for (const SettingRow& r : s.settings) if (r.id == id) return true; return false; }
static PropCandidate Level(bool unmoved) { PropCandidate c; c.level = true; c.haveStart = true; c.unmoved = unmoved; return c; }

int main() {
    // A level prop goes whether it moved or not, and with no start recorded (tracked late); /clear rocks only the moved.
    for (bool unmoved : {false, true}) CHECK(PropsOffClears(Level(unmoved)));
    { PropCandidate c = Level(true); c.haveStart = false; CHECK(PropsOffClears(c)); CHECK(!ClearableProp(c)); }
    CHECK(!ClearableProp(Level(true)) && ClearableProp(Level(false)));
    // Spawned objects: the match's go, the level's own start-up spawns stay (the fence bags of Fenced ring).
    { PropCandidate c; c.spawned = true; c.bornInMatch = true; CHECK(PropsOffClears(c)); }
    { PropCandidate c; c.spawned = true; c.bornInMatch = false; CHECK(!PropsOffClears(c)); }
    // Held, denied, detached bodies and structures stay, level or spawned.
    for (int k = 0; k < 4; ++k) {
        PropCandidate a = Level(false), b; b.spawned = true; b.bornInMatch = true;
        bool* fa[] = {&a.held, &a.denied, &a.sub, &a.structure};
        bool* fb[] = {&b.held, &b.denied, &b.sub, &b.structure};
        *fa[k] = true; *fb[k] = true;
        CHECK(!PropsOffClears(a));
        CHECK(!PropsOffClears(b));
    }
    { PropCandidate c = Level(true); c.fence = true; CHECK(!PropsOffClears(c)); CHECK(!ClearableProp(c)); }
    { PropCandidate c; c.spawned = c.bornInMatch = c.fence = true; CHECK(!PropsOffClears(c)); }      // fence bags built late stay
    CHECK(!PropsOffClears(PropCandidate{}));                          // neither a level prop nor a spawned object

    // The config: On by default (lobbies run as before); the stepper toggles it.
    CHECK(!MatchConfig{}.propsOff);
    {
        MatchConfig c = SelectFormat(MatchConfig{}, Format::Duel);
        c = StepSetting(c, LobbyRow::Props, 1);
        CHECK(c.propsOff);
        c = StepSetting(c, LobbyRow::Props, -1);
        CHECK(!c.propsOff);
        CHECK(Sanitize(c).propsOff == false);
        c.propsOff = true;
        CHECK(Sanitize(c).propsOff);
    }

    // The row: every lobby fight but Progression, in WORLD, apart from `settings` (their lists stay as they were).
    CHECK(SectionOf(LobbyRow::Props) == LobbySection::World);
    CHECK(size_t(LobbyRow::Props) < size_t(LobbyRow::Scenario) && size_t(LobbyRow::Scenario) + 1 == size_t(LobbyRow::Sync));
    for (Format f : {Format::FreeForAll, Format::Duel, Format::TwoTeams, Format::Tag, Format::PlayersVsBots, Format::Zombies}) {
        LobbyLocal l = Lobby(f);
        LobbyScene on = BuildLobbyScene(l);
        CHECK(on.hasProps && on.props.id == LobbyRow::Props && on.props.label == "Props" && on.props.value == "On" && on.props.enabled);
        CHECK(on.props.hint.find("stay") != std::string::npos);
        CHECK(!Listed(on, LobbyRow::Props));
        l.config.propsOff = true;
        LobbyScene off = BuildLobbyScene(l);
        CHECK(off.props.value == "Off" && off.props.hint.find("removed") != std::string::npos);
        CHECK(off.settings.size() == on.settings.size());             // the pick changes no other row
    }
    {
        LobbyLocal l = Lobby(Format::Duel, false);                     // a member reads it, cannot step it
        l.config.propsOff = true;
        LobbyScene s = BuildLobbyScene(l);
        CHECK(s.hasProps && s.props.value == "Off" && !s.props.enabled);
    }
    {
        LobbyLocal l = Lobby(Format::Duel);
        l.progression = true;
        CHECK(!BuildLobbyScene(l).hasProps);
    }

    if (g_fail) { std::printf("%d failure(s)\n", g_fail); return 1; }
    std::printf("hsm_props_off: ok\n");
    return 0;
}
