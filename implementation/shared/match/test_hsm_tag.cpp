// Tag (match/hsm_modes.h TagGame): the fuse passes on a landed hit, the fighter who just passed cannot be tagged back for 1.5 s,
// the fuse running out eliminates "it" and a new one is picked among the living, the round ends with the last one standing,
// the first "it" rotates, and "it" leaving or dying hands the fuse on. Also the Match integration (rounds), the config rules,
// the wire fields, the HUD text and the lobby's rows, card and availability.
#include "match/hsm_hud_scene.h"
#include "match/hsm_lobby_scene.h"
#include "match/hsm_maps.h"
#include "match/hsm_modes.h"
#include "match/hsm_nametag.h"
#include "protocol/hsm_protocol.h"
#include <cstdio>
#include <set>

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++g_fail; } } while (0)
using namespace hsm;

using V = std::vector<uint8_t>;
static bool Has(const std::string& s, const char* part) { return s.find(part) != std::string::npos; }

// Criterion: "it" is picked by seed among the living, always the same for the same input, and every fighter gets a turn over n steps.
static void TestPick() {
    CHECK(PickIt({}, 1, 0) == 0);
    CHECK(PickIt({4}, 99, 7) == 4);
    const V ids = {5, 2, 9, 2, 7};                              // order and repeats do not matter
    CHECK(PickIt(ids, 77, 3) == PickIt({2, 5, 7, 9}, 77, 3));
    std::set<uint8_t> seen;
    for (uint32_t step = 0; step < 4; ++step) seen.insert(PickIt(ids, 77, step));
    CHECK(seen == (std::set<uint8_t>{2, 5, 7, 9}));             // rotation: n consecutive steps are n different fighters
    bool differs = false;
    for (uint32_t seed = 1; seed < 20; ++seed) differs = differs || PickIt(ids, seed, 0) != PickIt(ids, 1, 0);
    CHECK(differs);                                             // the seed matters
    CHECK(FuseMs(30) == 30000 && FuseMs(1) == 10000 && FuseMs(200) == 60000);
}

// Criterion: a round starts with one living fighter "it" and a full fuse; fewer than two fighters have no "it".
static void TestRoundStart() {
    TagGame t; t.Reset(5, 30);
    CHECK(!t.active() && t.it() == 0 && t.FuseLeftMs(0) == 0);
    t.BeginRound({1, 2, 3}, 1000);
    CHECK(t.active() && t.it() != 0 && t.it() <= 3 && t.FuseLeftMs(1000) == 30000 && t.FuseLeftMs(11000) == 20000);
    CHECK(t.FuseLeftMs(31000) == 0 && t.FuseLeftMs(60000) == 0);
    t.BeginRound({7}, 2000);
    CHECK(t.active() && t.it() == 0);                           // nobody to chase
    t.EndRound();
    CHECK(!t.active() && t.it() == 0 && !t.OnHit(1, 2, 3000));
}

// Criterion: a hit by "it" on a living fighter passes the fuse (it keeps burning); nobody else's hit does; not on a dead fighter.
static void TestPass() {
    TagGame t; t.Reset(11, 30);
    t.BeginRound({1, 2, 3}, 0);
    const uint8_t a = t.it(), b = a == 1 ? 2 : 1, c = uint8_t(6 - a - b);
    CHECK(!t.OnHit(b, c, 100) && t.it() == a);                  // only "it" passes
    CHECK(!t.OnHit(a, a, 100) && t.it() == a);                  // not to itself
    CHECK(!t.OnHit(a, 9, 100) && t.it() == a);                  // not to a stranger
    CHECK(t.OnHit(a, b, 10000) && t.it() == b);
    CHECK(t.FuseLeftMs(10000) == 20000);                        // the fuse is carried, not reset
    CHECK(!t.OnHit(a, c, 10100) && t.it() == b);                // the old "it" no longer passes it
    // A dead fighter is not tagged.
    t.Update({a, b}, 10200);                                    // c died
    CHECK(!t.OnHit(b, c, 10300) && t.it() == b);
}

// Criterion: the fighter who just passed the fuse cannot be tagged back for 1.5 s; the new "it" may tag a third at once.
static void TestNoTagBack() {
    TagGame t; t.Reset(3, 30);
    t.BeginRound({1, 2, 3}, 0);
    const uint8_t a = t.it(), b = a == 1 ? 2 : 1, c = 6 - a - b;
    CHECK(t.OnHit(a, b, 5000));
    CHECK(!t.OnHit(b, a, 5000) && !t.OnHit(b, a, 5000 + kTagBackMs - 1) && t.it() == b);   // inside the window
    CHECK(t.OnHit(b, c, 5100) && t.it() == c);                  // a third fighter is fair game at once
    CHECK(!t.OnHit(c, b, 5200));                                // b just passed to c: c cannot tag b back inside the window
    CHECK(t.OnHit(c, a, 5200) && t.it() == a);
    CHECK(t.OnHit(a, c, 5200 + kTagBackMs) && t.it() == c);     // after the window the old holder may be tagged again
    TagGame u; u.Reset(3, 30); u.BeginRound({1, 2}, 0);
    const uint8_t x = u.it(), y = 3 - x;
    CHECK(u.OnHit(x, y, 1000) && !u.OnHit(y, x, 2000) && u.OnHit(y, x, 2500) && u.it() == x);   // two fighters: back after 1.5 s
}

// Criterion: a pass never leaves less than 3 s on the fuse.
static void TestPassFloor() {
    TagGame t; t.Reset(3, 10);
    t.BeginRound({1, 2, 3}, 0);
    const uint8_t a = t.it(), b = a == 1 ? 2 : 1;
    CHECK(t.OnHit(a, b, 9500));                                 // 0.5 s were left
    CHECK(t.FuseLeftMs(9500) == kTagPassFloorMs);
    CHECK(t.Update({1, 2, 3}, 9500 + kTagPassFloorMs - 1).burned == 0);
    CHECK(t.Update({1, 2, 3}, 9500 + kTagPassFloorMs).burned == b);
}

// Criterion: the fuse running out eliminates "it" (the caller kills it) and a new "it" is picked among the living, with a full fuse.
static void TestExpiry() {
    TagGame t; t.Reset(21, 20);
    t.BeginRound({1, 2, 3, 4}, 0);
    const uint8_t first = t.it();
    CHECK(t.Update({1, 2, 3, 4}, 19999).burned == 0 && t.it() == first);
    const TagGame::Step s = t.Update({1, 2, 3, 4}, 20000);
    CHECK(s.burned == first && s.picked != 0 && s.picked != first && t.it() == s.picked);
    CHECK(t.FuseLeftMs(20000) == 20000);                        // the next one starts a full fuse
    // The burned fighter is out: the next tick (without it) keeps the new "it".
    V rest; for (uint8_t e : V{1, 2, 3, 4}) if (e != first) rest.push_back(e);
    CHECK(t.Update(rest, 20200).picked == 0 && t.it() == s.picked);
    // Down to two: the fuse burns one, one is left, so there is no new "it".
    TagGame u; u.Reset(2, 10);
    u.BeginRound({3, 8}, 0);
    const uint8_t it = u.it();
    const TagGame::Step e = u.Update({3, 8}, 10000);
    CHECK(e.burned == it && e.picked == 0 && u.it() == 0);
    CHECK(u.Update({it == 3 ? uint8_t(8) : uint8_t(3)}, 10100).burned == 0);   // one standing: nothing happens
}

// Criterion: "it" killed or gone from the living is replaced with a full fuse; the new "it" is a living fighter.
static void TestItLeaves() {
    TagGame t; t.Reset(8, 30);
    t.BeginRound({1, 2, 3}, 0);
    const uint8_t was = t.it();
    V rest; for (uint8_t e : V{1, 2, 3}) if (e != was) rest.push_back(e);
    const TagGame::Step s = t.Update(rest, 12000);
    CHECK(s.burned == 0 && s.picked != 0 && s.picked != was && t.FuseLeftMs(12000) == 30000);
    CHECK(std::find(rest.begin(), rest.end(), t.it()) != rest.end());
    // Only one is left: no "it".
    CHECK(t.Update({rest[0]}, 12100).picked == 0 && t.it() == 0);
}

// Criterion: the first "it" of a round and a rematch rotates (the step counter moves on) and is the same for the same seed.
static void TestRotation() {
    std::set<uint8_t> firsts;
    TagGame t; t.Reset(42, 30);
    for (int round = 0; round < 3; ++round) { t.BeginRound({1, 2, 3}, uint32_t(round) * 100000u); firsts.insert(t.it()); t.EndRound(); }
    CHECK(firsts.size() == 3);                                  // three rounds, three different fighters
    TagGame u; u.Reset(42, 30);
    u.BeginRound({1, 2, 3}, 0);
    TagGame v; v.Reset(42, 30);
    v.BeginRound({1, 2, 3}, 777);
    CHECK(u.it() == v.it());                                    // deterministic by seed
    t.Reset(42, 30); t.BeginRound({1, 2, 3}, 0);
    CHECK(t.it() == u.it());                                    // Reset starts the rotation over
}

// Criterion: through the Match, the fuse's elimination is a death with no killer: the last one standing takes the round, the rounds
// to win are Duel's, and the next round starts afresh.
static void TestRounds() {
    MatchConfig c; c.format = Format::Tag; c.scoreLimit = 2; c.respawnDelayS = 3;
    CHECK(IsRoundFormat(Format::Tag) && ScoresPerFighter(Format::Tag) && !HasTeams(Format::Tag) && TeamCount(Format::Tag, 3) == 0);
    CHECK(DefaultScoreLimit(Format::Tag) == 3 && DropClearFor(Format::Tag) == DropClear::EachRound);
    Match m; m.Start(c, {{1, kNoTeam, true}, {2, kNoTeam, true}, {3, kNoTeam, true}}, 0);
    const uint32_t live = Match::kCountdownMs;
    m.Tick(live);
    CHECK(m.phase() == MatchPhase::Live && m.RoundInPlay());
    TagGame t; t.Reset(9, 10);
    t.BeginRound({1, 2, 3}, live);
    uint32_t now = live;
    V living = {1, 2, 3};
    for (int burns = 0; burns < 2; ++burns) {                   // the fuse burns two fighters in turn, each new "it" with a full fuse
        now += 10000;
        const TagGame::Step s = t.Update(living, now);
        CHECK(s.burned != 0);
        m.OnDeath(s.burned, 0, now);
        CHECK(m.Kills(s.burned) == 0 && m.Deaths(s.burned) == 1);
        living.erase(std::find(living.begin(), living.end(), s.burned));
    }
    CHECK(!m.RoundInPlay());
    const uint8_t winner = m.roundWinner();
    CHECK(winner != 0 && m.Alive(winner) && m.Score(int(winner)) == 1);
    // Round break, then everybody is back with a fresh "it".
    Match::Events ev = m.Tick(now + 3000);
    CHECK(ev.roundReset && ev.respawn.size() == 3 && m.round() == 2);
    t.BeginRound({1, 2, 3}, now + 3000);
    CHECK(t.it() != 0 && t.FuseLeftMs(now + 3000) == 10000);
    // The same fighter winning the second round takes the match (rounds to win: 2).
    for (uint8_t e : V{1, 2, 3}) if (e != winner) m.OnDeath(e, winner, now + 4000);
    CHECK(m.phase() == MatchPhase::Results && m.winner() == int(winner));
}

// Criterion: Sanitize clamps the fuse to the Fuse row, drops the clock and the crossbows Tag has no rows for.
static void TestConfig() {
    MatchConfig c; c.format = Format::Tag;
    CHECK(c.fuseS == kFuseDefaultS && kFuseDefaultS == 30);
    c.fuseS = 3; CHECK(Sanitize(c).fuseS == kFuseMinS);
    c.fuseS = 250; CHECK(Sanitize(c).fuseS == kFuseMaxS);
    c.fuseS = 45; c.timeLimitS = 600; c.crossbows = 4; c.clearGroundMin = 3;
    const MatchConfig s = Sanitize(c);
    CHECK(s.fuseS == 45 && s.timeLimitS == 0 && s.crossbows == 0 && s.clearGroundMin == 0);
    c.format = Format::FreeForAll; c.timeLimitS = 600; c.crossbows = 4;
    CHECK(Sanitize(c).timeLimitS == 600 && Sanitize(c).crossbows == 4);   // other modes keep theirs
}

// Criterion (0.8.0-pre26): Tag is bare-fisted on every machine (Sanitize), picking it starts at light hits (kTagDamagePct)
// the host can still change, and leaving it puts the gear, tier and (untouched) damage back.
static void TestBare() {
    MatchConfig c; c.format = Format::Tag; c.gear = Gear::RandomTier; c.tier = 6; c.crossbows = 3;
    const MatchConfig s = Sanitize(c);
    CHECK(s.gear == Gear::Fists && s.tier == kBrawlTier && s.crossbows == 0 && IsBare(s) && !IsBrawl(s));
    CHECK(GearAllowed(Format::Tag, Gear::Fists) && GearAllowed(Format::FreeForAll, Gear::Fists) && !GearAllowed(Format::Duel, Gear::Fists));
    CHECK(kTagDamagePct == 10 && IsBare(Gear::Fists) && !IsBare(Gear::Own));
    MatchConfig own; own.format = Format::Duel; own.gear = Gear::RandomTier; own.tier = 6;
    const MatchConfig t = SelectMode(own, Format::Tag, false);
    CHECK(t.format == Format::Tag && t.gear == Gear::Fists && t.tier == kBrawlTier && t.damagePct == kTagDamagePct);
    const DamageRange r;                                                  // the vanilla slider: 0..200 by 10, stock 100
    CHECK(StepSetting(t, LobbyRow::Damage, 1, r).damagePct == 20 && StepSetting(t, LobbyRow::Damage, -1, r).damagePct == 0);   // the host can change it
    MatchConfig back = SelectMode(t, Format::Duel, false);
    CHECK(back.format == Format::Duel && back.gear == Gear::Own && back.tier == MatchConfig{}.tier && back.damagePct == kDamageStock);
    MatchConfig changed = t; changed.damagePct = 50;
    back = SelectMode(changed, Format::FreeForAll, false);
    CHECK(back.gear == Gear::Own && back.damagePct == 50);                // a damage the host chose stays
    back = SelectMode(t, Format::FreeForAll, true);                       // Tag to the Brawl card: still bare, stock damage
    CHECK(IsBrawl(back) && back.damagePct == kDamageStock);
    MatchConfig brawl = SelectMode(MatchConfig{}, Format::FreeForAll, true);
    const MatchConfig tag = SelectMode(brawl, Format::Tag, false);       // Brawl to Tag: bare, light hits
    CHECK(tag.format == Format::Tag && tag.gear == Gear::Fists && tag.damagePct == kTagDamagePct);
    CHECK(SelectMode(tag, Format::Tag, false).damagePct == kTagDamagePct && SelectFormat(changed, Format::Tag).damagePct == 50);   // the same card changes nothing
    LobbyLocal l; l.phase = LobbyPhase::InLobby; l.host = true; l.config = Sanitize(tag); l.playerCount = 3;
    const LobbyScene ls = BuildLobbyScene(l);
    CHECK(ls.summary.find("TAG") != std::string::npos && ls.summary.find("loadouts") == std::string::npos && ls.summary.find("tier") == std::string::npos);
    bool blurb = false;
    for (const ModeCard& m : ls.modes) if (m.format == Format::Tag) blurb = Has(m.description, "Bare fists");
    CHECK(blurb);
}

// Criterion: the wire carries "it" and the fuse; a fuse without an "it", too long a fuse and a stranger as "it" are rejected.
static void TestWire() {
    MatchInfo m; m.format = uint8_t(Format::Tag); m.phase = uint8_t(MatchPhase::Live); m.round = 1;
    m.tagIt = 2; m.fuseLeftMs = 12345;
    m.fighters = {{1, kNoTeam, 0, 0, "Ann", 0, true, false, 0, 0}, {2, kNoTeam, 0, 0, "Bo", 0, true, false, 0, 0}};
    auto b = Encode(m); MatchInfo o;
    CHECK(Decode(b.data(), b.size(), o) && o.tagIt == 2 && o.fuseLeftMs == 12345 && o.format == uint8_t(Format::Tag));
    for (size_t cut = 0; cut < b.size(); ++cut) CHECK(!Decode(b.data(), cut, o));
    MatchInfo q = m; q.tagIt = 0; q.fuseLeftMs = 5; b = Encode(q); CHECK(!Decode(b.data(), b.size(), o));
    q = m; q.fuseLeftMs = uint16_t(kFuseMaxS * 1000 + 1); b = Encode(q); CHECK(!Decode(b.data(), b.size(), o));
    q = m; q.fuseLeftMs = uint16_t(kFuseMaxS * 1000); b = Encode(q); CHECK(Decode(b.data(), b.size(), o) && o.fuseLeftMs == kFuseMaxS * 1000);
    q = m; q.tagIt = uint8_t(kMaxWillies + 1); b = Encode(q); CHECK(!Decode(b.data(), b.size(), o));
    q = m; q.tagIt = 0; q.fuseLeftMs = 0; b = Encode(q); CHECK(Decode(b.data(), b.size(), o) && o.tagIt == 0);
    // The reliable feed event.
    ModeEventMsg e; e.kind = ModeEventKind::TagPass; e.a = 1; e.b = 2;
    auto eb = Encode(e); ModeEventMsg eo; MsgType t;
    CHECK(PeekType(eb.data(), eb.size(), t) && t == MsgType::ModeEvent && Decode(eb.data(), eb.size(), eo) && eo.kind == ModeEventKind::TagPass && eo.a == 1 && eo.b == 2);
    for (size_t cut = 0; cut < eb.size(); ++cut) CHECK(!Decode(eb.data(), cut, eo));
    e.b = 1; eb = Encode(e); CHECK(!Decode(eb.data(), eb.size(), eo));                          // tagging yourself
    e = {ModeEventKind::TagPass, 0, 2}; eb = Encode(e); CHECK(!Decode(eb.data(), eb.size(), eo));
    e = {ModeEventKind::TagBurn, 3, 0}; eb = Encode(e); CHECK(Decode(eb.data(), eb.size(), eo) && eo.kind == ModeEventKind::TagBurn && eo.a == 3);
    e = {ModeEventKind::TagBurn, 0, 0}; eb = Encode(e); CHECK(!Decode(eb.data(), eb.size(), eo));
    e = {ModeEventKind::Infected, 0, 4}; eb = Encode(e); CHECK(Decode(eb.data(), eb.size(), eo) && eo.a == 0 && eo.b == 4);
    e = {ModeEventKind::Infected, 2, 0}; eb = Encode(e); CHECK(!Decode(eb.data(), eb.size(), eo));
    e = {ModeEventKind::TagPass, uint8_t(kMaxWillies + 1), 2}; eb = Encode(e); CHECK(!Decode(eb.data(), eb.size(), eo));
    eb = Encode(ModeEventMsg{ModeEventKind::TagPass, 1, 2}); eb[4] = 0; CHECK(!Decode(eb.data(), eb.size(), eo));    // kind 0
    eb[4] = 4; CHECK(!Decode(eb.data(), eb.size(), eo));                                                              // kind 4
}

static MatchInfo TagInfo(uint8_t it, uint16_t fuseMs) {
    MatchInfo m; m.format = uint8_t(Format::Tag); m.phase = uint8_t(MatchPhase::Live); m.round = 2; m.scoreLimit = 3; m.tagIt = it; m.fuseLeftMs = fuseMs;
    m.fighters = {{1, kNoTeam, 0, 0, "Ann", 0, true, false, 0, 0}, {2, kNoTeam, 0, 0, "Bo", 0, true, false, 0, 0}, {3, kNoTeam, 0, 0, "Cy", 0, false, false, 0, 0}};
    return m;
}

// Criterion: "YOU ARE IT 12 s" for the it-fighter, "Bo is it" for the others, "it" red for everyone, an IT tag on the board.
static void TestHud() {
    const MatchInfo me = TagInfo(1, 11500), other = TagInfo(2, 11500);
    CHECK(TagLine(me, 1) == "YOU ARE IT 12 s" && TagLine(me, 2) == "Ann is it 12 s" && TagLine(TagInfo(0, 0), 1).empty());
    LocalHud l; l.me = 1; l.host = 1; l.infoAtMs = 1000;
    HudScene s = BuildHudScene(me, l, 1000);
    CHECK(s.pill.text == "YOU ARE IT 12 s" && s.pill.color.r == style::kBloodInk.r);
    s = BuildHudScene(me, l, 3000);
    CHECK(s.pill.text == "YOU ARE IT 10 s");                    // the clock runs on between MatchInfos
    s = BuildHudScene(other, l, 1000);
    CHECK(s.pill.text == "ROUND 2 \xC2\xB7 Bo is it 12 s");
    CHECK(FighterInk(other, 2).r == style::kBloodInk.r && FighterInk(other, 1).r == style::kBone.r);
    CHECK(FighterInk(me, 1).g == style::kBloodInk.g);
    l.tab = true; s = BuildHudScene(other, l, 1000);
    int its = 0; for (const BoardRow& r : s.board.rows) if (r.tag == "IT") { ++its; CHECK(r.name == "Bo"); }
    CHECK(its == 1);
    CHECK(Has(StatusLine(other, 1), "Bo is it 12 s"));
    // The fuse of a fresh info: 0 left shows 0.
    CHECK(TagLine(TagInfo(2, 0), 1) == "Bo is it 0 s");
    // The banner: it fires when this machine's fighter becomes "it", not at the first info.
    BannerTracker b;
    b.Observe(TagInfo(2, 9000), 1, 100);
    b.Observe(TagInfo(1, 9000), 1, 200);
    CHECK(b.shot().text == "YOU ARE IT" && b.shot().atMs == 200);
    BannerTracker c; c.Observe(TagInfo(1, 9000), 1, 100);
    CHECK(c.shot().text.empty());                               // joining a running match fires nothing
    // The feed lines.
    CHECK(ModeEventLine(me, ModeEventMsg{ModeEventKind::TagPass, 1, 2}) == "Ann tagged Bo");
    CHECK(ModeEventLine(me, ModeEventMsg{ModeEventKind::TagBurn, 2, 0}) == "Bo burned out");
    const FeedRow pass = MakeModeFeedRow(other, ModeEventMsg{ModeEventKind::TagPass, 1, 2}, 3);
    CHECK(pass.spans.size() == 3 && pass.spans[0].text == "Ann" && pass.spans[1].text == " tagged " && pass.spans[2].text == "Bo" && !pass.mine);
    CHECK(pass.spans[2].color.r == style::kBloodInk.r && pass.spans[0].color.r == style::kBone.r);   // the new "it" is red
    CHECK(MakeModeFeedRow(other, ModeEventMsg{ModeEventKind::TagPass, 1, 2}, 2).mine && MakeModeFeedRow(other, ModeEventMsg{ModeEventKind::TagBurn, 2, 0}, 2).mine);
    const FeedRow burn = MakeModeFeedRow(other, ModeEventMsg{ModeEventKind::TagBurn, 2, 0}, 1);
    CHECK(burn.spans.size() == 2 && burn.spans[1].text == " burned out" && !burn.mine);
    // The name tag ink.
    CHECK(NameTagInk(Format::Tag, kNoTeam, true).r == style::kBloodInk.r && NameTagInk(Format::Tag, kNoTeam, false).r == style::kBone.r);
    CHECK(NameTagInk(Format::FreeForAll, kNoTeam, true).r == style::kBone.r);                   // "it" means nothing elsewhere
    NameTagView v = NameTagLayout(true, 100, 100, 1920, 1080, 1.f, 800.f, true, kNoTeam, Format::Tag, 1.f, true);
    CHECK(v.visible && v.ink.r == style::kBloodInk.r);
    CHECK(Has(ModeName(Format::Tag, Gear::Own), "Tag") && ModeName(Format::Tag, Gear::Own, 3) == "Tag");
    // The extrapolation of the fuse.
    CHECK(Extrapolate(me, 0, 1500).fuseLeftMs == 10000 && Extrapolate(me, 0, 99999).fuseLeftMs == 0);
}

static LobbyLocal InLobby(Format f, int players = 4) {
    LobbyLocal l; l.phase = LobbyPhase::InLobby; l.host = true; l.config.format = f; l.playerCount = players;
    return l;
}
static std::vector<LobbyRow> Ids(const LobbyScene& s) { std::vector<LobbyRow> v; for (const SettingRow& r : s.settings) v.push_back(r.id); return v; }

// Criterion: the card, its blurb, the rows (Limit, Fuse time, Damage, FreeCam, Sync, Gear/Tier/Weapons), 2+ players, and arenas only.
static void TestLobby() {
    LobbyScene s = BuildLobbyScene(InLobby(Format::Tag));
    const ModeCard* card = nullptr;
    for (const ModeCard& m : s.modes) if (m.format == Format::Tag) card = &m;
    CHECK(card && card->name == "TAG" && card->selected && card->enabled && card->group == ModeGroup::Versus && !card->description.empty());
    CHECK(Ids(s) == (std::vector<LobbyRow>{LobbyRow::Limit, LobbyRow::Respawn, LobbyRow::Fuse, LobbyRow::FreeCam, LobbyRow::Damage, LobbyRow::Sync}));   // bare fists: no gear rows
    for (const SettingRow& r : s.settings) {
        if (r.id == LobbyRow::Fuse) CHECK(r.label == "Fuse time" && r.value == "30 s" && r.enabled && !r.hint.empty());
        if (r.id == LobbyRow::Limit) CHECK(r.label == "Rounds to win");
        if (r.id == LobbyRow::Respawn) CHECK(r.label == "Round break");
    }
    LobbyLocal rolled = InLobby(Format::Tag); rolled.config.gear = Gear::RandomTier;
    s = BuildLobbyScene(rolled);
    CHECK(Ids(s) == (std::vector<LobbyRow>{LobbyRow::Limit, LobbyRow::Respawn, LobbyRow::Fuse, LobbyRow::FreeCam, LobbyRow::Damage, LobbyRow::Sync}));   // a stale gear changes nothing
    CHECK(!ModeAvailable(Format::Tag, 1).ok && ModeAvailable(Format::Tag, 2).ok && ModeAvailable(Format::Tag, 8).ok);
    s = BuildLobbyScene(InLobby(Format::FreeForAll, 1));
    for (const ModeCard& m : s.modes) if (m.format == Format::Tag) CHECK(!m.enabled && !m.hint.empty());
    // The stepper: 10..60, wrapping, and a stale value steps from the start.
    MatchConfig c; c.format = Format::Tag;
    CHECK(StepSetting(c, LobbyRow::Fuse, 1).fuseS == 45 && StepSetting(c, LobbyRow::Fuse, -1).fuseS == 20);
    c.fuseS = 60; CHECK(StepSetting(c, LobbyRow::Fuse, 1).fuseS == 10);
    c.fuseS = 10; CHECK(StepSetting(c, LobbyRow::Fuse, -1).fuseS == 60);
    c.fuseS = 33; CHECK(StepSetting(c, LobbyRow::Fuse, 1).fuseS == 10);
    c = SelectFormat(MatchConfig{}, Format::Tag); CHECK(c.format == Format::Tag && c.scoreLimit == 3);
    // Arenas only, like free-for-all.
    for (const MapInfo& m : kMaps) CHECK(MapAllowsFormat(m, Format::Tag) == MapAllowsFormat(m, Format::FreeForAll));
    bool browser = false; for (const std::string& n : BrowserModes()) browser = browser || n == "Tag";
    CHECK(browser);
    CHECK(ModeMatches("Tag", "Tag") && !ModeMatches("Team duel (2)", "Tag"));
}

int main() {
    TestPick(); TestRoundStart(); TestPass(); TestNoTagBack(); TestPassFloor(); TestExpiry(); TestItLeaves(); TestRotation(); TestRounds();
    TestConfig(); TestBare(); TestWire(); TestHud(); TestLobby();
    if (g_fail) { std::printf("%d check(s) failed\n", g_fail); return 1; }
    std::printf("tag tests passed\n");
    return 0;
}
