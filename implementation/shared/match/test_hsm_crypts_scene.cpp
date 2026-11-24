// Unit tests for the Crypts map's two modes in the lobby (Crypt Royale, Grave Robbers; #3199, hsm_lobby_scene.h). Run via implementation/shared/run_tests.sh test_hsm_crypts_scene.
#include "match/hsm_lobby_scene.h"
#include "match/hsm_match_impl.h"
#include <algorithm>
#include <cstdio>
#include <string>

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++g_fail; } } while (0)

using namespace hsm;

static LobbyPlayer Player(const char* name, bool host, bool me) { LobbyPlayer p; p.name = name; p.host = host; p.me = me; return p; }
static LobbyLocal InLobby(bool host, MatchConfig c, int players = 2) {
    LobbyLocal l;
    l.phase = LobbyPhase::InLobby; l.host = host; l.version = "0.2.0"; l.myName = host ? "Ann" : "Bob";
    l.config = c;
    l.players = {Player("Ann", true, host), Player("Bob", false, !host)};
    l.playerCount = players;
    return l;
}
static MatchConfig Crypts(Format battle = Format::LastManStanding) {
    MatchConfig c = SelectMode(MatchConfig{}, Format::FreeForAll, false, true);
    for (size_t i = 0; i < kCryptsBattleCount && c.format != battle; ++i) c = StepSetting(c, LobbyRow::Battle, 1);
    if (c.format != battle) c.format = battle;                      // Duel is no stop of the Battle row (0.9.0: the Last man standing one); an older host's lobby can still hold it
    return c;
}
// The format's own rows only: the scene also lists the rows every lobby has (the player limit, Props, Strip, ...), which SettingsFor does not.
static std::vector<LobbyRow> FormatIds(const LobbyScene& s) {
    std::vector<LobbyRow> v;
    for (const SettingRow& r : s.settings) {
        switch (r.id) {
            case LobbyRow::Players: case LobbyRow::WinBy: case LobbyRow::Strip: case LobbyRow::Bite: case LobbyRow::KeepKit: case LobbyRow::TimeOfDay:
            case LobbyRow::Props: case LobbyRow::ClassPower: break;
            default: v.push_back(r.id);
        }
    }
    return v;
}
static size_t MapIndex(const char* display) {
    for (size_t i = 0; i < MapCount(); ++i) if (std::string(kMaps[i].display) == display) return i;
    return MapCount();
}
static const SettingRow* RowOf(const LobbyScene& s, LobbyRow id) { for (const SettingRow& r : s.settings) if (r.id == id) return &r; return nullptr; }
static const ModeCard* NamedCard(const LobbyScene& s, const char* name) { for (const ModeCard& m : s.modes) if (m.name == name) return &m; return nullptr; }
static const ModeCard* CryptsCard(const LobbyScene& s) { return NamedCard(s, "CRYPTS"); }          // the entry card: picks the Crypts map
static const ModeCard* RoyaleCard(const LobbyScene& s) { return NamedCard(s, "CRYPT ROYALE"); }
static const ModeCard* GraveCard(const LobbyScene& s) { return NamedCard(s, "GRAVE ROBBERS"); }
// The flag ([mod] crypts_coop, on by default) that shows Grave Robbers and the Crypts run's Together; =0 hides them.
struct CoopFlag { explicit CoopFlag(bool on) { g_cryptsCoopOffered = on; } ~CoopFlag() { g_cryptsCoopOffered = false; } };
static bool Has(const std::vector<LobbyRow>& v, LobbyRow r) { return std::find(v.begin(), v.end(), r) != v.end(); }
static LobbyLocal noCryptsOf(LobbyLocal l) { l.config.gear = Gear::Own; l.config.cryptsTier = 5; return l; }

// The cards: CRYPTS after Brawl (a versus entry card that picks the Crypts map), Progression, then Crypt Royale and Grave Robbers
// (the two modes of the Crypts map, shown on it alone). Picking one lights it alone.
static void TestCard() {
    LobbyScene own = BuildLobbyScene(InLobby(true, MatchConfig{}));
    CHECK(own.modes.size() == kModeCardCount);
    const ModeCard& k = own.modes[kModeOrderCount + 1];
    CHECK(k.crypts && !k.graveRobbers && !k.brawl && !k.progression && k.name == "CRYPTS" && !k.selected && k.enabled && k.shown && k.group == ModeGroup::Versus);
    CHECK(k.description == kCryptsBlurb && k.format == kCryptsDefaultBattle);
    CHECK(own.modes[kModeOrderCount].brawl && own.modes[kModeOrderCount + 2].progression && own.modes[kModeOrderCount + 2].shown);
    CHECK(RoyaleCard(own) && !RoyaleCard(own)->shown && GraveCard(own) && !GraveCard(own)->shown);   // the Crypts map is not picked: only the entry card shows
    LobbyScene s = BuildLobbyScene(InLobby(true, Crypts()));
    const ModeCard* c = RoyaleCard(s);
    CHECK(c && c->selected && c->enabled && c->shown && c->crypts && !c->graveRobbers && c->group == ModeGroup::Coop && c->description == kCryptRoyaleBlurb);
    CHECK(CryptsCard(s) && !CryptsCard(s)->shown && !CryptsCard(s)->selected);              // on the Crypts map the entry card gives way
    CHECK(GraveCard(s) && !GraveCard(s)->shown && !GraveCard(s)->selected);                 // held back without the dev flag
    int lit = 0; for (const ModeCard& m : s.modes) lit += m.selected;
    CHECK(lit == 1);                                                                         // the Last man standing card is not lit too
    LobbyScene member = BuildLobbyScene(InLobby(false, Crypts()));
    CHECK(RoyaleCard(member) && RoyaleCard(member)->selected && !RoyaleCard(member)->enabled);
    LobbyLocal prog = InLobby(true, Crypts()); prog.progression = true;
    CHECK(!RoyaleCard(BuildLobbyScene(prog))->selected && !RoyaleCard(BuildLobbyScene(prog))->shown && CryptsCard(BuildLobbyScene(prog))->shown);   // the Progression card wins
    LobbyScene bots = BuildLobbyScene(InLobby(true, Crypts(Format::PlayersVsBots)));
    CHECK(RoyaleCard(bots)->format == Format::PlayersVsBots && RoyaleCard(bots)->group == ModeGroup::Coop);
    {
        CoopFlag on(true);
        LobbyScene g = BuildLobbyScene(InLobby(true, Crypts()));
        CHECK(GraveCard(g)->shown && !GraveCard(g)->selected && GraveCard(g)->group == ModeGroup::Coop && GraveCard(g)->description == kGraveRobbersBlurb);
        MatchConfig gr = SelectMode(Crypts(), Format::FreeForAll, false, true, false, true);
        LobbyScene gs = BuildLobbyScene(InLobby(true, gr));
        CHECK(GraveCard(gs)->selected && !RoyaleCard(gs)->selected && GraveCard(gs)->shown && RoyaleCard(gs)->shown);
        lit = 0; for (const ModeCard& m : gs.modes) lit += m.selected;
        CHECK(lit == 1);
    }
}

// Picking and leaving the card.
static void TestSelect() {
    MatchConfig c; c.format = Format::TwoTeams; c.gear = Gear::Mirror; c.tier = 6; c.scoreLimit = 30; c.timeLimitS = 600; c.gatherS = 1200;
    MatchConfig k = SelectMode(c, Format::TwoTeams, false, true);
    CHECK(IsCrypts(k) && k.format == kCryptsDefaultBattle && k.scoreLimit == DefaultScoreLimit(kCryptsDefaultBattle) && k.tier == MatchConfig{}.tier);
    CHECK(k.timeLimitS == 600 && k.gatherS == 1200);                                    // the other settings stay
    CHECK(Sanitize(k).gear == Gear::Crypts);
    k.scoreLimit = 7; CHECK(SelectMode(k, Format::FreeForAll, false, true).scoreLimit == 7);   // the card again changes nothing
    CHECK(!k.graveRobbers && IsCryptRoyale(k) && !IsGraveRobbers(k));                   // the entry card is Crypt Royale
    MatchConfig same = SelectMode(k, Format::LastManStanding, false);                     // the battle's own card: the same format, own loadouts
    CHECK(!IsCrypts(same) && same.format == Format::LastManStanding && same.gear == Gear::Own && same.scoreLimit == 7);
    MatchConfig duel = SelectMode(k, Format::Duel, false);                                // the Duel card is the Last man standing one
    CHECK(duel.format == Format::LastManStanding && duel.gear == Gear::Own && duel.scoreLimit == 7);   // the same card: the settings stay
    MatchConfig brawl = SelectMode(k, Format::FreeForAll, true);
    CHECK(IsBrawl(brawl) && brawl.gear == Gear::Fists);
    MatchConfig fromBrawl = SelectMode(brawl, Format::FreeForAll, false, true);
    CHECK(IsCrypts(fromBrawl) && fromBrawl.tier == MatchConfig{}.tier && fromBrawl.format == kCryptsDefaultBattle);
    MatchConfig tag = SelectFormat(MatchConfig{}, Format::Tag);
    MatchConfig fromTag = SelectMode(tag, Format::FreeForAll, false, true);
    CHECK(IsCrypts(fromTag) && fromTag.damagePct == kDamageStock);                     // Tag's light hits stay in Tag
    // The Gear row never lands on Crypts.
    MatchConfig g; for (int i = 0; i < 20; ++i) { g = StepSetting(g, LobbyRow::Gear, i % 3 ? 1 : -1); CHECK(!IsCrypts(g)); }
    // Grave Robbers is the other mode of the same map; leaving the map's modes drops its flags.
    MatchConfig gr = SelectMode(k, Format::FreeForAll, false, true, false, true);
    CHECK(IsCrypts(gr) && IsGraveRobbers(gr) && !IsCryptRoyale(gr) && gr.graveRobbers && !gr.cryptsTogether && gr.gatherS == k.gatherS);
    gr.cryptsTogether = true; CHECK(!SelectMode(gr, Format::FreeForAll, false, true, false, true).cryptsTogether);
    MatchConfig back = SelectMode(gr, Format::FreeForAll, false, true);                   // back to Crypt Royale on the same map
    CHECK(IsCryptRoyale(back) && !back.graveRobbers && back.gatherS == gr.gatherS);
    MatchConfig left = SelectMode(gr, Format::LastManStanding, false);
    CHECK(!IsCrypts(left) && !left.graveRobbers && !left.cryptsTogether && left.gear == Gear::Own);
    CHECK(!IsGraveRobbers(MatchConfig{}) && !IsCryptRoyale(MatchConfig{}) && !SharedCryptsRun(MatchConfig{}) && !SharedCryptsRun(k));
    // The map picks the mode: the Crypts map from another mode is Crypt Royale, another map from the Crypts is that map's own mode.
    MatchConfig fromLms; fromLms.format = Format::LastManStanding;
    CHECK(IsCryptRoyale(MapPicked(fromLms, true)) && MapPicked(fromLms, false).format == Format::LastManStanding && !IsCrypts(MapPicked(fromLms, false)));
    CHECK(IsCrypts(MapPicked(k, true)) && MapPicked(k, true).format == k.format && MapPicked(k, true).gatherS == k.gatherS);   // the Crypts map again: unchanged
    CHECK(!IsCrypts(MapPicked(k, false)) && MapPicked(k, false).gear == Gear::Own && MapPicked(k, false).format == k.format);
    CHECK(!IsCrypts(MapPicked(gr, false)) && !MapPicked(gr, false).graveRobbers && IsGraveRobbers(MapPicked(gr, true)));   // Grave Robbers stays on its map
}

// The Battle row steps through the battle formats and keeps the gear; the Gather time through 5..30 minutes.
static void TestRows() {
    MatchConfig c = Crypts();
    std::vector<Format> seen;
    for (size_t i = 0; i < kCryptsBattleCount; ++i) { seen.push_back(c.format); CHECK(IsCrypts(c) && CryptsBattleFormat(c.format)); c = StepSetting(c, LobbyRow::Battle, 1); }
    CHECK(c.format == kCryptsDefaultBattle && seen.size() == kCryptsBattleCount);       // wraps
    for (size_t i = 0; i < kCryptsBattleCount; ++i) CHECK(seen[i] == kCryptsBattles[i]);
    CHECK(StepSetting(c, LobbyRow::Battle, -1).format == Format::PlayersVsBots);          // back wraps to the last
    CHECK(StepSetting(StepSetting(c, LobbyRow::Battle, 1), LobbyRow::Battle, -1).format == c.format);
    for (Format b : kCryptsBattles) CHECK(GearAllowed(b, Gear::Crypts) && Sanitize(Crypts(b)).gear == Gear::Crypts);
    MatchConfig three = c; three.format = Format::ThreeTeams; CHECK(StepSetting(three, LobbyRow::Battle, 1).format == Format::FreeForAll);   // a stale value steps from the first
    MatchConfig t = StepSetting(c, LobbyRow::Battle, 1);
    CHECK(t.scoreLimit == DefaultScoreLimit(t.format));                                  // a new battle starts at its own limit
    // Gather time.
    MatchConfig g = Crypts(); CHECK(g.gatherS == 600);
    g = StepSetting(g, LobbyRow::Gather, 1); CHECK(g.gatherS == 900);
    g = StepSetting(g, LobbyRow::Gather, -1); g = StepSetting(g, LobbyRow::Gather, -1); CHECK(g.gatherS == 300);
    g = StepSetting(g, LobbyRow::Gather, -1); CHECK(g.gatherS == 1800);                  // wraps
    g = StepSetting(g, LobbyRow::Gather, 1); CHECK(g.gatherS == 300);
    g.gatherS = 3600; CHECK(StepSetting(g, LobbyRow::Gather, 1).gatherS == 300 && StepSetting(g, LobbyRow::Gather, -1).gatherS == 1800);
    g.gatherS = 720; CHECK(StepSetting(g, LobbyRow::Gather, 1).gatherS == 900 && StepSetting(g, LobbyRow::Gather, -1).gatherS == 600);   // a typed 12 min
    LobbyScene s = BuildLobbyScene(InLobby(true, g));
    const SettingRow* gather = RowOf(s, LobbyRow::Gather);
    const SettingRow* battle = RowOf(s, LobbyRow::Battle);
    CHECK(gather && gather->label == "Gather time" && gather->value == "12 min" && gather->enabled && gather->hint.empty());
    CHECK(battle && battle->label == "Battle" && battle->value == "Duel" && battle->enabled);   // two players: the merged card reads Duel
    CHECK(RowOf(BuildLobbyScene(InLobby(true, g, 3)), LobbyRow::Battle)->value == "Last man standing");
    CHECK(SectionOf(LobbyRow::Battle) == LobbySection::Match && SectionOf(LobbyRow::Gather) == LobbySection::Match && SectionOf(LobbyRow::CryptsTier) == LobbySection::Match);
    CHECK(SectionOf(LobbyRow::CryptsRun) == LobbySection::Match);
    CHECK(size_t(LobbyRow::Players) + 1 == size_t(LobbyRow::Battle) && size_t(LobbyRow::Battle) + 1 == size_t(LobbyRow::Gather) && size_t(LobbyRow::Gather) + 1 == size_t(LobbyRow::CryptsRun) &&
          size_t(LobbyRow::CryptsRun) + 1 == size_t(LobbyRow::CryptsTier) && size_t(LobbyRow::CryptsTier) + 1 == size_t(LobbyRow::FightMap) &&
          size_t(LobbyRow::FightMap) + 1 == size_t(LobbyRow::Limit));
    CoopFlag coop(true);                                                                // the Crypts run row is shown from here (the dev flag)
    // Crypts run: Apart by default; either arrow flips it; nothing else moves.
    MatchConfig r = Crypts(); CHECK(!r.cryptsTogether && !MatchConfig{}.cryptsTogether);
    r = StepSetting(r, LobbyRow::CryptsRun, 1); CHECK(r.cryptsTogether);
    CHECK(!StepSetting(r, LobbyRow::CryptsRun, 1).cryptsTogether && !StepSetting(r, LobbyRow::CryptsRun, -1).cryptsTogether);
    CHECK(StepSetting(r, LobbyRow::CryptsRun, 1).gatherS == r.gatherS && StepSetting(r, LobbyRow::CryptsRun, 1).format == r.format && StepSetting(r, LobbyRow::CryptsRun, 1).cryptsTier == r.cryptsTier);
    CHECK(Sanitize(r).cryptsTogether);                                                  // kept on the Crypts map with the flag
    { g_cryptsCoopOffered = false; CHECK(!Sanitize(r).cryptsTogether); g_cryptsCoopOffered = true; }   // a release: never Together
    { MatchConfig off = r; off.gear = Gear::Own; CHECK(!Sanitize(off).cryptsTogether); }   // any other card: Apart
    LobbyScene rs = BuildLobbyScene(InLobby(true, r));
    const SettingRow* run = RowOf(rs, LobbyRow::CryptsRun);
    CHECK(run && run->label == "Crypts run" && run->value == "Together" && run->enabled && run->hint == "Everyone loots the same crypt.");
    CHECK(RowOf(rs, LobbyRow::CryptsTier));                                             // Together keeps the Dungeon tier
    CHECK(rs.summary.find(", together") != std::string::npos);
    const SettingRow* gt = RowOf(rs, LobbyRow::Gather);
    CHECK(gt && gt->hint.empty());
    r.cryptsTogether = false; rs = BuildLobbyScene(InLobby(true, r)); run = RowOf(rs, LobbyRow::CryptsRun);
    CHECK(run && run->value == "Apart" && run->hint == "Each player loots their own Crypts." && RowOf(rs, LobbyRow::CryptsTier));
    CHECK(rs.summary.find("together") == std::string::npos);
    r.cryptsTogether = true; LobbyScene rm = BuildLobbyScene(InLobby(false, r));
    CHECK(RowOf(rm, LobbyRow::CryptsRun) && !RowOf(rm, LobbyRow::CryptsRun)->enabled);   // members read it
    // Dungeon tier (#3350): Peasant by default, Beggar .. Knight, then the game's own; wraps both ways.
    MatchConfig k = Crypts(); CHECK(k.cryptsTier == kCryptsTierCap && MatchConfig{}.cryptsTier == kCryptsTierCap);
    k = StepSetting(k, LobbyRow::CryptsTier, 1); CHECK(k.cryptsTier == 2);
    k = StepSetting(k, LobbyRow::CryptsTier, -1); k = StepSetting(k, LobbyRow::CryptsTier, -1); CHECK(k.cryptsTier == 0);
    k = StepSetting(k, LobbyRow::CryptsTier, -1); CHECK(k.cryptsTier == kCryptsTierTop);                       // wraps to the game's own
    k = StepSetting(k, LobbyRow::CryptsTier, 1); CHECK(k.cryptsTier == 0);
    k.cryptsTier = 200; CHECK(StepSetting(k, LobbyRow::CryptsTier, 1).cryptsTier == 0 && StepSetting(k, LobbyRow::CryptsTier, -1).cryptsTier == 7);   // a stale value steps from the top
    CHECK(StepSetting(k, LobbyRow::CryptsTier, 1).gatherS == k.gatherS && StepSetting(k, LobbyRow::CryptsTier, 1).format == k.format);
    k.cryptsTier = 3; LobbyScene ks = BuildLobbyScene(InLobby(true, k));
    const SettingRow* tier = RowOf(ks, LobbyRow::CryptsTier);
    CHECK(tier && tier->label == "Dungeon tier" && tier->value == "Militia" && tier->enabled && tier->hint.empty());
    k.cryptsTier = kCryptsTierTop; ks = BuildLobbyScene(InLobby(true, k)); tier = RowOf(ks, LobbyRow::CryptsTier);
    CHECK(tier && tier->value == "Game's own" && tier->hint.empty());
    CHECK(CryptsTierLabel(0) == "Beggar" && CryptsTierLabel(1) == "Peasant" && CryptsTierLabel(7) == "Knight" && CryptsTierLabel(8) == "Game's own" && CryptsTierLabel(255) == "Game's own");
    LobbyScene km = BuildLobbyScene(InLobby(false, k));
    CHECK(RowOf(km, LobbyRow::CryptsTier) && !RowOf(km, LobbyRow::CryptsTier)->enabled);                       // members read it
    MatchConfig bad = k; bad.cryptsTier = 9; CHECK(Sanitize(bad).cryptsTier == kCryptsTierTop);
    bad.cryptsTier = 0; CHECK(Sanitize(bad).cryptsTier == 0);
    bad.cryptsTier = 5; CHECK(Sanitize(bad).cryptsTier == 5);
    LobbyScene m = BuildLobbyScene(InLobby(false, g));
    CHECK(RowOf(m, LobbyRow::Gather) && !RowOf(m, LobbyRow::Gather)->enabled);          // members read it
}

// The rows: Battle and Gather time first, then the Crypts run (the dev flag), the Dungeon tier and the Fight map, then the battle's
// own rows without Gear (the gathered kits are the gear), and the new character's Weapons after its tier and Bodies.
static void TestSettings() {
    for (bool flag : {false, true}) {
        CoopFlag f(flag);
        for (Format b : kCryptsBattles) {
            MatchConfig c = Crypts(b);
            const std::vector<LobbyRow> v = SettingsFor(c);
            std::vector<LobbyRow> head = {LobbyRow::Battle, LobbyRow::Gather};
            if (flag) head.push_back(LobbyRow::CryptsRun);
            head.push_back(LobbyRow::CryptsTier); head.push_back(LobbyRow::FightMap);
            CHECK(v.size() >= 5 && std::equal(head.begin(), head.end(), v.begin()) && v.back() == LobbyRow::Sync);
            CHECK(!Has(v, LobbyRow::Gear) && Has(v, LobbyRow::Weapons) && Has(v, LobbyRow::Tier) && Has(v, LobbyRow::Bodies));
            CHECK(Has(v, LobbyRow::CryptsRun) == flag);
            MatchConfig own = c; own.gear = Gear::Own;
            std::vector<LobbyRow> want = head;
            for (LobbyRow r : SettingsFor(own)) {
                want.push_back(r == LobbyRow::Gear ? LobbyRow::Tier : r);                 // the Crypts card has the Tier row where the others have Gear
                if (r == LobbyRow::Bodies) want.push_back(LobbyRow::Weapons);
            }
            CHECK(v == want);
            LobbyScene s = BuildLobbyScene(InLobby(true, c, b == Format::Duel ? 2 : 4));
            CHECK(FormatIds(s) == v);
            CHECK(RowOf(s, LobbyRow::Players) && RowOf(s, LobbyRow::Props) && RowOf(s, LobbyRow::Strip));
            MatchConfig t = c; t.cryptsTogether = true;                                  // Together: the same rows
            CHECK(SettingsFor(t) == want);
        }
    }
    for (Format f : {Format::LastManStanding, Format::Tag, Format::Duel}) {             // no other card shows them
        MatchConfig c; c.format = f;
        const std::vector<LobbyRow> v = SettingsFor(Sanitize(c));
        CHECK(!Has(v, LobbyRow::Battle) && !Has(v, LobbyRow::Gather) && !Has(v, LobbyRow::CryptsTier) && !Has(v, LobbyRow::CryptsRun) && !Has(v, LobbyRow::FightMap));
    }
    CHECK(Has(SettingsFor(Crypts(Format::TwoTeams)), LobbyRow::Teams) && Has(SettingsFor(Crypts(Format::PlayersVsBots)), LobbyRow::WaveBreak));
    // The Tier row is the new character's rank, with its budget; Weapons and Bodies speak of that character.
    MatchConfig c = Crypts(); c.tier = 3;
    LobbyScene s = BuildLobbyScene(InLobby(true, c));
    const SettingRow* tier = RowOf(s, LobbyRow::Tier);
    CHECK(tier && tier->label == "Starting tier" && tier->value == TierName(3));
    CHECK(tier && tier->hint.empty());
    const SettingRow* weapons = RowOf(s, LobbyRow::Weapons);
    CHECK(weapons && weapons->hint.empty() && weapons->chips.size() == size_t(kWeaponTypeCount) + 1);
    const SettingRow* bodies = RowOf(s, LobbyRow::Bodies);
    CHECK(bodies && bodies->hint == "Strength from the tier.");
    MatchConfig o; o.format = Format::LastManStanding;                                     // the other cards keep their words
    const LobbyScene os = BuildLobbyScene(InLobby(true, Sanitize(o)));
    const SettingRow* gear = RowOf(os, LobbyRow::Gear);                                    // no Tier row off the Crypts card: the tier rides on Gear
    CHECK(!RowOf(os, LobbyRow::Tier) && gear && gear->tierChips.empty());
    o.gear = Gear::RandomTier;
    const LobbyScene rolledScene = BuildLobbyScene(InLobby(true, Sanitize(o)));
    const SettingRow* rolled = RowOf(rolledScene, LobbyRow::Gear);
    CHECK(rolled && rolled->tierChips.size() == 8);
}

// Grave Robbers: a co-op loot of one shared crypt with no fight after. Two rows (Run time, Dungeon tier) and the player limit.
static void TestGraveRobbers() {
    CoopFlag on(true);
    const MatchConfig g = SelectMode(MatchConfig{}, Format::FreeForAll, false, true, false, true);
    CHECK(SettingsFor(g) == std::vector<LobbyRow>({LobbyRow::Gather, LobbyRow::CryptsTier}));
    LobbyScene s = BuildLobbyScene(InLobby(true, g));
    std::vector<LobbyRow> ids; for (const SettingRow& r : s.settings) ids.push_back(r.id);
    CHECK(ids == std::vector<LobbyRow>({LobbyRow::Players, LobbyRow::Gather, LobbyRow::CryptsTier}));
    const SettingRow* run = RowOf(s, LobbyRow::Gather);
    CHECK(run && run->label == "Run time" && run->value == "10 min" && run->enabled);
    CHECK(RowOf(s, LobbyRow::CryptsTier) && RowOf(s, LobbyRow::CryptsTier)->label == "Dungeon tier");
    CHECK(!RowOf(s, LobbyRow::Battle) && !RowOf(s, LobbyRow::FightMap) && !RowOf(s, LobbyRow::CryptsRun) && !RowOf(s, LobbyRow::Gear) && !RowOf(s, LobbyRow::Tier));
    CHECK(s.summary == "GRAVE ROBBERS \xC2\xB7 Crypts, 10 min");
    for (const MapCard& m : s.maps) if (kMaps[m.index].crypts) CHECK(m.selected && m.allowed && m.enabled);
    // Sanitize: Grave Robbers is a Crypts mode that never runs Together, and only with the flag.
    MatchConfig both = g; both.cryptsTogether = true;
    CHECK(Sanitize(both).graveRobbers && !Sanitize(both).cryptsTogether && IsCrypts(Sanitize(both)));
    MatchConfig off = g; off.gear = Gear::Own; CHECK(!Sanitize(off).graveRobbers);
    g_cryptsCoopOffered = false;
    CHECK(!Sanitize(g).graveRobbers && !Sanitize(g).cryptsTogether);                      // a release: Crypt Royale
    LobbyLocal rel = InLobby(true, g); LobbyScene rs = BuildLobbyScene(rel);
    CHECK(rs.modes.size() == kModeCardCount && !GraveCard(rs)->shown);
}

// START: the co-op half is playable unless [mod] crypts_coop=0 (docs/PLAN_CRYPTS_COOP.md) and is refused with a plain notice then.
static void TestNotReady() {
    MatchConfig g = SelectMode(MatchConfig{}, Format::FreeForAll, false, true, false, true);
    MatchConfig t = Crypts(); t.cryptsTogether = true;
    {
        CoopFlag on(true);
        g = SelectMode(MatchConfig{}, Format::FreeForAll, false, true, false, true);
        CHECK(IsGraveRobbers(g) && ModeAvailable(g, 4).ok && BuildLobbyScene(InLobby(true, g, 4)).start);
        CHECK(ModeAvailable(t, 4).ok && BuildLobbyScene(InLobby(true, t, 4)).start);
    }
    {
        CoopFlag off(false);   // a release: the choice is not offered, and a config that carries it anyway is refused
        CHECK(!ModeAvailable(g, 4).ok && std::string(ModeAvailable(g, 4).hint) == "Grave Robbers is turned off (crypts_coop=0)");
        LobbyScene gs = BuildLobbyScene(InLobby(true, g, 4));
        CHECK(!gs.start && gs.startWhy.find("Grave Robbers is turned off (crypts_coop=0)") != std::string::npos);
        CHECK(!ModeAvailable(t, 4).ok && std::string(ModeAvailable(t, 4).hint) == "Crypts run Together is turned off (crypts_coop=0)");
        CHECK(!BuildLobbyScene(InLobby(true, t, 4)).start);
    }
    MatchConfig a = Crypts(); CHECK(ModeAvailable(a, 4).ok && BuildLobbyScene(InLobby(true, a, 4)).start);   // Apart is today's flow
    MatchConfig lms; lms.format = Format::LastManStanding; CHECK(ModeAvailable(lms, 3).ok);
}

// The maps: the Crypts map is the mode's (picked from any mode), the Fight map row holds where the battle is fought, and the summary
// names the map, the battle, the gather and the fight.
static void TestMapsAndSummary() {
    LobbyLocal l = InLobby(true, Crypts());
    LobbyScene s = BuildLobbyScene(l);
    const MapCard* cm = nullptr; const MapCard* abyss = nullptr; const MapCard* alley = nullptr;
    for (const MapCard& m : s.maps) { if (kMaps[m.index].crypts) cm = &m; if (m.name == "Abyss") abyss = &m; if (m.name == "Alley") alley = &m; }
    CHECK(cm && cm->name == "Crypts" && cm->allowed && cm->enabled && cm->selected && cm->tag == "Large level \xC2\xB7 the whole dungeon");
    CHECK(abyss && !abyss->allowed && !abyss->selected && alley && alley->allowed && !alley->selected);   // another map leaves the Crypts (MapPicked)
    CHECK(lobby_detail::MapPathOf(l) == kMaps[CryptsMapIndex()].path);
    CHECK(s.summary == "CRYPT ROYALE (DUEL) \xC2\xB7 Crypts \xC2\xB7 Crypts loot, 10 min, fight on Abyss");   // Peasant, the default: not named
    LobbyLocal deep = l; deep.config.cryptsTier = 4;
    CHECK(BuildLobbyScene(deep).summary == "CRYPT ROYALE (DUEL) \xC2\xB7 Crypts \xC2\xB7 Crypts loot, 10 min, fight on Abyss, dungeon up to Soldier");
    deep.config.cryptsTier = kCryptsTierTop;
    CHECK(BuildLobbyScene(deep).summary == "CRYPT ROYALE (DUEL) \xC2\xB7 Crypts \xC2\xB7 Crypts loot, 10 min, fight on Abyss, the game's own dungeon");
    deep.config.cryptsTier = 0;
    CHECK(BuildLobbyScene(deep).summary == "CRYPT ROYALE (DUEL) \xC2\xB7 Crypts \xC2\xB7 Crypts loot, 10 min, fight on Abyss, dungeon up to Beggar");
    LobbyLocal ownTier = noCryptsOf(l);
    CHECK(BuildLobbyScene(ownTier).summary.find("dungeon") == std::string::npos);        // another card: never named
    LobbyLocal noCrypts = l; noCrypts.config.gear = Gear::Own;
    LobbyScene o = BuildLobbyScene(noCrypts);
    for (const MapCard& m : o.maps) {
        if (m.name == "Abyss") CHECK(!m.allowed && !m.selected);                          // Last man standing alone: no Abyss
        if (kMaps[m.index].crypts) CHECK(m.allowed && !m.selected);                       // but the Crypts map is a click away
    }
    LobbyLocal duel = InLobby(true, Crypts(Format::Duel));
    LobbyScene d = BuildLobbyScene(duel);
    CHECK(d.start && d.summary.find("DUEL") != std::string::npos);                        // two players: a Duel
    LobbyScene d3 = BuildLobbyScene(InLobby(true, Crypts(Format::Duel), 3));
    CHECK(d3.start && d3.waiting.empty() && d3.summary.find("LAST MAN STANDING") != std::string::npos);   // three: the same card starts as Last man standing
    CHECK(ModeName(Format::TwoTeams, Gear::Crypts, 3) == "Crypt Royale (Teams)" && [] { LobbyFound f; f.mode = ModeName(Format::TwoTeams, Gear::Crypts); f.players = 4; f.maxPlayers = 8; return ModeMatches(f, "Crypt Royale"); }());
    CHECK(std::find(BrowserModes().begin(), BrowserModes().end(), "Crypt Royale") != BrowserModes().end());
    CHECK(std::string(GearName(Gear::Crypts)) == "Crypts loot");
}

// The Fight map row: a dropdown of the maps the Battle format fights on, the Abyss by default; a stale pick reads the default.
static void TestFightMap() {
    LobbyLocal l = InLobby(true, Crypts());
    const LobbyScene first = BuildLobbyScene(l);
    const SettingRow* row = RowOf(first, LobbyRow::FightMap);
    CHECK(row && row->label == "Fight map" && row->value == "Abyss" && row->enabled && row->hint == "Where the Battle is fought.");
    CHECK(lobby_detail::RowKindOf(LobbyRow::FightMap) == RowKind::Choice);
    const std::vector<DropOption> opts = lobby_detail::ChoicesFor(l, LobbyRow::FightMap);
    CHECK(!opts.empty() && opts.size() == lobby_detail::ChoiceCount(l.config, LobbyRow::FightMap));
    bool abyss = false, crypts = false, tavern = false; size_t at = 0;
    for (size_t i = 0; i < opts.size(); ++i) { abyss = abyss || opts[i].label == "Abyss"; crypts = crypts || opts[i].label == "Crypts"; tavern = tavern || opts[i].label == "Tavern"; if (opts[i].label == "Abyss") at = i; }
    CHECK(abyss && !crypts && !tavern);                                                  // never the Crypts map or a menu map
    CHECK(lobby_detail::ChoiceIndex(l, LobbyRow::FightMap) == at);
    l.fightPick = kMaps[MapIndex("Alley")].path;
    CHECK(RowOf(BuildLobbyScene(l), LobbyRow::FightMap)->value == "Alley" && opts[lobby_detail::ChoiceIndex(l, LobbyRow::FightMap)].label == "Alley");
    CHECK(BuildLobbyScene(l).summary.find("fight on Alley") != std::string::npos);
    l.fightPick = "/Game/Maps/Mods/Gone";                                                 // a map this build does not list: the default
    CHECK(RowOf(BuildLobbyScene(l), LobbyRow::FightMap)->value == "Abyss");
    // Changing the Battle row to a format that cannot fight on the pick falls back to the default (the Abyss, or an arena).
    l.fightPick = "/Game/Maps/Abyss/Abyss_Map_Open_EA";
    l.config = Crypts(Format::Tournament);
    const LobbyScene ts = BuildLobbyScene(l);
    const SettingRow* t = RowOf(ts, LobbyRow::FightMap);
    CHECK(t && t->value != "Abyss");
    for (const DropOption& o : lobby_detail::ChoicesFor(l, LobbyRow::FightMap)) CHECK(o.label != "Abyss");   // Tournament stays in the arenas
    l.config = Crypts(Format::TwoTeams);
    CHECK(RowOf(BuildLobbyScene(l), LobbyRow::FightMap)->value == "Abyss");              // back: the pick stands again
    LobbyScene m = BuildLobbyScene(InLobby(false, Crypts()));
    CHECK(RowOf(m, LobbyRow::FightMap) && !RowOf(m, LobbyRow::FightMap)->enabled);       // members read it
    // StepSetting leaves it alone (it is the lobby's, not the match's).
    CHECK(StepSetting(Crypts(), LobbyRow::FightMap, 1).format == Crypts().format);
}

int main() {
    TestCard();
    TestSelect();
    TestRows();
    TestSettings();
    TestGraveRobbers();
    TestNotReady();
    TestMapsAndSummary();
    TestFightMap();
    if (g_fail) { std::printf("test_hsm_crypts_scene: %d failure(s)\n", g_fail); return 1; }
    std::printf("test_hsm_crypts_scene: ok\n");
    return 0;
}
