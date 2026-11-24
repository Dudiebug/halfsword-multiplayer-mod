// The lobby's merged modes (0.9.0), as pure rules, without a wire change (the protocol revision stays 48):
//   Duel and Last man standing are ONE card. The lobby holds Last man standing; both ids stay on the wire and the match picks
//   one when it starts, by the player count (FormatForPlayers, hsm_match.h). The card, the lobby list, the mode filter and the
//   summary line read the same rule: two players read "Duel", any other count "Last man standing" (DuelCardName).
//   Blossfechten is a card: Last man standing with the Blossfechten scenario value (IsBlossfechten, hsm_match.h).
//   Pit bridge duels, the Fenced ring and Carnage scenarios and the Free camera row are gone from the lobby. A lobby of an older
//   host can still carry them: LobbyConfigOf reads them as the nearest mode that is still there, and BrowserModeFor does the same
//   for the browser's listing.
// Game-independent and unit-tested (test_hsm_mode_merge.cpp). The lobby page (hsm_lobby_scene.h) and the session read these.
#pragma once
#include "match/hsm_hud_text.h"
#include "match/hsm_match.h"
#include <cstdint>
#include <string>

namespace hsm {

inline constexpr const char* kDuelName = "Duel";
inline constexpr const char* kLastManStandingName = "Last man standing";
inline constexpr const char* kBlossfechtenName = "Blossfechten";
inline constexpr const char* kBrawlName = "Brawl";
inline constexpr const char* kCarnageName = "Carnage";
inline constexpr const char* kCryptRoyaleName = "Crypt Royale";
inline constexpr const char* kGraveRobbersName = "Grave Robbers";
inline constexpr const char* kDuelCardChip = "Duel or last man standing";   // the browser's mode chip stop for the merged card

// Whether the merged card reads Duel: exactly two players, or a lobby that cannot hold more than two and has fewer so far (a
// duel lobby waiting for its opponent). `maxPlayers` 0: not known (the card on the lobby page reads the players alone).
inline bool ReadsDuel(int players, int maxPlayers = 0) {
    if (players == 2) return true;
    return maxPlayers >= 1 && maxPlayers <= 2 && players >= 0 && players < 2;
}
inline const char* DuelCardName(int players, int maxPlayers = 0) { return ReadsDuel(players, maxPlayers) ? kDuelName : kLastManStandingName; }

// What a match is called to the players, with the merged cards: ModeName, but Blossfechten by its own name, and the Duel card
// by the player count ("Crypt Royale (Duel)" where its battle is a Duel), and Grave Robbers by its own name.
inline std::string ModeNameFor(const MatchConfig& c, int players, int maxPlayers = 0) {
    if (IsGraveRobbers(c)) return kGraveRobbersName;
    if (IsBlossfechten(c)) return kBlossfechtenName;
    const Format shown = IsDuelCard(c.format) ? (ReadsDuel(players, maxPlayers) ? Format::Duel : Format::LastManStanding) : c.format;
    return ModeName(shown, c.gear, c.teams);
}

// What a lobby of an older host can carry that this build's lobby no longer has, read as the nearest mode that is: Duel is the
// Last man standing card (the match decides again at the start), Pit bridge duels a free-for-all (Carnage), and the retired
// Fenced ring and Carnage scenarios are Standard. The Free camera key is not read at all (MatchConfig::freeCam is always on,
// Sanitize). Lobby::config() runs this, then Sanitize.
inline MatchConfig LobbyConfigOf(MatchConfig c) {
    if (c.format == Format::Duel) c.format = Format::LastManStanding;
    if (c.format == Format::PitBridgeDuel) { c.format = Format::FreeForAll; c.scoreLimit = DefaultScoreLimit(Format::FreeForAll); }
    if (c.scenario == uint8_t(Scenario::FencedRing) || c.scenario == uint8_t(Scenario::Carnage)) c.scenario = uint8_t(Scenario::Standard);
    c.freeCam = true;
    return c;
}

// The mode a listed lobby is shown as, and filtered by: the text the host advertised (the mode name of its last level change),
// read with what the lobby data says now. `scenario`: the lobby key hsm_scen. `players`, `maxPlayers`: the list row's.
//   Free-for-all (an older host's name for Carnage) and Pit bridge duels are Carnage. An older host's Brawling scenario on a
//   free-for-all is Brawl; its Blossfechten scenario on Carnage, Duel or Last man standing is Blossfechten.
//   Duel and Last man standing are one: the player count says which, as the card does.
//   An older host's "Crypts (Duel)" is Crypt Royale (Duel): its Crypts card became the Crypt Royale mode.
inline std::string BrowserModeFor(const std::string& advertised, uint8_t scenario, int players, int maxPlayers) {
    if (advertised.compare(0, 8, "Crypts (") == 0) return std::string(kCryptRoyaleName) + advertised.substr(6);
    const bool ffa = advertised == "Free-for-all" || advertised == kCarnageName || advertised == "Pit bridge duels";
    const bool duelCard = advertised == kDuelName || advertised == kLastManStandingName;
    if ((ffa || duelCard) && scenario == uint8_t(Scenario::Blossfechten)) return kBlossfechtenName;
    if (ffa) return scenario == uint8_t(Scenario::Brawling) ? kBrawlName : kCarnageName;
    if (duelCard) return DuelCardName(players, maxPlayers);
    return advertised;
}

// A listed mode against a mode chip stop: equal, or the stop plus the detail the host adds ("Teams (3)" is "Teams"). The merged
// stop takes the card's two names.
inline bool BrowserModeMatches(const std::string& shown, const std::string& stop) {
    if (stop == kDuelCardChip) return shown == kDuelName || shown == kLastManStandingName;
    return shown == stop || (shown.size() > stop.size() && shown.compare(0, stop.size(), stop) == 0 && shown.compare(stop.size(), 2, " (") == 0);
}

}  // namespace hsm
