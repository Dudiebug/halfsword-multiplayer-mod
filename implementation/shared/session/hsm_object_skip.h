// Which actor classes the object walk never announces (0.8.0-pre2, M0 fix): fighters' gore, blood, decals, particle and
// Niagara actors, effect projectiles and the mod's own helper actors. Their physics bodies are cosmetic, per machine, and
// short-lived: announcing one would flood the wire with objects no other machine could ever bind. Game-independent.
//
// A class name is split into words (at '_', digits, other punctuation, a lower-to-upper change and the end of an upper-case
// run: "BP_MeshBloodSimManager_C" -> bp, mesh, blood, sim, manager, c) and the class is skipped when any word is on the
// list below, so "Blood_BP_P4_C", "BP_BloodDecal_C", "RunningBlood_BP_C", "NiagaraActor" and a Blueprint child of any of
// them all match, while "BP_Barrel_C", "BP_Gibson_C" (one word, "gibson") and "FlyingCrate" do not. Names are matched
// without case. A name given with a path ("/Game/Effects/Blood/BP_BloodDecal.BP_BloodDecal_C") is read from its last segment.
// The caller applies it to a class and to the classes it derives from (a child of a skipped class is skipped).
#pragma once
#include <cstring>
#include <string>
#include <vector>

namespace hsm {

namespace skipdetail {
inline char Lower(char c) { return (c >= 'A' && c <= 'Z') ? char(c - 'A' + 'a') : c; }
inline bool IsUpper(char c) { return c >= 'A' && c <= 'Z'; }
inline bool IsLowerCh(char c) { return c >= 'a' && c <= 'z'; }
inline bool IsDigit(char c) { return c >= '0' && c <= '9'; }
inline bool IsAlpha(char c) { return IsUpper(c) || IsLowerCh(c); }

// The words that mark an actor class as an effect (lower case, whole word).
inline const char* const* Words(size_t& n) {
    static const char* const kWords[] = {
        "blood", "bloody", "bleed", "gore", "gib", "gibs", "gibbed", "dismember",   // fighters' gore
        "decal", "decals", "splat", "splatter",                                      // marks on the floor and walls
        "niagara", "particle", "particles", "emitter", "fx", "vfx", "effect", "effects", "spark", "sparks",   // engine effects
        "wasps", "flies",                                                            // insect swarms (Niagara actors)
        "hsm", "generator",                                                          // the mod's own actors, spawn helpers
    };
    n = sizeof kWords / sizeof kWords[0];
    return kWords;
}

inline bool WordSkipped(const std::string& w) {
    size_t n = 0;
    const char* const* words = Words(n);
    for (size_t i = 0; i < n; ++i)
        if (w == words[i]) return true;
    return false;
}
}  // namespace skipdetail

// True when the class (by name) is an effect, gore or mod actor and must never become an object.
inline bool ObjectClassSkipped(const char* className) {
    using namespace skipdetail;
    if (!className) return false;
    const char* name = className;
    for (const char* p = className; *p; ++p)
        if (*p == '.' || *p == '/' || *p == ':') name = p + 1;
    std::string word;
    const size_t len = std::strlen(name);
    for (size_t i = 0; i <= len; ++i) {
        const char c = i < len ? name[i] : '\0';
        const bool alnum = IsAlpha(c);
        bool boundary = !alnum;                                            // '_', digits, punctuation and the end end a word
        if (alnum && !word.empty()) {
            const char prev = name[i - 1];
            if (IsLowerCh(prev) && IsUpper(c)) boundary = true;            // fooBar
            else if (IsUpper(prev) && IsUpper(c) && i + 1 < len && IsLowerCh(name[i + 1])) boundary = true;   // HSMGhost -> hsm, ghost
        }
        if (boundary) {
            if (!word.empty() && WordSkipped(word)) return true;
            word.clear();
        }
        if (alnum) word.push_back(Lower(c));
    }
    return false;
}

}  // namespace hsm
