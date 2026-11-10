// The UE4SS builds this mod accepts, by SHA-256 of UE4SS.dll. The mod carries one (kBundledUe4ss, client src/ue4ss_bundle.cpp) and
// players may have another, so this lists versions rather than one file. A build of his own is how a tester got a gun mod past the
// checks and its damage to other players. A new accepted build goes here.
// Nothing else about the player's PC is judged: what's in the game folder is the player's mods, and the host's rule decides on those.
// Game-independent.
#pragma once
#include <string_view>

namespace hsm {

inline constexpr std::string_view kBundledUe4ss = "aab30033e336df1feae7ee5f175a7dbdbdc0630c9c3d70f3086d212f25009f8b";   // v3.0.1-1217-g4fb9b615
inline constexpr std::string_view kKnownUe4ss[] = {
    "94c2ebd3ca9f4c4212898d727df5482937f586771c67502635ebd4367422052a",   // v3.0.1 Beta, git f58e8f84
    "8ac18fbffc1ef96b0662d4a2d537b3f224c26d65caaba7989a9404c566102b26",   // v3.0.1, the release
    "82e3b900b43e0e0376144143a6391fd0a89bdd4edcf2b9b7dc246b2c0f1757b6",   // a tester's experimental-latest, as sent 2026-11-04
    "d946975cc6d2d9a1bd07d801c5e2b77350d4dbc6431c08a1f302bb175bd1d860",   // v3.0.1-1212-gd4aebbf0
    "405e9ecf23940894de5fb8eb7a0fb271aa584b20eacd777d04ce2a91669fb9ac",   // v3.0.1-1214-g69f1eba5
    "aab30033e336df1feae7ee5f175a7dbdbdc0630c9c3d70f3086d212f25009f8b",   // v3.0.1-1217-g4fb9b615, the one the mod carries
};
inline bool IsKnownUe4ss(std::string_view sha256) {
    for (std::string_view h : kKnownUe4ss) if (h == sha256) return true;
    return false;
}

}  // namespace hsm
