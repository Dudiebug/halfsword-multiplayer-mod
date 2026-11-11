// Regions for the lobby browser. A host's lobby says where it is (lobby key hsm_region); the browser can show one region's
// lobbies, and asks Steam for lobbies worldwide when it does. The player's own region is picked on the Multiplayer page or
// guessed from the Steam relay data centres: the one with the lowest direct ping says where the player is. No wire change:
// lobby keys only. Game-independent and unit-tested.
#pragma once
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace hsm {

// Auto is no region: as a pick it means "guess it", as a lobby's or a guess's region it means "not known".
enum class Region : uint8_t { Auto, NaEast, NaWest, SouthAmerica, Europe, Asia, Oceania };
constexpr size_t kRegionCount = 7;   // Auto and the six regions
inline bool RegionKnown(Region r) { return r != Region::Auto; }

inline const char* RegionLabel(Region r) {
    switch (r) {
        case Region::NaEast: return "NA East";
        case Region::NaWest: return "NA West";
        case Region::SouthAmerica: return "South America";
        case Region::Europe: return "Europe";
        case Region::Asia: return "Asia";
        case Region::Oceania: return "Oceania";
        default: return "Auto";
    }
}
// The word in HSMMod.ini (region=) and in the lobby key hsm_region.
inline const char* RegionKey(Region r) {
    switch (r) {
        case Region::NaEast: return "na-east";
        case Region::NaWest: return "na-west";
        case Region::SouthAmerica: return "south-america";
        case Region::Europe: return "europe";
        case Region::Asia: return "asia";
        case Region::Oceania: return "oceania";
        default: return "auto";
    }
}
// A key as written by RegionKey, in any case. False for anything else (an empty key, a word from a later version): out stays.
inline bool ParseRegion(std::string_view key, Region& out) {
    std::string k;
    for (char c : key) k += char(std::tolower(static_cast<unsigned char>(c)));
    for (size_t i = 0; i < kRegionCount; ++i)
        if (k == RegionKey(Region(i))) { out = Region(i); return true; }
    return false;
}
// What a lobby row shows: the host's region, or Auto when it did not say (an older build, a host that could not tell).
inline Region LobbyRegion(std::string_view key) {
    Region r = Region::Auto;
    ParseRegion(key, r);
    return r;
}
// The picker steps through Auto and the six regions, then back to Auto.
inline Region NextRegion(Region r) { return Region((size_t(r) + 1) % kRegionCount); }

// The region in use: the pick, or, on Auto, what the guess found (Auto when it found nothing).
inline Region EffectiveRegion(Region pick, Region guessed) { return pick != Region::Auto ? pick : guessed; }
// The picker's button text: "Auto (Europe)", "Auto (unknown)", or the region picked.
inline std::string RegionPickLabel(Region pick, Region guessed) {
    if (pick != Region::Auto) return RegionLabel(pick);
    return std::string("Auto (") + (RegionKnown(guessed) ? RegionLabel(guessed) : "unknown") + ")";
}

// ---------------------------------------------------------------- the guess
// Steam's data centres ("POPs") have three-letter codes, sometimes with a number ("sgp1", "sto2"). The table covers the
// ones Valve runs; a code it does not list (Africa, an airport added later) is no region, and no region is no guess.
inline Region RegionOfPop(std::string_view pop) {
    if (pop.size() < 3) return Region::Auto;
    char c[4] = {char(std::tolower(static_cast<unsigned char>(pop[0]))), char(std::tolower(static_cast<unsigned char>(pop[1]))),
                 char(std::tolower(static_cast<unsigned char>(pop[2]))), 0};
    struct Row { const char* code; Region region; };
    static constexpr Row kPops[] = {
        // North America: the middle of the country counts as the east
        {"iad", Region::NaEast}, {"atl", Region::NaEast}, {"ord", Region::NaEast}, {"dfw", Region::NaEast}, {"mia", Region::NaEast},
        {"ewr", Region::NaEast}, {"jfk", Region::NaEast}, {"bos", Region::NaEast}, {"yyz", Region::NaEast}, {"ymq", Region::NaEast},
        {"lax", Region::NaWest}, {"sea", Region::NaWest}, {"eat", Region::NaWest}, {"sjc", Region::NaWest}, {"sfo", Region::NaWest},
        {"pdx", Region::NaWest}, {"den", Region::NaWest}, {"phx", Region::NaWest}, {"yvr", Region::NaWest},
        {"gru", Region::SouthAmerica}, {"sao", Region::SouthAmerica}, {"scl", Region::SouthAmerica}, {"lim", Region::SouthAmerica},
        {"eze", Region::SouthAmerica}, {"bog", Region::SouthAmerica},
        {"ams", Region::Europe}, {"fra", Region::Europe}, {"lhr", Region::Europe}, {"lon", Region::Europe}, {"par", Region::Europe},
        {"cdg", Region::Europe}, {"mad", Region::Europe}, {"sto", Region::Europe}, {"arn", Region::Europe}, {"vie", Region::Europe},
        {"waw", Region::Europe}, {"hel", Region::Europe}, {"mil", Region::Europe}, {"mxp", Region::Europe}, {"lux", Region::Europe},
        {"dub", Region::Europe}, {"zrh", Region::Europe}, {"cph", Region::Europe}, {"osl", Region::Europe}, {"prg", Region::Europe},
        {"otp", Region::Europe}, {"ist", Region::Europe},
        {"sgp", Region::Asia}, {"sin", Region::Asia}, {"hkg", Region::Asia}, {"tyo", Region::Asia}, {"nrt", Region::Asia},
        {"sha", Region::Asia}, {"pvg", Region::Asia}, {"pek", Region::Asia}, {"can", Region::Asia}, {"ctu", Region::Asia},
        {"tsn", Region::Asia}, {"seo", Region::Asia}, {"icn", Region::Asia}, {"bom", Region::Asia}, {"maa", Region::Asia},
        {"del", Region::Asia}, {"blr", Region::Asia}, {"dxb", Region::Asia}, {"bkk", Region::Asia}, {"kul", Region::Asia},
        {"mnl", Region::Asia}, {"tpe", Region::Asia}, {"osa", Region::Asia}, {"kix", Region::Asia}, {"cgk", Region::Asia},
        {"syd", Region::Oceania}, {"mel", Region::Oceania}, {"per", Region::Oceania}, {"akl", Region::Oceania}, {"bne", Region::Oceania},
    };
    for (const Row& r : kPops) if (std::string_view(r.code) == c) return r.region;
    return Region::Auto;
}

struct PopPing { std::string pop; int pingMs = -1; };   // a data centre and the direct ping to it (-1: no data)
struct RegionGuess { Region region = Region::Auto; std::string pop; int pingMs = -1; };

constexpr int kRegionMaxPingMs = 90;     // the nearest data centre farther than this says nothing about where the player is
constexpr int kRegionMarginMs = 8;       // a data centre of another region this close behind the nearest: too near to call

// The region of the data centre with the lowest direct ping, when that is clear: its ping is within kRegionMaxPingMs, its code
// is in the table, and no data centre of another region (or one the table does not list) is within kRegionMarginMs of it.
// Otherwise Auto: "Auto (unknown)" on the page, and the browser filters nothing by it.
inline RegionGuess GuessRegion(const std::vector<PopPing>& pops) {
    const PopPing* best = nullptr;
    for (const PopPing& p : pops) if (p.pingMs >= 0 && (!best || p.pingMs < best->pingMs)) best = &p;
    if (!best || best->pingMs > kRegionMaxPingMs) return {};
    const Region region = RegionOfPop(best->pop);
    if (!RegionKnown(region)) return {};
    for (const PopPing& p : pops)
        if (&p != best && p.pingMs >= 0 && p.pingMs - best->pingMs < kRegionMarginMs && RegionOfPop(p.pop) != region) return {};
    return {region, best->pop, best->pingMs};
}

// ---------------------------------------------------------------- how far Steam looks
// ELobbyDistanceFilter (ISteamMatchmaking::AddRequestLobbyListDistanceFilter). Without a call Steam uses Default.
enum class LobbyDistance : int { Close = 0, Default = 1, Far = 2, Worldwide = 3 };

}  // namespace hsm
