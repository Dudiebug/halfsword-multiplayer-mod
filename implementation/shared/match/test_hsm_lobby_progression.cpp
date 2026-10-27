// The lobby's Progression card (the stock hub, Free Mode off): a 7th mode card; chosen, it replaces the settings and maps.
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
    CHECK(off.modes.size() == kModeCardCount && off.modes.size() == size_t(kFormatCount) + 1);
    CHECK(off.modes.back().progression && !off.modes.back().selected && off.modes.back().enabled);
    CHECK(!off.settings.empty() && !off.maps.empty());
    size_t selected = 0;
    for (const ModeCard& m : off.modes) selected += m.selected;
    CHECK(selected == 1);

    LobbyScene on = BuildLobbyScene(Lobby(true, true));
    CHECK(on.modes.back().selected && on.modes.back().progression);
    selected = 0;
    for (const ModeCard& m : on.modes) selected += m.selected;
    CHECK(selected == 1);                              // the formats are all unselected
    CHECK(on.settings.empty() && on.maps.empty());
    CHECK(on.start);

    LobbyScene member = BuildLobbyScene(Lobby(false, true));
    CHECK(member.modes.back().selected && !member.modes.back().enabled);
    for (const ModeCard& m : member.modes) CHECK(!m.enabled);

    for (size_t i = 0; i < size_t(kFormatCount); ++i) CHECK(off.modes[i].format == kModeOrder[i] && !off.modes[i].progression);
    if (!g_fail) std::printf("test_hsm_lobby_progression: all passed\n");
    return g_fail ? 1 : 0;
}
