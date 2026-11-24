// The Biting row: the rule (match/hsm_bite.h), the config field and the lobby row.
#include "match/hsm_bite.h"
#include "match/hsm_lobby_scene.h"
#include <cstdio>
#include <string>

using namespace hsm;

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++g_fail; } } while (0)

static LobbyLocal Lobby(bool host = true) {
    LobbyLocal l;
    l.phase = LobbyPhase::InLobby; l.host = host;
    l.config = SelectFormat(l.config, Format::Duel);
    l.mapPick = "/Game/Maps/Arenas/Map_Arena_Pit.Map_Arena_Pit";
    LobbyPlayer me; me.name = "Me"; me.me = true; me.host = host;
    l.players.push_back(me);
    if (!host) { LobbyPlayer h; h.name = "Host"; h.host = true; l.players.push_back(h); }
    return l;
}
static bool Listed(const LobbyScene& s, LobbyRow id) { for (const SettingRow& r : s.settings) if (r.id == id) return true; return false; }
static SettingRow Row(const LobbyScene& s, LobbyRow id) { for (const SettingRow& r : s.settings) if (r.id == id) return r; CHECK(false); return SettingRow{}; }

int main() {
    // In force only in a lobby fight that is not Progression, with the row Off.
    CHECK(BiteOffHere(true, false, true));
    CHECK(!BiteOffHere(false, false, true) && !BiteOffHere(true, true, true) && !BiteOffHere(true, false, false));

    // Only a player's fighter biting a fighter is stopped.
    CHECK(BlockBite(true, true, true));
    CHECK(!BlockBite(false, true, true));    // the row On: the game's bite
    CHECK(!BlockBite(true, false, true));    // a bot's bite (a zombie's) runs as usual
    CHECK(!BlockBite(true, true, false));    // food: eating still works
    CHECK(!BlockBite(true, false, false));

    // The config: On by default; the toggle flips it either way.
    CHECK(!MatchConfig{}.biteOff);
    {
        MatchConfig c = SelectFormat(MatchConfig{}, Format::Duel);
        c = StepSetting(c, LobbyRow::Bite, 1);
        CHECK(c.biteOff);
        c = StepSetting(c, LobbyRow::Bite, -1);
        CHECK(!c.biteOff);
        c = StepSetting(c, LobbyRow::Bite, -1);
        CHECK(c.biteOff);
        CHECK(!c.stripOff);                   // its own field, not Stripping's
    }

    // The row: every lobby fight but Progression, in COMBAT after Weapon weight; a member reads it greyed.
    CHECK(SectionOf(LobbyRow::Bite) == LobbySection::Combat);
    CHECK(size_t(LobbyRow::WeaponWeight) + 1 == size_t(LobbyRow::Bite) && size_t(LobbyRow::Bite) < size_t(LobbyRow::Sync));
    CHECK(lobby_detail::RowKindOf(LobbyRow::Bite) == RowKind::Toggle);
    {
        LobbyLocal l = Lobby();
        LobbyScene s = BuildLobbyScene(l);
        CHECK(Listed(s, LobbyRow::Bite) && Row(s, LobbyRow::Bite).label == "Biting" && Row(s, LobbyRow::Bite).value == "On");
        CHECK(Row(s, LobbyRow::Bite).enabled && Row(s, LobbyRow::Bite).hint.empty());
        const size_t rows = s.settings.size();
        l.config.biteOff = true;
        s = BuildLobbyScene(l);
        CHECK(Row(s, LobbyRow::Bite).value == "Off" && Row(s, LobbyRow::Bite).hint == "Players can't bite fighters; eating still works.");
        CHECK(s.settings.size() == rows);
        CHECK(Row(s, LobbyRow::Strip).value == "On");
    }
    {
        LobbyLocal l = Lobby(false);
        l.config.biteOff = true;
        const LobbyScene s = BuildLobbyScene(l);
        CHECK(Listed(s, LobbyRow::Bite) && Row(s, LobbyRow::Bite).value == "Off" && !Row(s, LobbyRow::Bite).enabled);
    }
    {
        LobbyLocal l = Lobby();
        l.progression = true;
        CHECK(!Listed(BuildLobbyScene(l), LobbyRow::Bite));
    }

    if (g_fail) { std::printf("%d failure(s)\n", g_fail); return 1; }
    std::printf("hsm_bite: ok\n");
    return 0;
}
