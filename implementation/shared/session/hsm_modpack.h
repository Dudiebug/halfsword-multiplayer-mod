// Multiplayer modpack (HSMMod 0.8.0, mechanism only): a pack is a folder Content/Paks/~mods/<pack>/ with a manifest
// HSMPack.ini beside the .pak files that make it up (maps, weapons ...). The host's lobby names its pack (lobby data
// hsm_pack = name, hsm_packv = version, hsm_packh = identity prefix); a joining client compares that with its own copy and
// refuses to join when it is missing or different, with the line below. Nothing is downloaded or sent: the player
// installs the pack by hand (docs/MODPACK.md). Game-independent and unit-tested; the file scan is gameplay/mod_maps.cpp,
// the lobby side gameplay/mod_pack.cpp.
//
// The manifest (a small ini, strict):
//     [pack]
//     name=ArenaPack        the folder name too: A-Z a-z 0-9 . _ - (1..32)
//     version=1.2           A-Z a-z 0-9 . _ - (1..16)
//     [pak]                 one section per .pak file, 1..32 of them
//     file=ArenaMap_P.pak   a file name, never a path: A-Z a-z 0-9 . _ - ending in .pak
//     size=12345678         bytes
//     sha256=<64 hex>
//     author=...            optional, for the pack's page: author, source (an https:// link), license (text <= 120)
// A line starting with ; or # is a comment (there are no comments after a value). Anything else (a path, a second [pack], a bad hash, a duplicate file) makes the whole manifest invalid.
//
// The identity is the SHA-256 of the manifest's meaning, not of its bytes: name, version and every pak's file, size and
// hash in file-name order, so a different line ending or an edited licence note does not change it.
#pragma once
#include "session/hsm_modmaps.h"
#include "util/hsm_sha256.h"
#include <algorithm>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace hsm {

inline constexpr size_t kMaxPackName = 32;
inline constexpr size_t kMaxPackVersion = 16;
inline constexpr size_t kMaxPackPaks = 32;
inline constexpr size_t kMaxPackFile = 64;
inline constexpr size_t kMaxPackText = 120;
inline constexpr size_t kMaxPackManifestBytes = 64 * 1024;
inline constexpr size_t kMaxPackLine = 400;
inline constexpr size_t kPackHashChars = 16;   // hsm_packh: the first 16 hex digits of the identity
inline constexpr const char kPackManifestFile[] = "HSMPack.ini";

struct PackPak {
    std::string file;
    uint64_t size = 0;
    Sha256Digest sha{};
    std::string author, source, license;
};
struct PackManifest {
    std::string name, version;
    std::vector<PackPak> paks;
};

enum class PackParse : uint8_t {
    Ok, TooBig, BadLine, BadSection, MissingPack, BadName, BadVersion, BadFormat, NoPaks, TooManyPaks,
    BadFile, DuplicateFile, BadSize, BadHash, BadText, BadSource,
};

inline const char* PackParseText(PackParse p) {
    switch (p) {
        case PackParse::Ok: return "ok";
        case PackParse::TooBig: return "the manifest is too big";
        case PackParse::BadLine: return "a line is not key=value";
        case PackParse::BadSection: return "a section is not [pack] or [pak], or [pack] is repeated";
        case PackParse::MissingPack: return "no [pack] section";
        case PackParse::BadName: return "the pack name is not valid";
        case PackParse::BadVersion: return "the pack version is not valid";
        case PackParse::BadFormat: return "the manifest format is not 1";
        case PackParse::NoPaks: return "the pack lists no paks";
        case PackParse::TooManyPaks: return "the pack lists too many paks";
        case PackParse::BadFile: return "a pak file name is not valid";
        case PackParse::DuplicateFile: return "a pak is listed twice";
        case PackParse::BadSize: return "a pak size is not valid";
        case PackParse::BadHash: return "a pak sha256 is not valid";
        case PackParse::BadText: return "a note is not plain text";
        case PackParse::BadSource: return "a source is not an https link";
    }
    return "invalid";
}

namespace modpack_detail {
inline bool NameChars(std::string_view s) {
    for (char c : s) {
        const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-';
        if (!ok) return false;
    }
    return !s.empty();
}
inline std::string_view Trim(std::string_view s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.remove_prefix(1);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r')) s.remove_suffix(1);
    return s;
}
inline bool PlainText(std::string_view s) {
    if (s.size() > kMaxPackText) return false;
    for (char c : s) { const unsigned char u = static_cast<unsigned char>(c); if (u < 0x20 || u >= 0x7F) return false; }
    return true;
}
inline bool Number(std::string_view s, uint64_t& v) {
    if (s.empty() || s.size() > 19) return false;
    v = 0;
    for (char c : s) { if (c < '0' || c > '9') return false; v = v * 10 + uint64_t(c - '0'); }
    return true;
}
inline std::string Lower(std::string_view s) { std::string o(s); for (char& c : o) if (c >= 'A' && c <= 'Z') c = char(c + 32); return o; }
}  // namespace modpack_detail

inline bool PackNameOk(std::string_view s) { return s.size() <= kMaxPackName && modpack_detail::NameChars(s) && s[0] != '.'; }
inline bool PackVersionOk(std::string_view s) { return s.size() <= kMaxPackVersion && modpack_detail::NameChars(s); }
// A pak file in a manifest: a plain name ending in .pak, short enough for the ModMap message too.
inline bool PackFileOk(std::string_view s) {
    return s.size() >= 5 && s.size() <= kMaxPackFile && s.compare(s.size() - 4, 4, ".pak") == 0 && s[0] != '.' &&
           modpack_detail::NameChars(s.substr(0, s.size() - 4)) && ModPakNameOk(s);
}

inline PackParse ParsePackManifest(std::string_view text, PackManifest& out) {
    using namespace modpack_detail;
    out = PackManifest{};
    if (text.size() > kMaxPackManifestBytes) return PackParse::TooBig;
    enum class In { None, Pack, Pak } in = In::None;
    bool havePack = false, haveName = false, haveVersion = false;
    PackPak cur;
    bool haveFile = false, haveSize = false, haveSha = false;
    auto closePak = [&]() -> PackParse {
        if (in != In::Pak) return PackParse::Ok;
        if (!haveFile) return PackParse::BadFile;
        if (!haveSize) return PackParse::BadSize;
        if (!haveSha) return PackParse::BadHash;
        for (const PackPak& p : out.paks) if (Lower(p.file) == Lower(cur.file)) return PackParse::DuplicateFile;
        out.paks.push_back(cur);
        return PackParse::Ok;
    };
    size_t pos = 0;
    while (pos <= text.size()) {
        size_t end = text.find('\n', pos);
        if (end == std::string_view::npos) end = text.size();
        const std::string_view raw = text.substr(pos, end - pos);
        pos = end + 1;
        if (raw.size() > kMaxPackLine) return PackParse::BadLine;
        std::string_view line = Trim(raw);
        if (line.empty() || line.front() == ';' || line.front() == '#') continue;
        if (line.front() == '[') {
            if (line.back() != ']') return PackParse::BadSection;
            const std::string sec = Lower(Trim(line.substr(1, line.size() - 2)));
            if (const PackParse c = closePak(); c != PackParse::Ok) return c;
            if (sec == "pack") {
                if (havePack) return PackParse::BadSection;
                havePack = true; in = In::Pack;
            } else if (sec == "pak") {
                if (out.paks.size() >= kMaxPackPaks) return PackParse::TooManyPaks;
                cur = PackPak{}; haveFile = haveSize = haveSha = false; in = In::Pak;
            } else {
                return PackParse::BadSection;
            }
            continue;
        }
        const size_t eq = line.find('=');
        if (eq == std::string_view::npos || in == In::None) return PackParse::BadLine;
        const std::string key = Lower(Trim(line.substr(0, eq)));
        const std::string_view value = Trim(line.substr(eq + 1));
        if (in == In::Pack) {
            if (key == "name") { if (!PackNameOk(value)) return PackParse::BadName; out.name = std::string(value); haveName = true; }
            else if (key == "version") { if (!PackVersionOk(value)) return PackParse::BadVersion; out.version = std::string(value); haveVersion = true; }
            else if (key == "format") { if (value != "1") return PackParse::BadFormat; }
        } else {
            if (key == "file") { if (!PackFileOk(value)) return PackParse::BadFile; cur.file = std::string(value); haveFile = true; }
            else if (key == "size") {
                uint64_t v = 0;
                if (!Number(value, v) || v < 1 || v > kMaxModPakBytes) return PackParse::BadSize;
                cur.size = v; haveSize = true;
            }
            else if (key == "sha256") { if (!Sha256FromHex(std::string(value), cur.sha) || Sha256IsZero(cur.sha)) return PackParse::BadHash; haveSha = true; }
            else if (key == "author") { if (!PlainText(value)) return PackParse::BadText; cur.author = std::string(value); }
            else if (key == "license") { if (!PlainText(value)) return PackParse::BadText; cur.license = std::string(value); }
            else if (key == "source") {
                if (!PlainText(value)) return PackParse::BadText;
                if (!value.empty() && value.compare(0, 8, "https://") != 0) return PackParse::BadSource;
                cur.source = std::string(value);
            }
        }
    }
    if (const PackParse c = closePak(); c != PackParse::Ok) return c;
    if (!havePack) return PackParse::MissingPack;
    if (!haveName) return PackParse::BadName;
    if (!haveVersion) return PackParse::BadVersion;
    if (out.paks.empty()) return PackParse::NoPaks;
    return PackParse::Ok;
}

// ------------------------------------------------------------------ identity

inline Sha256Digest PackIdentity(const PackManifest& m) {
    std::vector<const PackPak*> sorted;
    for (const PackPak& p : m.paks) sorted.push_back(&p);
    std::sort(sorted.begin(), sorted.end(), [](const PackPak* a, const PackPak* b) { return modpack_detail::Lower(a->file) < modpack_detail::Lower(b->file); });
    std::string s = "hsmpack1\n" + m.name + "\n" + m.version + "\n";
    for (const PackPak* p : sorted) s += modpack_detail::Lower(p->file) + ":" + std::to_string(p->size) + ":" + Sha256Hex(p->sha) + "\n";
    return Sha256Of(reinterpret_cast<const uint8_t*>(s.data()), s.size());
}
inline std::string PackHashPrefix(const PackManifest& m) { return Sha256Hex(PackIdentity(m)).substr(0, kPackHashChars); }

// ------------------------------------------------------------------ the lobby's pack

// What a lobby announces (hsm_pack, hsm_packv, hsm_packh), as read from peer-written data: all three valid or no pack.
struct LobbyPack {
    std::string name, version, hash;
    bool any() const { return !name.empty(); }
};

inline LobbyPack ReadLobbyPack(std::string_view name, std::string_view version, std::string_view hash) {
    LobbyPack p;
    if (name.empty() || !PackNameOk(name) || !PackVersionOk(version) || hash.size() != kPackHashChars) return p;
    for (char c : hash) if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return p;
    p.name = std::string(name); p.version = std::string(version); p.hash = std::string(hash);
    return p;
}
inline LobbyPack LobbyPackOf(const PackManifest& m) { LobbyPack p; p.name = m.name; p.version = m.version; p.hash = PackHashPrefix(m); return p; }

// ------------------------------------------------------------------ the local copy

// A folder under ~mods and what its manifest said.
struct LocalPack {
    std::string folder;
    bool valid = false;
    PackParse error = PackParse::Ok;   // when !valid
    PackManifest manifest;             // when valid; its name equals the folder's (the scan checks)
};

enum class PackVerdict : uint8_t {
    NoPack,          // the lobby has no pack
    Ok,              // every pak is here with the right size and hash
    Pending,         // the scan or a hash is still running: ask again
    NotInstalled,    // no ~mods/<name>/ with a manifest
    BadManifest,     // the folder is there but its manifest is not valid
    DifferentPack,   // another version, or another list of paks
    MissingPak,      // `file` is not here
    DifferentPak,    // `file` is here with other bytes
};
struct PackCheck { PackVerdict verdict = PackVerdict::NoPack; std::string file; };

// The same decision a joining client makes about the lobby's pack and a host makes about its own: `lobby` against the
// local folders and the scanned paks (session/hsm_modmaps.h ModPak). `listed`: the file scan has listed every pak and read
// every manifest (a name that is not in the list is then missing; hashes may still be running). A pak whose hash is still
// pending can only match when its size does, so the check waits for it (Pending) instead of telling the player to install
// what they have.
inline PackCheck CheckPack(const LobbyPack& lobby, const std::vector<LocalPack>& locals, const std::vector<ModPak>& paks, bool listed) {
    using modpack_detail::Lower;
    PackCheck r;
    if (!lobby.any()) return r;
    const LocalPack* local = nullptr;
    for (const LocalPack& l : locals) if (Lower(l.folder) == Lower(lobby.name)) { local = &l; break; }
    if (!local) { r.verdict = listed ? PackVerdict::NotInstalled : PackVerdict::Pending; return r; }
    if (!local->valid) { r.verdict = PackVerdict::BadManifest; return r; }
    const PackManifest& m = local->manifest;
    if (m.version != lobby.version || PackHashPrefix(m) != lobby.hash) { r.verdict = PackVerdict::DifferentPack; return r; }
    bool waiting = false;
    for (const PackPak& want : m.paks) {
        bool match = false, sameName = false, undecided = false;
        for (const ModPak& p : paks) {
            if (Lower(p.name) != Lower(want.file)) continue;
            sameName = true;
            if (p.size != want.size) continue;
            if (!p.hashed) undecided = true;
            else if (p.sha == want.sha) match = true;
        }
        if (match) continue;
        if (undecided || !listed) { waiting = true; continue; }
        r.verdict = sameName ? PackVerdict::DifferentPak : PackVerdict::MissingPak;
        r.file = want.file;
        return r;
    }
    r.verdict = waiting ? PackVerdict::Pending : PackVerdict::Ok;
    return r;
}

// The player's line when the join is refused, and the log line for any verdict. Empty for NoPack and Ok.
inline std::string PackJoinLine(const LobbyPack& lobby, const PackCheck& c) {
    const std::string pack = lobby.name + " " + lobby.version;
    const std::string install = "install it in ~mods/" + lobby.name + ", see the pack's page";
    switch (c.verdict) {
        case PackVerdict::NotInstalled: return "This lobby uses the pack " + pack + ": " + install + ".";
        case PackVerdict::BadManifest: return "This lobby uses the pack " + pack + ", but ~mods/" + lobby.name + "/" + kPackManifestFile + " is not valid: reinstall the pack, see the pack's page.";
        case PackVerdict::DifferentPack: return "This lobby uses the pack " + pack + ", but your copy is another version: " + install + ".";
        case PackVerdict::MissingPak: return "This lobby uses the pack " + pack + ", but " + c.file + " is missing: " + install + ".";
        case PackVerdict::DifferentPak: return "This lobby uses the pack " + pack + ", but your " + c.file + " is a different file: " + install + ".";
        case PackVerdict::Pending: return "Still checking your pack files for " + pack + ". Try again in a moment.";
        case PackVerdict::NoPack: case PackVerdict::Ok: break;
    }
    return {};
}

// The browser row's short mark: "pack ArenaPack" and, when this machine lacks it, "pack ArenaPack (not installed)".
inline std::string PackRowText(const LobbyPack& lobby, PackVerdict v) {
    if (!lobby.any()) return {};
    const std::string base = "pack " + lobby.name;
    switch (v) {
        case PackVerdict::Ok: case PackVerdict::NoPack: return base;
        case PackVerdict::Pending: return base + " (checking)";
        case PackVerdict::NotInstalled: case PackVerdict::BadManifest: return base + " (not installed)";
        default: return base + " (different)";
    }
}

// The browser row's note for a lobby this machine cannot join because of its pack (the row is then not joinable).
inline std::string PackRowReason(PackVerdict v) {
    switch (v) {
        case PackVerdict::NotInstalled: case PackVerdict::BadManifest: return "Install the pack";
        case PackVerdict::Pending: return "Checking your pack files";
        case PackVerdict::NoPack: case PackVerdict::Ok: return {};
        default: return "Your pack differs";
    }
}

// ------------------------------------------------------------------ the host's pack

// Which installed pack the host announces: `configured` ("pack=" under [mod]) names one; "off" announces none; empty
// picks the only valid one. Several valid packs with none named, or a named pack that is not valid, announce nothing
// (`why` says so for the log). Only a pack whose own files match its manifest is announced.
inline const LocalPack* ChooseHostPack(const std::vector<LocalPack>& locals, std::string_view configured, std::string& why) {
    using modpack_detail::Lower;
    why.clear();
    if (Lower(configured) == "off") return nullptr;
    const LocalPack* pick = nullptr;
    if (!configured.empty()) {
        for (const LocalPack& l : locals) if (Lower(l.folder) == Lower(configured)) pick = &l;
        if (!pick) { why = "pack=" + std::string(configured) + " is not installed in ~mods"; return nullptr; }
        if (!pick->valid) { why = std::string("pack ") + pick->folder + ": " + PackParseText(pick->error); return nullptr; }
        return pick;
    }
    size_t valid = 0;
    for (const LocalPack& l : locals) if (l.valid) { ++valid; pick = &l; }
    if (valid > 1) { why = "several packs are installed: set pack=<name> under [mod]"; return nullptr; }
    return valid == 1 ? pick : nullptr;
}

}  // namespace hsm
