// The Stripping row (#1931): the rule (match/hsm_strip.h), the config field and the lobby row.
#include "match/hsm_strip.h"
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
static SettingRow Row(const LobbyScene& s, LobbyRow id) { for (const SettingRow& r : s.settings) if (r.id == id) return r; CHECK(false); return SettingRow{}; }   // a copy: the scene is often a temporary

int main() {
    // In force only in a lobby fight that is not Progression, with the row Off.
    CHECK(StripOffHere(true, false, true));
    CHECK(!StripOffHere(false, false, true) && !StripOffHere(true, true, true) && !StripOffHere(true, false, false));

    // Only a strip's own Remove Armor on the fighter being stripped is blocked.
    int downed = 0, other = 0;
    CHECK(BlockStripArmor(true, &downed, &downed));
    CHECK(!BlockStripArmor(false, &downed, &downed));     // the row On: the game's strip
    CHECK(!BlockStripArmor(true, nullptr, &downed));      // no strip running: a pick-up swap, the fighter's own
    CHECK(!BlockStripArmor(true, &downed, &other));       // during a strip, another fighter's armour
    CHECK(!BlockStripArmor(true, nullptr, nullptr));

    // The config: On by default; the stepper toggles it either way.
    CHECK(!MatchConfig{}.stripOff);
    {
        MatchConfig c = SelectFormat(MatchConfig{}, Format::Duel);
        c = StepSetting(c, LobbyRow::Strip, 1);
        CHECK(c.stripOff);
        c = StepSetting(c, LobbyRow::Strip, -1);
        CHECK(!c.stripOff);
        c = StepSetting(c, LobbyRow::Strip, -1);
        CHECK(c.stripOff);
    }

    // The row: every lobby fight but Progression, in GEAR, apart from `settings`; a member reads it greyed.
    CHECK(SectionOf(LobbyRow::Strip) == LobbySection::Gear);
    CHECK(size_t(LobbyRow::Strip) < size_t(LobbyRow::Sync));
    {
        LobbyLocal l = Lobby();
        LobbyScene s = BuildLobbyScene(l);
        CHECK(Listed(s, LobbyRow::Strip) && Row(s, LobbyRow::Strip).id == LobbyRow::Strip && Row(s, LobbyRow::Strip).label == "Stripping" && Row(s, LobbyRow::Strip).value == "On");
        CHECK(Row(s, LobbyRow::Strip).enabled && Row(s, LobbyRow::Strip).hint.empty());
        const size_t rows = s.settings.size();
        l.config.stripOff = true;
        s = BuildLobbyScene(l);
        CHECK(Row(s, LobbyRow::Strip).value == "Off" && Row(s, LobbyRow::Strip).hint.empty());
        CHECK(s.settings.size() == rows);
    }
    {
        LobbyLocal l = Lobby(false);
        l.config.stripOff = true;
        const LobbyScene s = BuildLobbyScene(l);
        CHECK(Listed(s, LobbyRow::Strip) && Row(s, LobbyRow::Strip).value == "Off" && !Row(s, LobbyRow::Strip).enabled);
    }
    {
        LobbyLocal l = Lobby();
        l.progression = true;
        CHECK(!Listed(BuildLobbyScene(l), LobbyRow::Strip));
    }

    if (g_fail) { std::printf("%d failure(s)\n", g_fail); return 1; }
    std::printf("hsm_strip: ok\n");
    return 0;
}
