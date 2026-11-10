// Turning refused mods off for one game (docs/PLAN_MOD_RULES.md). A host's rule refuses some of what this machine runs:
// instead of telling the player to delete files, HSMMod turns those mods off until the game closes, restarts the game
// and joins the lobby again. The decisions are here, pure; the file work is gameplay/mod_rules.cpp.
//
//   PlanSwitch       the rule against this machine's report: what to turn off, by kind, and what can't be fixed that way
//   SwitchSummary    the plan as the dialog lists it
//   switch file      HSMMod-switch.txt, written by the game that restarts, read by the new one in DllMain before the
//                    engine opens any pak: which paks to set aside, whether to lock HS Enhancer, which process to wait for
//   aside list       HSMMod-files-off.txt lines: a bare name is a DLL beside the exe, "pak <rel>" a pak under Content/Paks
//   rejoin file      HSMMod-rejoin.txt: the lobby the new game joins on its own, with a deadline and the password if known
//
// Every file is this machine's own, written by the same build that reads it, but a parse still refuses what it can't
// use: a path that leaves Content/Paks is never renamed.
#pragma once
#include "session/hsm_mod_rules.h"
#include <cstdint>
#include <cstdlib>
#include <string>
#include <vector>

namespace hsm {

struct SwitchPlan {
    std::vector<std::string> modsTxt;   // UE4SS mod names to set ": 0" in mods.txt (Ue4ssMod)
    std::vector<std::string> paks;      // pak names (the report's) to rename .pak -> .pak.off (MapPak, ContentPak)
    std::vector<std::string> exeDlls;   // proxy DLLs beside the exe to rename .dll -> .dll.off (Proxy)
    std::vector<std::string> blockers;  // what turning files off can't fix: a mod the lobby needs, too many mods
    bool lockHse = false;               // HS Enhancer stays loaded, locked: its menu, hotkeys and features off (LockedHseIni)
    bool Empty() const { return modsTxt.empty() && paks.empty() && exeDlls.empty() && blockers.empty() && !lockHse; }
    bool Switchable() const { return !Empty() && blockers.empty(); }   // a restart with these off would pass the rule
    size_t Count() const { return modsTxt.size() + paks.size() + exeDlls.size(); }   // files off; the HS Enhancer lock apart
};

// The rule against the report, item by item (ItemRefused), then the rule's required list (what CheckMods calls Missing,
// here even when an item fails too). A report still being built, or a rule the host checks itself (Unchecked), plans
// nothing: the join goes ahead as today. HS Enhancer is never turned off: wherever the rule refuses it, it is locked.
inline SwitchPlan PlanSwitch(const ModRule& rule, const ModReport& mine) {
    SwitchPlan p;
    if (!mine.complete) return p;
    if (mine.truncated) { p.blockers.push_back("more than " + std::to_string(kMaxModItems) + " mods"); return p; }
    for (const ModItem& it : mine.items) {
        switch (ItemRefused(rule, it)) {
            case RuleFail::None: case RuleFail::Pending: break;
            case RuleFail::Unchecked: return SwitchPlan{};
            case RuleFail::Hse: p.lockHse = true; break;
            case RuleFail::NoMods: case RuleFail::NotAllowed:
                if (IsHseItem(it)) p.lockHse = true;   // allowed, but not by this preset: locked all the same
                else if (it.kind == ModKind::Ue4ssMod) p.modsTxt.push_back(it.name);
                else if (it.kind == ModKind::MapPak || it.kind == ModKind::ContentPak) p.paks.push_back(it.name);
                else if (it.kind == ModKind::Proxy) p.exeDlls.push_back(it.name);
                else p.blockers.push_back(it.name);   // a Module: loaded from outside the game folder, nothing to rename
                break;
            case RuleFail::Missing: case RuleFail::TooMany: p.blockers.push_back(it.name); break;
        }
    }
    if (rule.preset == ModPreset::Bundle || rule.preset == ModPreset::Custom)
        for (const ModAllow& a : rule.list) {
            if (!a.required) continue;
            bool have = false;
            for (const ModItem& it : mine.items) if (AllowMatches(a, it)) { have = true; break; }
            if (!have) p.blockers.push_back(a.name + " (this lobby needs it)");
        }
    return p;
}

// The dialog's lines: kHseLockLine, "HalfWolverine (UE4SS mod)", "Foo.pak", "version.dll", then "+N more" past maxLines.
inline constexpr const char* kHseLockLine = "HS Enhancer's menu, hotkeys and features (it stays installed)";
inline std::vector<std::string> SwitchSummary(const SwitchPlan& p, size_t maxLines) {
    std::vector<std::string> all;
    if (p.lockHse) all.push_back(kHseLockLine);
    for (const std::string& m : p.modsTxt) all.push_back(m + " (UE4SS mod)");
    for (const std::string& k : p.paks) all.push_back(k);
    for (const std::string& d : p.exeDlls) all.push_back(d);
    if (maxLines == 0 || all.size() <= maxLines) return all;
    const size_t keep = maxLines > 1 ? maxLines - 1 : 0;
    std::vector<std::string> out(all.begin(), all.begin() + std::ptrdiff_t(keep));
    out.push_back("+" + std::to_string(all.size() - keep) + " more");
    return out;
}

// The browser row's note for a lobby this machine can join after a restart.
// The restart in words, for the status line and the log: "2 mods off", "HS Enhancer locked", "1 mod off and HS
// Enhancer locked".
inline std::string SwitchWords(const SwitchPlan& p) {
    const size_t n = p.Count();
    const std::string mods = std::to_string(n) + (n == 1 ? " mod off" : " mods off");
    if (!p.lockHse) return mods;
    return n == 0 ? std::string("HS Enhancer locked") : mods + " and HS Enhancer locked";
}

inline std::string SwitchTag(const SwitchPlan& p) {
    const size_t n = p.Count();
    if (n == 0 && p.lockHse) return "Restart: HS Enhancer locked";
    return "Restart: " + std::to_string(n) + (n == 1 ? " mod off" : " mods off") + (p.lockHse ? ", HSE locked" : "");
}

// ---- the switch file: "pid N", "pak <rel>" lines (rel under Content/Paks, forward slashes), "hse lock" ------------------

struct SwitchFile { unsigned long pid = 0; std::vector<std::string> paks; bool lockHse = false; };

// A relative pak path the new game may rename: no drive, no root, no "..", no control characters, ends in .pak.
inline bool PakRelOk(const std::string& rel) {
    if (rel.empty() || rel.size() > 512) return false;
    if (rel[0] == '/' || rel[0] == '\\') return false;
    if (rel.find(':') != std::string::npos) return false;
    if (rel.size() < 5 || rel.compare(rel.size() - 4, 4, ".pak") != 0) return false;
    for (unsigned char c : rel) if (c < 0x20 || c == 0x7f) return false;
    size_t at = 0;
    while (at <= rel.size()) {
        size_t end = rel.find_first_of("/\\", at);
        if (end == std::string::npos) end = rel.size();
        const std::string part = rel.substr(at, end - at);
        if (part.empty() || part == "." || part == "..") return false;
        at = end + 1;
    }
    return true;
}

inline std::string EncodeSwitchFile(const SwitchFile& f) {
    std::string s = "pid " + std::to_string(f.pid) + "\n";
    for (const std::string& p : f.paks) if (PakRelOk(p)) s += "pak " + p + "\n";
    if (f.lockHse) s += "hse lock\n";
    return s;
}

// False: no usable pid line. Pak lines that fail PakRelOk are dropped, the rest kept; other lines are ignored.
inline bool ParseSwitchFile(const std::string& text, SwitchFile& out) {
    out = SwitchFile{};
    bool pid = false;
    size_t at = 0;
    while (at < text.size()) {
        size_t end = text.find('\n', at);
        if (end == std::string::npos) end = text.size();
        std::string line = text.substr(at, end - at);
        at = end + 1;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.rfind("pid ", 0) == 0 && !pid) {
            const std::string v = line.substr(4);
            if (v.empty() || v.size() > 10 || v.find_first_not_of("0123456789") != std::string::npos) return false;
            out.pid = std::strtoul(v.c_str(), nullptr, 10);
            pid = true;
        } else if (line.rfind("pak ", 0) == 0) {
            const std::string rel = line.substr(4);
            if (PakRelOk(rel)) out.paks.push_back(rel);
        } else if (line == "hse lock") out.lockHse = true;
    }
    return pid;
}

// ---- the aside list's lines -------------------------------------------------------------------------------------------

struct AsideEntry { bool pak = false; std::string name; };   // pak: name is a rel path under Content/Paks; else a DLL beside the exe

inline std::string AsideLine(const AsideEntry& e) { return (e.pak ? "pak " : "") + e.name + "\n"; }

// False: a line this build won't act on (a DLL name with a path in it, a pak path that leaves Content/Paks).
inline bool ParseAsideLine(std::string line, AsideEntry& out) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    out = AsideEntry{};
    if (line.rfind("pak ", 0) == 0) { out.pak = true; out.name = line.substr(4); return PakRelOk(out.name); }
    out.name = line;
    return !line.empty() && line.find_first_of("\\/:") == std::string::npos;
}

// ---- the rejoin file: "lobby <id64>", "until <unix s>", "pass <password>" ---------------------------------------------

struct RejoinFile { uint64_t lobby = 0; uint64_t until = 0; std::string password; };
inline constexpr uint64_t kRejoinWindowS = 600;   // a restart that takes longer than this joins nothing

inline std::string EncodeRejoinFile(const RejoinFile& f) {
    std::string s = "lobby " + std::to_string(f.lobby) + "\nuntil " + std::to_string(f.until) + "\n";
    if (!f.password.empty()) s += "pass " + f.password + "\n";
    return s;
}

// False: no lobby, no deadline, or the deadline (nowS) has passed. The password line is optional.
inline bool ParseRejoinFile(const std::string& text, uint64_t nowS, RejoinFile& out) {
    out = RejoinFile{};
    bool lobby = false, until = false;
    size_t at = 0;
    const auto num = [](const std::string& v, uint64_t& n) {
        if (v.empty() || v.size() > 20 || v.find_first_not_of("0123456789") != std::string::npos) return false;
        n = std::strtoull(v.c_str(), nullptr, 10);
        return true;
    };
    while (at < text.size()) {
        size_t end = text.find('\n', at);
        if (end == std::string::npos) end = text.size();
        std::string line = text.substr(at, end - at);
        at = end + 1;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.rfind("lobby ", 0) == 0) lobby = num(line.substr(6), out.lobby);
        else if (line.rfind("until ", 0) == 0) until = num(line.substr(6), out.until);
        else if (line.rfind("pass ", 0) == 0) out.password = line.substr(5);
    }
    if (!lobby || !until || !out.lobby) return false;
    return nowS <= out.until;
}

}  // namespace hsm
