// One Crossbow (#778): Last Man Standing's rounds between unarmed peasants; one of them starts each round with a crossbow
// and whoever holds it has endless bolts. The format's tables, the lobby's rows and transitions, the draw and the bolts.
#include "match/hsm_crossbow.h"
#include "match/hsm_hud_text.h"
#include "match/hsm_lobby_scene.h"
#include "match/hsm_match.h"
#include "match/hsm_modes.h"
#include "session/hsm_autoflow.h"
#include <cstdio>
#include <set>
#include <string>
#include <vector>

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++g_fail; } } while (0)
using namespace hsm;

static MatchConfig One() { MatchConfig c; c.format = Format::OneCrossbow; return Sanitize(c); }

// The format: a round format with no teams and no roles; bare peasants; exactly one crossbow whatever the lobby held.
static void TestFormat() {
    const Format f = Format::OneCrossbow;
    CHECK(uint8_t(f) == 17 && IsOneCrossbow(f) && !IsOneCrossbow(Format::LastManStanding) && !IsOneCrossbow(Format::CrossbowTDM));
    CHECK(IsRoundFormat(f) && !HasTeams(f) && !HasRoles(f) && !IsQueueFormat(f) && !IsTag(f) && ScoresPerFighter(f) && TeamCount(f, 3) == 0);
    CHECK(DefaultScoreLimit(f) == 3 && DropClearFor(f) == DropClear::EachRound);
    const MatchConfig c = One();
    CHECK(c.format == f && c.gear == Gear::Fists && IsBare(c) && !IsBrawl(c) && c.tier == kOneCrossbowTier && kOneCrossbowTier == 1);
    CHECK(c.crossbows == uint8_t(Crossbows::One));
    for (uint8_t stale : {uint8_t(0), uint8_t(Crossbows::All), uint8_t(Crossbows::Half), uint8_t(9)}) {   // whatever another mode left
        MatchConfig o; o.format = f; o.crossbows = stale; o.gear = Gear::Mirror; o.tier = 7;
        const MatchConfig s = Sanitize(o);
        CHECK(s.crossbows == uint8_t(Crossbows::One) && s.gear == Gear::Fists && s.tier == kOneCrossbowTier);
    }
    CHECK(GearAllowed(f, Gear::Fists) && GearAllowed(Format::Tag, Gear::Fists) && !GearAllowed(Format::Duel, Gear::Fists));
    CHECK(RegearFor(c, RespawnCause::RoundReset) == Regear::None && RegearFor(c, RespawnCause::Rematch) == Regear::None);   // nothing to roll
    { MatchConfig brawl; brawl.gear = Gear::Fists; CHECK(Sanitize(brawl).tier == kBrawlTier && Sanitize(brawl).crossbows == 0); }   // Brawl keeps its own
    { MatchConfig tag; tag.format = Format::Tag; tag.crossbows = 2; CHECK(Sanitize(tag).crossbows == 0 && Sanitize(tag).tier == kBrawlTier); }
    { MatchConfig tdm; tdm.format = Format::CrossbowTDM; CHECK(Sanitize(tdm).crossbows == uint8_t(Crossbows::All)); }
    // the rounds are Last Man Standing's
    Match m; m.Start(c, {{1, kNoTeam, true}, {2, kNoTeam, true}, {3, kNoTeam, true}}, 0);
    const uint32_t t = Match::kCountdownMs;
    m.Tick(t);
    m.OnDeath(1, 2, t + 1); m.OnDeath(2, 3, t + 2);
    CHECK(m.roundWinner() == 3 && m.Score(3) == 1 && !m.RoundInPlay());
    const auto ev = m.Tick(t + 2 + 5000);
    CHECK(ev.roundReset && ev.regear == Regear::None && ev.respawn == std::vector<uint8_t>({1, 2, 3}) && m.round() == 2);
}

// The one crossbow: one holder a round, a different one over the rounds; no crossbow class: fist fights, and the host says so.
static void TestTheCrossbow() {
    const MatchConfig c = One();
    CHECK(EffectiveCrossbows(c, true) == uint8_t(Crossbows::One) && EffectiveCrossbows(c, false) == uint8_t(Crossbows::Off));
    CHECK(std::string(CrossbowMissingLine(Format::OneCrossbow)) == kOneCrossbowMissingLine && std::string(CrossbowMissingLine(Format::CrossbowTDM)) == kCrossbowTdmMissingLine);
    CHECK(std::string(CrossbowMissingLine(Format::LastManStanding)) == kCrossbowRowMissingLine && !CrossbowForEveryone(Format::OneCrossbow, true));
    std::set<uint8_t> holders;
    for (uint32_t round = 0; round < 200; ++round) {
        const std::vector<uint8_t> p = PickCrossbowHolders(c.crossbows, {1, 2, 3, 4, 5}, round * 2654435761u + 7);
        CHECK(p.size() == 1);
        if (!p.empty()) holders.insert(p[0]);
    }
    CHECK(holders == std::set<uint8_t>({1, 2, 3, 4, 5}));                 // everybody's turn comes
    CHECK(CrossbowCount(c.crossbows, 2) == 1 && CrossbowCount(c.crossbows, 20) == 1);
}

// Endless bolts: only in One Crossbow, in a running match, for a living fighter holding the crossbow; a quiver under the
// floor is filled up, any other is left alone.
static void TestEndlessBolts() {
    const Format f = Format::OneCrossbow;
    CHECK(EndlessBoltsFor(f, true, true, true));
    CHECK(!EndlessBoltsFor(f, false, true, true) && !EndlessBoltsFor(f, true, false, true) && !EndlessBoltsFor(f, true, true, false));
    for (Format o : {Format::LastManStanding, Format::CrossbowTDM, Format::FreeForAll, Format::Chaos}) CHECK(!EndlessBoltsFor(o, true, true, true));
    CHECK(kEndlessBoltFloor == 2);
    CHECK(EndlessBolts(0, 8) == 8 && EndlessBolts(1, 8) == 8 && EndlessBolts(2, 8) == 2 && EndlessBolts(5, 8) == 5 && EndlessBolts(8, 8) == 8 && EndlessBolts(30, 8) == 30);
    CHECK(EndlessBolts(-3, 8) == 8);                                       // a bad count is under the floor too
    CHECK(EndlessBolts(1, 1) == 1 && EndlessBolts(1, 0) == 1 && EndlessBolts(0, 1) == 1);   // never fewer than there are
}

// The lobby: its card, its rows (no gear rows, no Crossbows row), two players; picking it makes the peasants, leaving it
// puts gear, tier and the Crossbows row back.
static void TestLobby() {
    const Format f = Format::OneCrossbow;
    CHECK(FormatName(f) == "One crossbow" && std::string(FormatBlurb(f)).find("crossbow that never runs out of bolts") != std::string::npos);
    CHECK(SettingsFor(One()) == std::vector<LobbyRow>({LobbyRow::Limit, LobbyRow::Time, LobbyRow::Respawn, LobbyRow::FreeCam, LobbyRow::Damage, LobbyRow::DeadWeight,
                                                      LobbyRow::WeaponWeight, LobbyRow::Sync}));
    CHECK(!ModeAvailable(f, 1).ok && ModeAvailable(f, 2).ok && ModeAvailable(f, 20).ok && std::string(ModeAvailable(f, 1).hint) == "Needs 2 or more players");
    bool card = false, browser = false;
    for (size_t i = 0; i < kModeOrderCount; ++i) card = card || kModeOrder[i] == f;
    for (const std::string& n : BrowserModes()) browser = browser || n == "One crossbow";
    CHECK(card && browser && ModeMatches("One crossbow", "One crossbow"));
    Format parsed = Format::FreeForAll;
    CHECK(ParseServerFormat("onecrossbow", &parsed) && parsed == f);
    for (const MapInfo& m : kMaps) CHECK(MapAllowsFormat(m, f) == MapAllowsFormat(m, Format::LastManStanding));
    // picking the card
    MatchConfig from; from.gear = Gear::Mirror; from.tier = 6; from.crossbows = uint8_t(Crossbows::Half); from.damagePct = 150;
    const MatchConfig in = SelectMode(from, f, false);
    CHECK(in.format == f && in.gear == Gear::Fists && in.tier == kOneCrossbowTier && in.crossbows == uint8_t(Crossbows::One) && in.scoreLimit == DefaultScoreLimit(f));
    CHECK(in.damagePct == 150 && Sanitize(in).gear == Gear::Fists);         // the Damage row is the host's still
    // leaving it: for a format card and for the Brawl card
    const MatchConfig out = SelectMode(in, Format::LastManStanding, false);
    CHECK(out.format == Format::LastManStanding && out.gear == Gear::Own && out.tier == MatchConfig{}.tier && out.crossbows == 0 && out.damagePct == 150);
    const MatchConfig brawl = SelectMode(in, Format::FreeForAll, true);
    CHECK(IsBrawl(brawl) && brawl.tier == kBrawlTier && brawl.crossbows == 0);
    MatchConfig tagLike = in; tagLike.damagePct = kTagDamagePct;               // Tag's light hits are Tag's: One Crossbow keeps what the host set
    CHECK(SelectMode(tagLike, Format::Duel, false).damagePct == kTagDamagePct);
    MatchConfig tag = SelectMode(MatchConfig{}, Format::Tag, false);
    CHECK(tag.damagePct == kTagDamagePct && SelectMode(tag, Format::Duel, false).damagePct == kDamageStock && SelectMode(tag, f, false).crossbows == uint8_t(Crossbows::One));
    CHECK(SelectMode(in, f, false).crossbows == uint8_t(Crossbows::One));      // the same card again: nothing changes
    // the scene: the card is there and selected, no gear in the summary, the pill reads rounds
    LobbyLocal l; l.phase = LobbyPhase::InLobby; l.host = true; l.myName = "Ann"; l.version = "0.2.0"; l.config = in;
    for (const char* name : {"Ann", "Bob", "Cat"}) { LobbyPlayer p; p.name = name; p.me = l.players.empty(); p.host = l.players.empty(); l.players.push_back(p); }
    const LobbyScene s = BuildLobbyScene(l);
    bool selected = false;
    for (const ModeCard& m : s.modes) if (m.format == f && !m.brawl && !m.progression) selected = m.selected && m.name == "ONE CROSSBOW" && m.enabled;
    CHECK(selected && s.summary.find("ONE CROSSBOW") == 0 && s.summary.find("Bare") == std::string::npos && s.summary.find("Own loadouts") == std::string::npos);
    MatchInfo mi; mi.format = uint8_t(f); mi.phase = uint8_t(MatchPhase::Live); mi.round = 1; mi.scoreLimit = 3;
    FighterEntry a; a.entity = 1; a.name = "Ann"; a.alive = true;
    FighterEntry b; b.entity = 2; b.name = "Bo"; b.alive = true;
    mi.fighters = {a, b};
    CHECK(StatusLine(mi, 1).find("Rounds") == 0);
}

int main() {
    TestFormat(); TestTheCrossbow(); TestEndlessBolts(); TestLobby();
    if (g_fail) { std::printf("%d check(s) failed\n", g_fail); return 1; }
    std::printf("one crossbow tests passed\n");
    return 0;
}
