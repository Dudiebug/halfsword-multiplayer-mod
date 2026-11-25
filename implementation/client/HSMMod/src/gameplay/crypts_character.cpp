#include "gameplay/crypts_character.h"
#include <windows.h>
#include "log.h"
#include "session/hsm_crypt_character.h"
#include "session/hsm_gvas.h"
#include <string>

namespace hsm::game::cryptschar {
namespace {

constexpr const wchar_t* kFile = L"HSMMP_Crypts.sav";
constexpr const wchar_t* kPrev = L"HSMMP_Crypts.prev.sav";   // the character as it was before its last write
constexpr size_t kMaxBytes = 256 * 1024;

bool ReadAll(const std::wstring& path, std::vector<uint8_t>& out) {
    out.clear();
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER size{};
    bool ok = GetFileSizeEx(h, &size) && size.QuadPart > 0 && size.QuadPart <= LONGLONG(kMaxBytes);
    if (ok) {
        out.resize(size_t(size.QuadPart));
        DWORD got = 0;
        ok = ReadFile(h, out.data(), DWORD(out.size()), &got, nullptr) && got == out.size();
    }
    CloseHandle(h);
    if (!ok) out.clear();
    return ok;
}

// Written beside the target and moved over it, so a crash never leaves half a file.
bool WriteAll(const std::wstring& path, const std::vector<uint8_t>& bytes) {
    const std::wstring tmp = path + L".hsmtmp";
    HANDLE h = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD put = 0;
    bool ok = WriteFile(h, bytes.data(), DWORD(bytes.size()), &put, nullptr) && put == bytes.size();
    ok = FlushFileBuffers(h) && ok;
    CloseHandle(h);
    if (ok) ok = MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
    if (!ok) DeleteFileW(tmp.c_str());
    return ok;
}

bool Good(const std::vector<uint8_t>& b, std::string* why) { return !b.empty() && CheckPassportSave(b.data(), b.size(), why, kMaxBytes); }

}  // namespace

std::vector<uint8_t> Load(Adapter& adapter) {
    const std::wstring dir = adapter.SaveGamesDir();
    if (dir.empty()) { HSM_LOG("crypts co-op: no save folder yet; this run starts as the blank peasant"); return {}; }
    std::vector<uint8_t> main, prev;
    const bool hadMain = ReadAll(dir + kFile, main), hadPrev = ReadAll(dir + kPrev, prev);
    std::string whyMain, whyPrev;
    const bool mainGood = Good(main, &whyMain), prevGood = Good(prev, &whyPrev);
    switch (crypt::PickCharacterFile(mainGood, prevGood)) {
        case 0: HSM_LOG("crypts co-op: your crypt character read (%zu bytes)", main.size()); return main;
        case 1:
            HSM_LOG("crypts co-op: HSMMP_Crypts.sav %s (%s); the one before it read (%zu bytes)", hadMain ? "refused" : "missing",
                    whyMain.empty() ? "empty" : whyMain.c_str(), prev.size());
            return prev;
        default:
            if (hadMain || hadPrev)
                HSM_LOG("crypts co-op: no usable crypt character (HSMMP_Crypts.sav: %s, .prev: %s); this run starts as the blank peasant",
                        hadMain ? (whyMain.empty() ? "empty" : whyMain.c_str()) : "missing", hadPrev ? (whyPrev.empty() ? "empty" : whyPrev.c_str()) : "missing");
            else HSM_LOG("crypts co-op: no crypt character yet; this run starts as the blank peasant");
            return {};
    }
}

bool Save(Adapter& adapter, const std::vector<uint8_t>& passport) {
    std::string why;
    if (!Good(passport, &why)) { HSM_LOG("crypts co-op: character not saved: %s", why.empty() ? "empty" : why.c_str()); return false; }
    const std::wstring dir = adapter.SaveGamesDir();
    if (dir.empty()) { HSM_LOG("crypts co-op: character not saved: no save folder"); return false; }
    std::vector<uint8_t> was;
    if (ReadAll(dir + kFile, was) && Good(was, nullptr) && !WriteAll(dir + kPrev, was))
        HSM_LOG("crypts co-op: the character before could not be kept as HSMMP_Crypts.prev.sav");
    if (!WriteAll(dir + kFile, passport)) { HSM_LOG("crypts co-op: HSMMP_Crypts.sav could not be written"); return false; }
    HSM_LOG("crypts co-op: crypt character saved (%zu bytes)", passport.size());
    return true;
}

}  // namespace hsm::game::cryptschar
