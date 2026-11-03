// Multiplayer modpack, the lobby side (HSMMod 0.8.0; the rules and the manifest format are session/hsm_modpack.h, how to
// build a pack is docs/MODPACK.md). The host announces the pack installed in Paks/~mods/<pack>/ in its lobby data
// (hsm_pack, hsm_packv, hsm_packh); a joining machine checks its own copy against it and refuses to join when it lacks
// the pack or has another version. The file scan and the hashes are gameplay/mod_maps.cpp's; nothing is downloaded.
// Game thread only.
#pragma once
#include "session/hsm_modpack.h"
#include <string>

namespace hsm::game {
class Lobby;

namespace modpack {

// [mod] pack= from HSMMod.ini: the pack the host announces ("off": none; empty: the only valid one installed).
void SetConfigured(const std::string& name);
// Each frame while in a lobby: as host, announces the pack once the scan has listed and hashed its files.
void Tick(Lobby& lobby);
// This machine against a lobby's pack, with the current scan (Pending until the scan is over).
PackCheck Check(const LobbyPack& lobby);
// "pack: ok" / "pack: missing <file>" / "pack: <file> sha256 differs" ...: once per distinct result, for a join.
void LogJoinCheck(const LobbyPack& lobby, const PackCheck& check);

}  // namespace modpack
}  // namespace hsm::game
