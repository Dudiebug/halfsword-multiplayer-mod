// The lobby's Progression card (the stock hub, Free Mode off): the last mode card, after Brawl; chosen, it replaces the settings and maps.
#include "match/hsm_lobby_scene.h"
#include <cstdio>

using namespace hsm;

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++g_fail; } } while (0)

static LobbyLocal Lobby(bool host, bool progression) {
    LobbyLocal l;
    l.phase = LobbyPhase::InLobby;
    l.host = host;
    l.progression = progression;
    l.config = SelectFormat(l.config, Format::Duel);
    LobbyPlayer me; me.name = "Me"; me.me = true; me.host = host;
    l.players.push_back(me);
    return l;
}

int main() {
    LobbyScene off = BuildLobbyScene(Lobby(true, false));
    CHECK(off.modes.size() == kModeCardCount && off.modes.size() == kModeOrderCount + 5);   // the formats, Brawl, CRYPTS, Progression, Crypt Royale, Grave Robbers
    const auto prog = [](const LobbyScene& s) -> const ModeCard& { for (const ModeCard& m : s.modes) if (m.progression) return m; return s.modes.back(); };
    CHECK(prog(off).progression && !prog(off).selected && prog(off).enabled);
    CHECK(!off.settings.empty() && !off.maps.empty());
    size_t selected = 0;
    for (const ModeCard& m : off.modes) selected += m.selected;
    CHECK(selected == 1);

    LobbyScene on = BuildLobbyScene(Lobby(true, true));
    CHECK(prog(on).selected && prog(on).progression);
    selected = 0;
    for (const ModeCard& m : on.modes) selected += m.selected;
    CHECK(selected == 1);                              // the formats are all unselected
    CHECK(on.settings.size() == 4 && on.settings[0].id == LobbyRow::Players && on.settings[1].id == LobbyRow::Damage && on.settings[2].id == LobbyRow::DeadWeight &&
          on.settings[3].id == LobbyRow::WeaponWeight && on.maps.empty());   // the player limit, player damage and the weights alone
    CHECK(on.start);

    LobbyScene member = BuildLobbyScene(Lobby(false, true));
    CHECK(prog(member).selected && !prog(member).enabled);
    for (const ModeCard& m : member.modes) CHECK(!m.enabled);

    for (size_t i = 0; i < kModeOrderCount; ++i) CHECK(off.modes[i].format == kModeOrder[i].format && off.modes[i].blossfechten == kModeOrder[i].blossfechten && !off.modes[i].progression);
    if (!g_fail) std::printf("test_hsm_lobby_progression: all passed\n");
    return g_fail ? 1 : 0;
}
