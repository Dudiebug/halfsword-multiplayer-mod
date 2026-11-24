// Unit tests for hsm_crypts.h and the Crypts gear's rules (#3199). Run via implementation/shared/run_tests.sh test_hsm_crypts.
#include "match/hsm_crypts.h"
#include "match/hsm_map_list.h"
#include "match/hsm_match_impl.h"
#include <cstdio>
#include <cstring>

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++g_fail; } } while (0)
#define CHECKF(c, i) do { if (!(c)) { std::printf("FAIL %s:%d: %s (case %d)\n", __FILE__, __LINE__, #c, int(i)); ++g_fail; } } while (0)

using namespace hsm;

static size_t MapIndex(const char* display) {
    for (size_t i = 0; i < MapCount(); ++i) if (std::strcmp(kMaps[i].display, display) == 0) return i;
    return MapCount();
}

// Crypts is a gear mode: the battle formats take it, everything else falls back to own loadouts.
static void TestGear() {
    CHECK(uint8_t(Gear::Crypts) == 5 && kGearCount == 6 && IsCrypts(Gear::Crypts) && !IsCrypts(Gear::Own));
    for (Gear g : kGearRow) CHECK(g != Gear::Crypts);                                   // no stop of the Gear row: the card sets it
    for (uint8_t i = 0; i < kFormatCount; ++i) {
        const Format f = Format(i);
        const bool battle = f == Format::LastManStanding || f == Format::FreeForAll || f == Format::TwoTeams || f == Format::ThreeTeams
                         || f == Format::TeamDuel || f == Format::Tournament || f == Format::Duel || f == Format::PlayersVsBots;
        CHECK(CryptsBattleFormat(f) == battle);
        CHECK(GearAllowed(f, Gear::Crypts) == battle);
        MatchConfig c; c.format = f; c.gear = Gear::Crypts;
        const MatchConfig s = Sanitize(c);
        if (battle && !HasRoles(f)) CHECK(s.gear == Gear::Crypts);
        if (!CryptsBattleFormat(s.format)) CHECK(s.gear != Gear::Crypts);               // Tag, Riot, Zombies, ...: not a Crypts battle (a stale Capture the flag is Teams by then)
    }
    CHECK(!GearRolled(Gear::Crypts));
}

// The Gather time: 2 to 60 whole minutes, 10 by default, clamped at both ends; the row steps through 5 to 30.
static void TestGatherTime() {
    MatchConfig c; CHECK(c.gatherS == kGatherDefaultS && kGatherDefaultS == 600);
    CHECK(kGatherMinS == 120 && kGatherMaxS == 3600);
    CHECK(kGatherOptions.front() == 300 && kGatherOptions.back() == 1800 && kGatherOptions.size() == 6);
    for (int o : kGatherOptions) CHECK(o >= kGatherMinS && o <= kGatherMaxS && o % 60 == 0);
    c.gatherS = 0; CHECK(Sanitize(c).gatherS == kGatherMinS);
    c.gatherS = 119; CHECK(Sanitize(c).gatherS == kGatherMinS);
    c.gatherS = 120; CHECK(Sanitize(c).gatherS == 120);
    c.gatherS = 300; CHECK(Sanitize(c).gatherS == 300);
    c.gatherS = 779; CHECK(Sanitize(c).gatherS == 720);                                 // whole minutes: what the pill and the row show
    c.gatherS = 3600; CHECK(Sanitize(c).gatherS == 3600);
    c.gatherS = 3601; CHECK(Sanitize(c).gatherS == kGatherMaxS);
    c.gatherS = 0xFFFF; CHECK(Sanitize(c).gatherS == kGatherMaxS);
}

// The arrows step to the next option above or below the time, wrapping; a typed time steps to its neighbours.
static void TestGatherStep() {
    CHECK(StepGather(600, 1) == 900 && StepGather(600, -1) == 300);
    CHECK(StepGather(1800, 1) == 300 && StepGather(300, -1) == 1800);                   // wraps
    CHECK(StepGather(720, 1) == 900 && StepGather(720, -1) == 600);                     // typed 12 min
    CHECK(StepGather(120, 1) == 300 && StepGather(120, -1) == 1800);                    // below the first step
    CHECK(StepGather(3600, 1) == 300 && StepGather(3600, -1) == 1800);                  // above the last
}

// What the host can type: whole minutes 2 to 60, with "min" or "m" and spaces around; nothing else.
static void TestGatherInput() {
    CHECK(ParseGatherInput("12") == std::optional<uint16_t>(720));
    CHECK(ParseGatherInput(" 7 min ") == std::optional<uint16_t>(420));
    CHECK(ParseGatherInput("45m") == std::optional<uint16_t>(2700));
    CHECK(ParseGatherInput("20 MIN") == std::optional<uint16_t>(1200));
    CHECK(ParseGatherInput("2") == std::optional<uint16_t>(120));
    CHECK(ParseGatherInput("60") == std::optional<uint16_t>(3600));
    CHECK(ParseGatherInput("02") == std::optional<uint16_t>(120));
    for (const char* bad : {"", " ", "min", "1", "0", "61", "99", "100", "600", "-5", "+5", "5.5", "5:30", "5 s", "five", "5 mins", "m5", "5 5", "002", "1:", "2/", "1 2", "123456789012"})
        CHECK(!ParseGatherInput(bad));
}

// A gather holds the Crypts at Peasant: higher tiers come back as the cap, lower ones as they are; 8 is no cap.
static void TestTierCap() {
    CHECK(kCryptsTierCap == 1 && kCryptsTierTop == 8);
    CHECK(CappedCryptsTier(0, kCryptsTierCap) == 0 && CappedCryptsTier(1, kCryptsTierCap) == 1);
    CHECK(CappedCryptsTier(2, kCryptsTierCap) == 1 && CappedCryptsTier(8, kCryptsTierCap) == 1 && CappedCryptsTier(255, kCryptsTierCap) == 1);
    for (int t = 0; t <= 8; ++t) CHECK(CappedCryptsTier(uint8_t(t), kCryptsTierTop) == t);
    CHECK(CappedCryptsTier(3, 0) == 0);
    CHECK(CryptsTierCapFrom(-1) == 0 && CryptsTierCapFrom(0) == 0 && CryptsTierCapFrom(1) == 1 && CryptsTierCapFrom(8) == 8 && CryptsTierCapFrom(99) == 8);
    // A run's cap (#3350): the lobby's Dungeon tier row, unless this machine's crypts_max_tier is set (>= 0).
    CHECK(CryptsRunCap(-1, 1) == 1 && CryptsRunCap(-1, 0) == 0 && CryptsRunCap(-1, 5) == 5 && CryptsRunCap(-1, 8) == 8 && CryptsRunCap(-1, 200) == 8);
    CHECK(CryptsRunCap(0, 5) == 0 && CryptsRunCap(8, 1) == 8 && CryptsRunCap(3, 8) == 3 && CryptsRunCap(99, 0) == 8);
    for (int t = 0; t <= 8; ++t) {                                                       // the row's stepper: a ring of nine
        CHECK(StepCryptsTier(uint8_t(t), 1) == (t + 1) % 9 && StepCryptsTier(uint8_t(t), -1) == (t + 8) % 9);
        CHECK(StepCryptsTier(StepCryptsTier(uint8_t(t), 1), -1) == t);
    }
    CHECK(StepCryptsTier(9, 1) == 0 && StepCryptsTier(200, 1) == 0 && StepCryptsTier(200, -1) == 7);   // a stale value steps from the game's own
}

// Each player's gathered kit is dealt when everybody starts (the battle, a rematch) and kept for every life after.
static void TestRegear() {
    for (Format f : {Format::LastManStanding, Format::FreeForAll, Format::TwoTeams, Format::Duel, Format::Tournament, Format::PlayersVsBots}) {
        MatchConfig c; c.format = f; c.gear = Gear::Crypts; c = Sanitize(c);
        CHECK(c.gear == Gear::Crypts);
        CHECK(RegearFor(c, RespawnCause::Rematch) == Regear::Each);
        CHECK(RegearFor(c, RespawnCause::RoundReset) == Regear::None);
        CHECK(RegearFor(c, RespawnCause::Individual) == Regear::None);
        CHECK(RegearFor(c, RespawnCause::WaveCleared) == Regear::None);
        CHECK(JoinRegearFor(c) == Regear::Each);                                        // a joiner in the battle gets a kit dealt (a peasant's)
        CHECK(StartLoadoutsAhead(c, 4) == 0);                                           // nothing is rolled
    }
}

// The maps: the Crypts modes are played on the Crypts map and nowhere else, every other mode is refused it, and the fight after a
// Crypt Royale run is on the Fight map (the Abyss for the formats that take large levels; Duel and Tournament stay in the arenas).
static void TestMaps() {
    const size_t abyss = MapIndex("Abyss"), alley = MapIndex("Alley"), tavern = MapIndex("Tavern"), crypts = CryptsMapIndex();
    CHECK(abyss < MapCount() && alley < MapCount() && tavern < MapCount() && crypts < MapCount() && kMaps[abyss].abyss && kMaps[crypts].crypts);
    for (uint8_t i = 0; i < kFormatCount; ++i) {
        const Format f = Format(i);
        for (size_t m = 0; m < MapCount(); ++m) {
            for (Gear g : {Gear::Own, Gear::RandomTier, Gear::Mirror, Gear::Fists, Gear::HostKit})
                CHECK(MapAllowsMatch(kMaps[m], f, g) == MapAllowsFormat(kMaps[m], f));   // the other gears keep the format's maps
            CHECK(!kMaps[m].crypts || !MapAllowsFormat(kMaps[m], f));                    // the Crypts map is no format's
            CHECK(MapAllowsMatch(kMaps[m], f, Gear::Crypts) == (kMaps[m].crypts && CryptsBattleFormat(f)));   // Crypts gear: that map alone
            CHECK(!MapAllowsMatch(kMaps[m], f, Gear::Crypts, true));                     // never the Blossfechten card
        }
    }
    for (Format f : {Format::LastManStanding, Format::Duel, Format::FreeForAll, Format::TwoTeams, Format::ThreeTeams, Format::TeamDuel, Format::PlayersVsBots}) {   // Duel is the Last man standing card with two players (0.9.0)
        CHECK(!MapAllowsFormat(kMaps[abyss], f) && !MapAllowsMatch(kMaps[abyss], f, Gear::Crypts) && MapAllowsMatch(kMaps[crypts], f, Gear::Crypts));
        CHECK(MapAllowsFight(kMaps[abyss], f) && MapAllowsFight(kMaps[alley], f) && !MapAllowsFight(kMaps[crypts], f) && !kMaps[tavern].pick);
        CHECK(CryptsDefaultMap(f) == abyss);                                             // the Fight map row's default
        CHECK(EffectiveFightMap(f, "") == abyss && EffectiveFightMap(f, "/Game/Maps/Abyss/Abyss_Map_Open_EA") == abyss);
        CHECK(EffectiveFightMap(f, kMaps[alley].path) == alley);                         // the host may pick an arena
        CHECK(EffectiveFightMap(f, kMaps[tavern].path) == abyss && EffectiveFightMap(f, kMaps[crypts].path) == abyss);   // never a menu-only map or the Crypts
        CHECK(EffectiveMap(f, Gear::Crypts, "") == crypts && EffectiveMap(f, Gear::Own, "/Game/Maps/Abyss/Abyss_Map_Open_EA") != abyss);
    }
    for (Format f : {Format::Tournament}) {
        CHECK(!MapAllowsFight(kMaps[abyss], f));
        CHECK(CryptsDefaultMap(f) < MapCount() && kMaps[CryptsDefaultMap(f)].arena && kMaps[CryptsDefaultMap(f)].pick);
        CHECK(EffectiveFightMap(f, "/Game/Maps/Abyss/Abyss_Map_Open_EA") != abyss);
    }
    CHECK(!MapAllowsMatch(kMaps[crypts], Format::Tag, Gear::Crypts) && !MapAllowsMatch(kMaps[crypts], Format::Zombies, Gear::Crypts));   // not battle formats
    CHECK(NextMap(alley, Format::LastManStanding, Gear::Crypts) == crypts && NextMap(crypts, Format::LastManStanding, Gear::Crypts) == crypts);
}

static void TestGatherFlow() {
    CryptsGather g;
    CHECK(!g.Active() && g.phase() == GatherPhase::Off && g.LeftMs(0) == 0 && g.Update(0) == GatherStep::None);
    g.Start(1000, 300, {0, 1, 2});
    CHECK(g.Active() && g.phase() == GatherPhase::Gathering && g.seq() == 1 && g.Players() == 3 && g.Kits() == 0 && g.StillGathering() == 3);
    CHECK(g.LeftMs(1000) == 300000 && g.LeftMs(301000) == 0 && g.LeftMs(300999) == 1);
    CHECK(g.Update(2000) == GatherStep::None);
    CHECK(!g.TakesKit(1)); g.OnKit(1); CHECK(!g.HasKit(1));                              // before the run: a CHANGE LOADOUT on the old level
    g.OnNote(1, GatherNote::InCrypts); g.OnNote(2, GatherNote::InCrypts);
    CHECK(g.TakesKit(1) && !g.TakesKit(0));
    g.OnKit(1); CHECK(g.HasKit(1) && !g.HasKit(0) && g.Kits() == 1);
    g.OnKit(9); CHECK(!g.In(9) && !g.TakesKit(9) && g.Kits() == 1);                      // not in the gather: nothing
    g.OnNote(0, GatherNote::Loading); CHECK(!g.TakesKit(0));                             // still loading: not in the run yet
    g.OnNote(0, GatherNote::InCrypts); CHECK(!g.Done(0) && g.TakesKit(0));
    g.OnNote(0, GatherNote::Died); CHECK(g.Done(0) && g.StillGathering() == 2);
    CHECK(!g.TakesKit(0)); g.OnKit(0); CHECK(!g.HasKit(0) && g.Kits() == 1);             // after a death: the last kit sent alive stays
    CHECK(g.Update(3000) == GatherStep::None);
    // The clock runs out: collect the last kits.
    CHECK(g.Update(301000) == GatherStep::Collect && g.phase() == GatherPhase::Collecting);
    CHECK(g.LeftMs(301000) == kGatherCollectMs);
    CHECK(g.Update(302000) == GatherStep::None);                                        // 1 and 2 have not answered
    g.OnKit(1); CHECK(g.Update(303000) == GatherStep::None);                             // 2 still out
    g.OnNote(2, GatherNote::Extracted); CHECK(g.Update(304000) == GatherStep::Battle);   // everyone done or a final kit in
    CHECK(!g.Active() && g.LeftMs(304000) == 0 && g.Update(305000) == GatherStep::None);
    CHECK(!g.TakesKit(1)); g.OnKit(1);                                                   // the battle is on: no more kits
    CHECK(g.Players() == 3 && g.HasKit(1));                                               // the record lasts until the next Start
}

static void TestGatherEarlyAndTimeout() {
    CryptsGather g;
    g.Start(0, 1800, {0, 1});
    CHECK(g.LeftMs(0) == 1800000);
    g.OnNote(0, GatherNote::Extracted); CHECK(g.Update(10) == GatherStep::None);
    g.OnNote(1, GatherNote::Waiting); CHECK(g.Update(20) == GatherStep::Collect);       // nobody still in the Crypts: no waiting out the clock
    CHECK(g.Update(30) == GatherStep::Battle);                                           // and everyone is done already
    // Collecting times out without the last kits.
    g.Start(0, 300, {0, 1});
    CHECK(g.seq() == 2);
    CHECK(g.Update(300000) == GatherStep::Collect);
    CHECK(g.Update(300000 + kGatherCollectMs - 1) == GatherStep::None);
    CHECK(g.Update(300000 + kGatherCollectMs) == GatherStep::Battle);
    // A kit that came during the gather is no final one.
    g.Start(0, 300, {0});
    g.OnNote(0, GatherNote::InCrypts);
    g.OnKit(0); CHECK(g.Update(300000) == GatherStep::Collect && g.Update(300001) == GatherStep::None);
    g.OnKit(0); CHECK(g.Update(300002) == GatherStep::Battle);
    // Stop calls it off.
    g.Start(0, 300, {0}); g.Stop(); CHECK(!g.Active() && g.Players() == 0 && g.Update(1) == GatherStep::None);
    g.OnKit(0); g.OnNote(0, GatherNote::Died); CHECK(!g.HasKit(0) && !g.Done(0));
    // The gather time is clamped.
    g.Start(0, 10, {0}); CHECK(g.LeftMs(0) == uint32_t(kGatherMinS) * 1000u);
    g.Start(0, 60000, {0}); CHECK(g.LeftMs(0) == uint32_t(kGatherMaxS) * 1000u);
}

// Together: the gather knows it; CollectNow ends the gathering early (every player down), then the last kits as at the clock.
static void TestGatherTogether() {
    CryptsGather g;
    CHECK(!g.together() && !g.CollectNow(0) && g.phase() == GatherPhase::Off);           // no gather: nothing to collect
    g.Start(1000, 600, {0, 1}, true);
    CHECK(g.together() && g.phase() == GatherPhase::Gathering);
    g.OnNote(0, GatherNote::InCrypts); g.OnNote(1, GatherNote::InCrypts);
    g.OnKit(0); CHECK(g.HasKit(0) && g.Update(5000) == GatherStep::None);
    CHECK(g.CollectNow(60000) && g.phase() == GatherPhase::Collecting && g.LeftMs(60000) == kGatherCollectMs);
    CHECK(!g.CollectNow(61000) && g.LeftMs(61000) == kGatherCollectMs - 1000);           // once: the collect clock does not restart
    CHECK(g.Update(61000) == GatherStep::None);                                          // the last kits: none yet
    CHECK(g.TakesKit(1)); g.OnKit(1); CHECK(g.Update(62000) == GatherStep::None);        // 1's final kit; 0 has not sent one since
    g.OnKit(0); CHECK(g.Update(63000) == GatherStep::Battle && !g.Active() && !g.CollectNow(63000));
    // Timed out: the battle at kGatherCollectMs after CollectNow.
    g.Start(0, 600, {0}, true); g.OnNote(0, GatherNote::InCrypts);
    CHECK(g.CollectNow(1000) && g.Update(1000 + kGatherCollectMs - 1) == GatherStep::None && g.Update(1000 + kGatherCollectMs) == GatherStep::Battle);
    // The next gather is Apart unless it says so.
    g.Start(0, 600, {0}); CHECK(!g.together());
    g.Start(0, 600, {0}, true); g.Stop(); CHECK(!g.CollectNow(1));
    GatherClock c; c.Set(0, GatherPhase::Gathering, 60000, 3, true); CHECK(c.together() && c.Active());
    c.Set(0, GatherPhase::Gathering, 60000, 4); CHECK(!c.together());
    c.Set(0, GatherPhase::Gathering, 60000, 5, true); c.Clear(); CHECK(!c.together() && !c.Active());
}

// Together's match: Players vs Bots in Crypts gear, whatever the battle's format; the rest of the lobby goes on.
static void TestTogetherRunConfig() {
    for (Format f : {Format::FreeForAll, Format::TwoTeams, Format::Duel, Format::Tournament, Format::LastManStanding, Format::PlayersVsBots}) {
        MatchConfig c; c.format = f; c.gear = Gear::Crypts; c.scoreLimit = 7; c.timeLimitS = 600; c.friendlyFire = true;
        c.shuffle = TeamShuffle::EachMatch; c.tier = 3; c.weapons = 5; c.gatherS = 900; c.cryptsTogether = true; c.respawnDelayS = 9;
        const MatchConfig t = TogetherRunConfig(c);
        CHECKF(t.format == Format::PlayersVsBots && t.gear == Gear::Crypts && t.scoreLimit == DefaultScoreLimit(Format::PlayersVsBots), int(f));
        CHECKF(t.timeLimitS == 0 && !t.friendlyFire && t.shuffle == TeamShuffle::Off, int(f));
        CHECKF(t.tier == 3 && t.weapons == 5 && t.gatherS == 900 && t.cryptsTogether && t.respawnDelayS == 9, int(f));
        g_cryptsCoopOffered = true;                                                      // Together is held back until [mod] crypts_coop=1
        const MatchConfig s = Sanitize(t);                                               // as StartMatch deals it
        g_cryptsCoopOffered = false;
        CHECKF(s.format == Format::PlayersVsBots && s.gear == Gear::Crypts && s.cryptsTogether && s.timeLimitS == 0 && !s.friendlyFire, int(f));
    }
    MatchConfig own; own.gear = Gear::Own;                                               // any gear: the run is Crypts
    CHECK(TogetherRunConfig(own).gear == Gear::Crypts);
}

static void TestGatherJoinLeave() {
    CryptsGather g;
    CHECK(!g.Join(3, 0));                                                                // no gather
    g.Start(0, 300, {0});
    CHECK(g.Join(3, 0) && g.In(3) && !g.Done(3) && g.StillGathering() == 2);           // plenty of time: opens a run
    CHECK(g.Join(4, 300000 - kGatherJoinMinMs) && !g.Done(4));                          // exactly a minute left
    CHECK(!g.Join(5, 300000 - kGatherJoinMinMs + 1) && g.In(5) && g.Done(5));            // under a minute: waits in the menu
    g.OnNote(5, GatherNote::InCrypts); g.OnKit(5); CHECK(!g.TakesKit(5) && !g.HasKit(5));  // and sends no kit that counts
    g.OnNote(3, GatherNote::Died); CHECK(!g.Join(3, 1000) && g.Done(3));                 // back after dying: no second run
    g.Leave(4); CHECK(!g.In(4));
    g.OnKit(4); CHECK(!g.HasKit(4));
    g.Leave(0);
    CHECK(g.Update(1000) == GatherStep::Collect);                                        // 3 and 5 are done, 0 and 4 left
    CHECK(!g.Join(6, 1000) && g.Done(6));                                                // collecting: too late
    CHECK(g.Update(1000) == GatherStep::Battle);
    // Seq never wraps to 0.
    CryptsGather w; for (int i = 0; i < 70000; ++i) { w.Start(0, 300, {}); CHECK(w.seq() != 0); }
}

static void TestClock() {
    GatherClock c;
    CHECK(!c.Active() && c.LeftMs(5) == 0);
    c.Set(1000, GatherPhase::Gathering, 600000, 7);
    CHECK(c.Active() && c.seq() == 7 && c.phase() == GatherPhase::Gathering);
    CHECK(c.LeftMs(1000) == 600000 && c.LeftMs(11000) == 590000 && c.LeftMs(601000) == 0 && c.LeftMs(900000) == 0);
    c.Set(0xFFFFFF00u, GatherPhase::Collecting, 1000, 7); CHECK(c.LeftMs(0x100) == 1000 - 0x200);   // across the tick's wrap
    c.Clear(); CHECK(!c.Active() && c.LeftMs(0x100) == 0 && c.seq() == 7);
    c.Set(0, GatherPhase::Off, 5000, 8); CHECK(!c.Active() && c.LeftMs(0) == 0);
}

static void TestSnapshots() {
    CHECK(KitSnapshotDue(GatherSelf::Gathering, true, 0, 0, true));
    CHECK(!KitSnapshotDue(GatherSelf::Gathering, true, kKitSnapshotMs - 1, 0, false));
    CHECK(KitSnapshotDue(GatherSelf::Gathering, true, kKitSnapshotMs, 0, false));
    CHECK(!KitSnapshotDue(GatherSelf::Gathering, false, kKitSnapshotMs, 0, true));       // dead: the last kit read alive stands
    for (GatherSelf s : {GatherSelf::None, GatherSelf::Opening, GatherSelf::Died, GatherSelf::Extracted, GatherSelf::Waiting})
        CHECK(!KitSnapshotDue(s, true, kKitSnapshotMs, 0, true));
    CHECK(!KitSnapshotDue(GatherSelf::Gathering, true, 5, 0xFFFFFFF0u, false));           // 21 ms across the tick's wrap
    CHECK(KitSnapshotDue(GatherSelf::Gathering, true, kKitSnapshotMs, 0xFFFFFFF0u, false));
    const std::vector<uint8_t> a = {1, 2}, b = {1, 3}, none;
    CHECK(KitUploadDue(a, none) && KitUploadDue(a, b) && !KitUploadDue(a, a) && !KitUploadDue(none, a) && !KitUploadDue(none, none));
}

static void TestNotes() {
    CHECK(!ValidGatherNote(0) && ValidGatherNote(1) && ValidGatherNote(5) && !ValidGatherNote(6) && !ValidGatherNote(255));
    CHECK(!GatherNoteEnds(GatherNote::Loading) && !GatherNoteEnds(GatherNote::InCrypts));
    CHECK(GatherNoteEnds(GatherNote::Died) && GatherNoteEnds(GatherNote::Extracted) && GatherNoteEnds(GatherNote::Waiting));
    for (uint8_t n = 1; n <= 5; ++n) CHECK(std::strcmp(GatherNoteLog(GatherNote(n)), "?") != 0);
    CHECK(std::strcmp(GatherNoteLog(GatherNote::Died), "died - kit kept") == 0);
    CHECK(std::strcmp(GatherNoteLog(GatherNote::Extracted), "exit reached - kit locked") == 0);
    CHECK(kGatherPhaseCount == 3 && uint8_t(GatherPhase::Collecting) == 2);
}

int main() {
    TestGear();
    TestGatherTime();
    TestGatherStep();
    TestGatherInput();
    TestTierCap();
    TestRegear();
    TestMaps();
    TestGatherFlow();
    TestGatherEarlyAndTimeout();
    TestGatherTogether();
    TestTogetherRunConfig();
    TestGatherJoinLeave();
    TestClock();
    TestSnapshots();
    TestNotes();
    if (g_fail) { std::printf("test_hsm_crypts: %d failure(s)\n", g_fail); return 1; }
    std::printf("test_hsm_crypts: ok\n");
    return 0;
}
