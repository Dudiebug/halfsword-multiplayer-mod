// Paths next to the running game executable (Binaries/Win64), where the proxy DLL lives.
#pragma once
#include <windows.h>
#include <string>
#include "agent/hsm_agent.h"

namespace hsm {
inline std::wstring ModFilePath(const wchar_t* file) {
    wchar_t exe[MAX_PATH];
    DWORD n = GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::wstring dir(exe, n);
    size_t slash = dir.find_last_of(L"\\/");
    if (slash != std::wstring::npos) dir.resize(slash + 1);
    return dir + file;
}
// The process command line as UTF-8 (for -hsm* flags).
inline std::string CommandLineUtf8() {
    const wchar_t* w = GetCommandLineW();
    int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    std::string s(size_t(n > 0 ? n : 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w, -1, s.data(), n, nullptr, nullptr);
    if (!s.empty() && s.back() == '\0') s.pop_back();
    return s;
}
// Per-instance file next to the exe: "<base>-<tag><ext>" with -hsmtag=<tag> (letters/digits only), else "<base><ext>".
inline std::wstring InstanceFilePath(const wchar_t* base, const wchar_t* ext) {
    std::string tag = CmdArg(CommandLineUtf8(), "hsmtag").value_or("");
    std::wstring w(base);
    std::wstring t;
    for (char c : tag) if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) t.push_back(wchar_t(c));
    if (!t.empty()) w += L"-" + t;
    return ModFilePath((w + ext).c_str());
}
}  // namespace hsm
