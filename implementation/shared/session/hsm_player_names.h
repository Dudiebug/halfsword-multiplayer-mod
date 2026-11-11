// Names a player picks (a nickname, a lobby's name) and how another player's name is shown on this screen. The wire is
// unchanged: a nickname rides Hello.name (kMaxPlayerName bytes) and the lobby member data; the lobby name is the Steam lobby
// key "name". Receivers clean what they read (CleanPeerName), so a modified client cannot send control characters, emoji or
// half a character. Game-independent and unit-tested.
#pragma once
#include "hsm_chat.h"
#include "hsm_protocol.h"
#include "match/hsm_name_filter.h"
#include "session/hsm_roster.h"
#include <cstddef>
#include <cstdint>
#include <string>

namespace hsm {

constexpr size_t kMaxNicknameChars = 24;     // the nickname box: characters, not bytes (kMaxPlayerName still caps the bytes on the wire)
constexpr size_t kMaxLobbyNameChars = 32;    // the lobby name box
constexpr size_t kMaxLobbyNameBytes = 128;   // 32 characters of up to 4 bytes

namespace names_detail {
// `s` (valid UTF-8, as SanitizeChat leaves it) cut to at most `chars` code points and `bytes` bytes, on a code-point
// boundary, with the spaces the cut leaves at the end dropped.
inline std::string Cut(const std::string& s, size_t chars, size_t bytes) {
    size_t at = 0, n = 0;
    while (at < s.size() && n < chars) {
        size_t len = 0; uint32_t cp = 0;
        if (!ChatUtf8Next(s, at, len, cp) || at + len > bytes) break;
        at += len;
        ++n;
    }
    std::string out = s.substr(0, at);
    while (!out.empty() && out.back() == ' ') out.pop_back();
    return out;
}
}  // namespace names_detail

// A typed nickname made fit to send: control characters, emoji and bad UTF-8 out, spaces trimmed (SanitizeChat), at most
// kMaxNicknameChars characters and kMaxPlayerName bytes. May be empty.
inline std::string CleanNickname(const std::string& in) {
    return names_detail::Cut(SanitizeChat(in), kMaxNicknameChars, kMaxPlayerName);
}
// A typed lobby name, cleaned the same way, at most kMaxLobbyNameChars characters. May be empty.
inline std::string CleanLobbyName(const std::string& in) {
    return names_detail::Cut(SanitizeChat(in), kMaxLobbyNameChars, kMaxLobbyNameBytes);
}
// A name read from the wire or from Steam (Hello.name, a persona name), cleaned before it is stored or shown: the same
// cleaning, at most kMaxPlayerName bytes. No slur check: it is another player's, and a Steam name was never checked.
inline std::string CleanPeerName(const std::string& in) {
    return names_detail::Cut(SanitizeChat(in), kMaxPlayerName, kMaxPlayerName);
}

// The nickname to use: the cleaned one, unless it is empty or holds a slur (a hand-edited HSMMod.ini): then empty.
inline std::string UsableNickname(const std::string& in) {
    const std::string n = CleanNickname(in);
    return NameHasSlur(n) ? std::string() : n;
}
inline std::string UsableLobbyName(const std::string& in) {
    const std::string n = CleanLobbyName(in);
    return NameHasSlur(n) ? std::string() : n;
}

// The name this player sends in Hello: the nickname when there is one, else the Steam name.
inline std::string OwnName(const std::string& nickname, const std::string& steamName) {
    const std::string n = UsableNickname(nickname);
    return n.empty() ? steamName : n;
}

// The Steam lobby's name key: the host's lobby name, or "<host>'s fight" when there is none.
inline std::string LobbyTitle(const std::string& lobbyName, const std::string& hostName) {
    const std::string n = UsableLobbyName(lobbyName);
    return n.empty() ? hostName + "'s fight" : n;
}

// ---------------------------------------------------------------- HIDE PLAYER NAMES
// "Player 1" to "Player 8" by slot (slot 0 is the host). A negative slot (not known) is just "Player".
inline std::string HiddenName(int slot) { return slot < 0 ? std::string("Player") : "Player " + std::to_string(slot + 1); }

// The name this screen shows for a player. With the switch on, every other player is "Player N" by slot; the player's own
// name is never hidden. Only what is drawn goes through this: the wire and the logs keep the real names.
inline std::string DisplayName(bool hideOthers, bool mine, int slot, const std::string& name) {
    return hideOthers && !mine ? HiddenName(slot) : name;
}

// The same by entity id (a player's is slot + 1, 1..kMaxFighters; a bot's is past that and keeps its name).
inline std::string DisplayNameOfEntity(bool hideOthers, uint8_t myEntity, uint8_t entity, const std::string& name) {
    const bool player = entity >= 1 && entity <= kMaxFighters;
    return DisplayName(hideOthers && player, entity == myEntity, int(entity) - 1, name);
}

// The match as this screen draws it: with the switch on, every other player's name in the fighter list is replaced, so the
// scoreboard, the kill feed, the banners and the name tags built from it all agree.
inline MatchInfo HideNamesIn(MatchInfo m, uint8_t myEntity) {
    for (FighterEntry& f : m.fighters) f.name = DisplayNameOfEntity(true, myEntity, f.entity, f.name);
    return m;
}

}  // namespace hsm
