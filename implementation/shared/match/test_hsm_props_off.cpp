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
static SettingRow Row(const LobbyScene& s, LobbyRow id) { for (const SettingRow& r : s.settings) if (r.id == id) return r; CHECK(false); return SettingRow{}; }   // a copy: the scene is often a temporary
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
    for (int k = 0; k < 5; ++k) {
        PropCandidate a = Level(false), b; b.spawned = true; b.bornInMatch = true;
        bool* fa[] = {&a.held, &a.denied, &a.sub, &a.structure, &a.worn};
        bool* fb[] = {&b.held, &b.denied, &b.sub, &b.structure, &b.worn};
        *fa[k] = true; *fb[k] = true;
        CHECK(!PropsOffClears(a));
        CHECK(!PropsOffClears(b));
        CHECK(!ClearableProp(a) && !ClearableProp(b));               // /clear rocks spares them too
    }
    // #3909: the bolt quiver a fighter wears is a spawned object (Attach Empty Quiver) born in the match; worn, it stays, and
    // dropped (Drop Attached Quiver clears MyWillie) it is a loose spawned object again.
    { PropCandidate q; q.spawned = q.bornInMatch = true; q.worn = true; CHECK(!PropsOffClears(q)); q.worn = false; CHECK(PropsOffClears(q)); }
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

    // The row: every lobby fight but Progression, in WORLD.
    CHECK(SectionOf(LobbyRow::Props) == LobbySection::World);
    CHECK(size_t(LobbyRow::Props) < size_t(LobbyRow::Sync));
    for (Format f : {Format::FreeForAll, Format::Duel, Format::TwoTeams, Format::Tag, Format::PlayersVsBots, Format::Zombies}) {
        LobbyLocal l = Lobby(f);
        LobbyScene on = BuildLobbyScene(l);
        CHECK(Listed(on, LobbyRow::Props) && Row(on, LobbyRow::Props).id == LobbyRow::Props && Row(on, LobbyRow::Props).label == "Props" && Row(on, LobbyRow::Props).value == "On" && Row(on, LobbyRow::Props).enabled);
        CHECK(Row(on, LobbyRow::Props).hint.empty());
        l.config.propsOff = true;
        LobbyScene off = BuildLobbyScene(l);
        CHECK(Row(off, LobbyRow::Props).value == "Off" && Row(off, LobbyRow::Props).hint.empty());
        CHECK(off.settings.size() == on.settings.size());             // the pick changes no other row
    }
    {
        LobbyLocal l = Lobby(Format::Duel, false);                     // a member reads it, cannot step it
        l.config.propsOff = true;
        LobbyScene s = BuildLobbyScene(l);
        CHECK(Listed(s, LobbyRow::Props) && Row(s, LobbyRow::Props).value == "Off" && !Row(s, LobbyRow::Props).enabled);
    }
    {
        LobbyLocal l = Lobby(Format::Duel);
        l.progression = true;
        CHECK(!Listed(BuildLobbyScene(l), LobbyRow::Props));
    }

    if (g_fail) { std::printf("%d failure(s)\n", g_fail); return 1; }
    std::printf("hsm_props_off: ok\n");
    return 0;
}
