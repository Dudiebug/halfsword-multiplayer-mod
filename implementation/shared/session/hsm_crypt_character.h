// Co-op Crypts (IsCoopCrypts): each player's own crypt character, kept between runs on their own machine, as the game keeps
// its Crypts character (GI.Abyss Character Player 1 FM) between solo runs. The game's own save is never written in a
// session (the save guard), so the mod keeps it in its own file, HSMMP_Crypts.sav (plus .prev), next to the game's saves.
// What a run leaves the player with, by how their run ended (crypt::Outcome, session/hsm_crypt_run.h):
//   Out (got out)              the kit they carried out
//   Down (fell)                the blank peasant, as the game wipes a dead character
//   Inside / Left (the run ended with them inside, the host left, or they left)  the last kit read while alive
//   Absent (never got in)      the character they came with, unchanged
// With no kit read at all (Out/Inside/Left), the character they came with is kept: a run never loses a character to a
// missing read. Game-independent and unit-tested (session/test_hsm_crypt_character.cpp); the DLL does the file IO.
#pragma once
#include "session/hsm_crypt_run.h"
#include <cstdint>

namespace hsm::crypt {

inline constexpr const char* kCoopCharacterFile = "HSMMP_Crypts.sav";

enum class CharAfter : uint8_t { Keep = 0, Final = 1, Blank = 2 };
inline const char* CharAfterName(CharAfter a) {
    switch (a) {
        case CharAfter::Keep: return "kept as it was";
        case CharAfter::Final: return "saved with the run's kit";
        case CharAfter::Blank: return "wiped (fell in the Crypts)";
    }
    return "?";
}

// haveKit: a kit was read for this player during the run (the final one at the exit, else the last one read alive).
inline CharAfter CoopCharAfter(Outcome o, bool haveKit) {
    switch (o) {
        case Outcome::Down: return CharAfter::Blank;
        case Outcome::Out: case Outcome::Inside: case Outcome::Left: return haveKit ? CharAfter::Final : CharAfter::Keep;
        case Outcome::Absent: return CharAfter::Keep;
    }
    return CharAfter::Keep;
}

// Which of the two files to load, each told good when it is a passport save (CheckPassportSave): the main one, else the
// backup, else none (a first run: the blank peasant). 0 = main, 1 = .prev, -1 = none.
inline int PickCharacterFile(bool mainGood, bool prevGood) { return mainGood ? 0 : prevGood ? 1 : -1; }

// One save per run: a run's character is written once (the first end the player sees), so a later, weaker end (the
// results screen after an exit, the lobby after a fall) cannot overwrite it.
class CharSaveOnce {
public:
    bool Take(uint16_t runSeq) { if (runSeq == 0 || runSeq == saved_) return false; saved_ = runSeq; return true; }
    uint16_t saved() const { return saved_; }
private:
    uint16_t saved_ = 0;
};

}  // namespace hsm::crypt
