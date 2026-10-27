// role=server auto flow: map list parsing and every state transition.
#include "session/hsm_autoflow.h"
#include <cstdio>

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++g_fail; } } while (0)
using namespace hsm;
using K = FlowAction::Kind;

static size_t Idx(const char* shortName) { return size_t(FindMap(shortName) - kMaps); }

int main() {
    Format f{};
    CHECK(ParseServerFormat("FFA", &f) && f == Format::FreeForAll);
    CHECK(ParseServerFormat("teams", &f) && f == Format::TwoTeams);
    CHECK(ParseServerFormat("survival", &f) && f == Format::PlayersVsBots);
    CHECK(ParseServerFormat("zombies", &f) && f == Format::Zombies);
    CHECK(ParseServerFormat("duel", &f) && f == Format::Duel);
    CHECK(!ParseServerFormat("bogus", &f));

    // Map list parsing. "all" means every map the picker offers for the format (no Tavern, Cutting, Smithery).
    size_t ffaMaps = 0;
    for (size_t i = 0; i < MapCount(); ++i) if (MapAllowsFormat(kMaps[i], Format::FreeForAll)) ++ffaMaps;
    CHECK(ParseServerMaps("all", Format::FreeForAll).size() == ffaMaps);
    CHECK(ParseServerMaps("", Format::FreeForAll).size() == ffaMaps);
    auto z = ParseServerMaps("all", Format::Zombies);
    CHECK(z.size() == 1 && z[0] == Idx("Abyss_Map_Open_EA"));
    CHECK(ParseServerMaps("all", Format::Duel).size() == 8);                             // arenas only
    std::vector<std::string> unk;
    auto m = ParseServerMaps(" yard , EAST TOWER,Map_Arena_Cellar, nope, yard, Tavern", Format::FreeForAll, &unk);
    CHECK(m.size() == 4 && m[0] == Idx("Map_Arena_Yard") && m[1] == Idx("Map_Arena_EastTower") && m[2] == Idx("Map_Arena_Cellar"));
    CHECK(unk.size() == 1 && unk[0] == "nope");
    auto d = ParseServerMaps("Yard,Tavern,Abyss", Format::Duel);                         // the big maps are refused for Duel
    CHECK(d.size() == 1 && d[0] == Idx("Map_Arena_Yard"));
    CHECK(ParseServerMaps("Yard", Format::Zombies).size() == 1);                         // nothing usable: every allowed map
    CHECK(ParseServerMaps("Yard", Format::Zombies)[0] == Idx("Abyss_Map_Open_EA"));
    CHECK(ParseServerMaps("garbage", Format::FreeForAll, &unk).size() == ffaMaps);

    AutoFlowConfig c; c.minPlayers = 2; c.countdownMs = 30000; c.announceMs = 10000; c.emptyGraceMs = 10000; c.travelRetryMs = 20000;
    const size_t A = Idx("Map_Arena_Alley"), B = Idx("Map_Arena_Cellar");
    AutoFlow fl(c, {A, B});
    auto T = [&](uint32_t t, int clients, FlowPhase p = FlowPhase::None) { return fl.Tick({t, clients, p}); };

    CHECK(T(0, 0).kind == K::None && T(1000, 1).kind == K::None);                       // below the minimum: idle
    FlowAction a = T(2000, 2);                                                           // countdown starts
    CHECK(a.kind == K::Announce && a.secondsLeft == 30 && fl.state() == AutoFlow::State::Waiting);
    CHECK(T(5000, 2).kind == K::None);
    a = T(12000, 2); CHECK(a.kind == K::Announce && a.secondsLeft == 20);                // cadence 10 s
    CHECK(T(13000, 2).kind == K::None);
    CHECK(T(20000, 1).kind == K::None && fl.state() == AutoFlow::State::Idle);           // a leaver resets the countdown
    a = T(21000, 3); CHECK(a.kind == K::Announce && a.secondsLeft == 30);                // restarts from the full length
    a = T(31000, 3); CHECK(a.kind == K::Announce && a.secondsLeft == 20);
    a = T(41000, 3); CHECK(a.kind == K::Announce && a.secondsLeft == 10);
    a = T(51000, 3); CHECK(a.kind == K::Travel && a.map == A && fl.state() == AutoFlow::State::Traveling);
    CHECK(T(52000, 3).kind == K::None && T(60000, 3, FlowPhase::Results).kind == K::None);  // a stale results phase is not the new match
    CHECK(fl.state() == AutoFlow::State::Traveling);
    a = T(71000, 3); CHECK(a.kind == K::Travel && a.map == A);                           // no match after 20 s: ask again (same map)
    CHECK(T(75000, 3, FlowPhase::Active).kind == K::None && fl.state() == AutoFlow::State::InMatch);
    CHECK(T(80000, 3, FlowPhase::Active).kind == K::None);
    CHECK(T(90000, 3, FlowPhase::Results).kind == K::None && fl.state() == AutoFlow::State::Results);
    CHECK(T(95000, 3, FlowPhase::Results).kind == K::None);                              // still holding
    fl.ResultsOver();
    a = T(95100, 3, FlowPhase::Results); CHECK(a.kind == K::Travel && a.map == B);       // rotation: next map
    T(97000, 3, FlowPhase::Active); T(98000, 3, FlowPhase::Results);
    a = T(113001, 3, FlowPhase::Results); CHECK(a.kind == K::Travel && a.map == A);      // the hold timer alone also advances; the list wraps

    // Everyone leaves: grace period, then the menu, once.
    T(114000, 3, FlowPhase::Active);
    CHECK(T(120000, 0).kind == K::None);
    CHECK(T(125000, 0).kind == K::None);
    CHECK(T(130000, 0).kind == K::ToMenu && fl.state() == AutoFlow::State::Idle);
    CHECK(T(140000, 0).kind == K::None);
    a = T(150000, 2); CHECK(a.kind == K::Announce);                                      // someone returns: a fresh countdown
    CHECK(T(160000, 0).kind == K::None && fl.state() == AutoFlow::State::Idle);          // left during the countdown: no menu trip needed
    CHECK(T(175000, 0).kind == K::None);

    // Zero countdown, a single player, an empty rotation.
    AutoFlowConfig q; q.minPlayers = 1; q.countdownMs = 0;
    AutoFlow quick(q, {B});
    a = quick.Tick({0, 1, FlowPhase::None}); CHECK(a.kind == K::Travel && a.map == B);
    AutoFlow none(c, {});
    CHECK(none.Tick({0, 5, FlowPhase::None}).kind == K::None);
    if (!g_fail) std::printf("test_hsm_autoflow: ok\n");
    return g_fail ? 1 : 0;
}
