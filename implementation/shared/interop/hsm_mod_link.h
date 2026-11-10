// The link page: how HSMMod (dwmapi.dll) and Fetch (dsound.dll) tell each other what they are doing while both run in one game.
// It is one small named file mapping per game process. Each mod owns one half and writes only that half. Either mod may make the
// page; the other opens it. A mod that is not there leaves its half zero, and a zero half reads as "not there".
//
// THE LAYOUT IS A CONTRACT BETWEEN TWO REPOSITORIES (HSMMod's and Fetch's). The same file lives in both.
// It only grows: never move, resize or reuse a field or a flag bit. New fields go at the end of a half, and the half's `size` says
// how much its writer filled, so an older reader ignores what it does not know and a newer reader sees zero for what is missing.
//
// This header has no OS calls: the mod maps the page (CreateFileMappingW with LinkName, kLinkBytes, read-write) and hands the
// bytes here. Every field is an aligned 32-bit word, written and read on the game thread.
#pragma once
#include <cstdint>
#include <cstring>
#include <string>

namespace hsm::interop {

inline constexpr uint32_t kLinkBytes = 256;          // the mapping's size, for good
inline constexpr uint32_t kLinkMagic = 0x4B4E4C48;   // "HLNK", little endian, at byte 0
inline constexpr uint32_t kHsmAt = 16;               // HSMMod's half: bytes 16..135
inline constexpr uint32_t kFetchAt = 136;            // Fetch's half: bytes 136..255
inline constexpr uint32_t kHalfBytes = 120;          // room in each half
inline constexpr uint32_t kLinkStaleMs = 2000;       // a half whose beat has not moved for this long is not there

inline std::wstring LinkName(uint32_t pid) { return L"Local\\HSMLink-" + std::to_wstring(pid); }

// Words of a half, in order. Word 0 is the half's size in bytes, word 1 its beat (goes up by one on every write).
enum : uint32_t { kWordSize = 0, kWordBeat = 1, kWordFlags = 2, kWordExtra = 3 };

// HSMMod's flags (word 2). Word 3 is its sync mode.
enum HsmFlag : uint32_t {
    kHsmInSession = 1u << 0,       // in a multiplayer session (lobby or match)
    kHsmTyping = 1u << 1,          // chat or any text field of HSMMod's has the keyboard
    kHsmReplayingInput = 1u << 2,  // set only while HSMMod plays a held-back key again (host input delay): not a new key press
    kHsmHostPlaysMine = 1u << 3,   // host physics and I am a client: the host's machine moves my fighter
};
enum class SyncMode : uint32_t { None = 0, Owner = 1, Host = 2 };

// Fetch's flags (word 2).
enum FetchFlag : uint32_t {
    kFetchMenuOpen = 1u << 0,      // Fetch's menu is open and has the keyboard
    kFetchTyping = 1u << 1,        // a name is being typed in Fetch's menu
};

struct HsmState {
    bool inSession = false, typing = false, replayingInput = false, hostPlaysMine = false;
    SyncMode syncMode = SyncMode::None;
    bool operator==(const HsmState&) const = default;
};
struct FetchState {
    bool menuOpen = false, typing = false;
    bool operator==(const FetchState&) const = default;
};

namespace detail {
inline uint32_t Word(const uint8_t* page, uint32_t at, uint32_t word) {
    uint32_t v = 0;
    std::memcpy(&v, page + at + word * 4, 4);
    return v;
}
inline void SetWord(uint8_t* page, uint32_t at, uint32_t word, uint32_t v) { std::memcpy(page + at + word * 4, &v, 4); }
// A word the writer did not fill reads as zero.
inline uint32_t WordIf(const uint8_t* page, uint32_t at, uint32_t word) {
    const uint32_t size = Word(page, at, kWordSize);
    return size <= kHalfBytes && (word + 1) * 4 <= size ? Word(page, at, word) : 0;
}
inline constexpr uint32_t Bit(bool on, uint32_t flag) { return on ? flag : 0u; }
inline void Write(uint8_t* page, uint32_t at, uint32_t flags, uint32_t extra) {
    SetWord(page, 0, 0, kLinkMagic);
    SetWord(page, at, kWordFlags, flags);
    SetWord(page, at, kWordExtra, extra);
    SetWord(page, at, kWordSize, 16);   // the words this build fills
    SetWord(page, at, kWordBeat, Word(page, at, kWordBeat) + 1);
}
}  // namespace detail

inline bool LinkValid(const uint8_t* page) { return page && detail::Word(page, 0, 0) == kLinkMagic; }

// Each mod calls its own Write once per game tick, and again at once when a flag another mod acts on changes mid-tick.
inline void WriteHsm(uint8_t* page, const HsmState& s) {
    if (!page) return;
    detail::Write(page, kHsmAt,
                  detail::Bit(s.inSession, kHsmInSession) | detail::Bit(s.typing, kHsmTyping) |
                      detail::Bit(s.replayingInput, kHsmReplayingInput) | detail::Bit(s.hostPlaysMine, kHsmHostPlaysMine),
                  static_cast<uint32_t>(s.syncMode));
}
inline void WriteFetch(uint8_t* page, const FetchState& s) {
    if (!page) return;
    detail::Write(page, kFetchAt, detail::Bit(s.menuOpen, kFetchMenuOpen) | detail::Bit(s.typing, kFetchTyping), 0);
}

// One flag of HSMMod's, set and cleared in place without a full write: for the replay flag, which changes inside a tick.
inline void SetHsmFlag(uint8_t* page, HsmFlag flag, bool on) {
    if (!page) return;
    const uint32_t f = detail::Word(page, kHsmAt, kWordFlags);
    detail::SetWord(page, kHsmAt, kWordFlags, on ? f | flag : f & ~static_cast<uint32_t>(flag));
}

inline uint32_t HsmBeat(const uint8_t* page) { return LinkValid(page) ? detail::WordIf(page, kHsmAt, kWordBeat) : 0; }
inline uint32_t FetchBeat(const uint8_t* page) { return LinkValid(page) ? detail::WordIf(page, kFetchAt, kWordBeat) : 0; }

inline HsmState ReadHsm(const uint8_t* page) {
    HsmState s;
    if (!LinkValid(page)) return s;
    const uint32_t f = detail::WordIf(page, kHsmAt, kWordFlags), mode = detail::WordIf(page, kHsmAt, kWordExtra);
    s.inSession = (f & kHsmInSession) != 0;
    s.typing = (f & kHsmTyping) != 0;
    s.replayingInput = (f & kHsmReplayingInput) != 0;
    s.hostPlaysMine = (f & kHsmHostPlaysMine) != 0;
    s.syncMode = mode <= static_cast<uint32_t>(SyncMode::Host) ? static_cast<SyncMode>(mode) : SyncMode::None;   // a mode this build does not know
    return s;
}
inline FetchState ReadFetch(const uint8_t* page) {
    FetchState s;
    if (!LinkValid(page)) return s;
    const uint32_t f = detail::WordIf(page, kFetchAt, kWordFlags);
    s.menuOpen = (f & kFetchMenuOpen) != 0;
    s.typing = (f & kFetchTyping) != 0;
    return s;
}

// Whether the other mod's half is alive: its beat has moved within kLinkStaleMs. A beat of zero was never written.
class LinkWatch {
public:
    // Once per game tick with the other half's beat and the tick's length. Returns whether the other mod is there.
    bool Tick(uint32_t beat, uint32_t dtMs) {
        if (beat != 0 && (!seen_ || beat != beat_)) { seen_ = true; beat_ = beat; quietMs_ = 0; }
        else if (quietMs_ < kLinkStaleMs) quietMs_ += dtMs < kLinkStaleMs ? dtMs : kLinkStaleMs;
        return live();
    }
    bool live() const { return seen_ && quietMs_ < kLinkStaleMs; }
    void Reset() { *this = LinkWatch(); }

private:
    bool seen_ = false;
    uint32_t beat_ = 0, quietMs_ = 0;
};

// The rules both mods follow. `live` is LinkWatch's answer for the other mod.

// Fetch's menu in a Steam lobby. Offline it opens as always. In a lobby it needs HSMMod on the link (so HSMMod can leave the
// menu's keys alone) and HSMMod not typing (so `;` in chat is a semicolon).
inline bool FetchMenuMayOpen(bool offline, bool hsmLive, const HsmState& hsm) { return offline || (hsmLive && !hsm.typing); }
// An open menu shuts when HSMMod starts typing, or when the link is lost in a lobby.
inline bool FetchMenuMustClose(bool offline, bool hsmLive, const HsmState& hsm) { return !FetchMenuMayOpen(offline, hsmLive, hsm); }
// A key Fetch's recorder sees while HSMMod plays held-back keys again is not a new press. The flag is trusted only from a live
// half, so a mod that died with it set cannot stop recording for good.
inline bool FetchSkipsInput(bool hsmLive, const HsmState& hsm) { return hsmLive && hsm.replayingInput; }
// HSMMod ignores its own keys (chat, bite, build card, voice, the rest) while Fetch's menu has the keyboard.
inline bool HsmKeysBlocked(bool fetchLive, const FetchState& fetch) { return fetchLive && (fetch.menuOpen || fetch.typing); }

}  // namespace hsm::interop
