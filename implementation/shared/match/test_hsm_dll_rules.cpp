// A DLL is judged by what it is. These are the ways round a check that goes by name, as a tester tried them.
#include "hsm_dll_rules.h"
#include <cstdio>
#include <cstdlib>
using namespace hsm;
#define CHECK(x) do { if (!(x)) { std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #x); std::exit(1); } } while (0)

int main() {
    const std::string win = std::string(kWin64Dir);
    const std::string_view real = "d2afe12d3ac81f642266747a3688c157fbab614b442d12a54a15d08628c683b6";
    // The game's own pass only as themselves.
    CHECK(DllNeedsHash(win + "boost_atomic-mt-x64.dll") && JudgeGameDll(win + "boost_atomic-mt-x64.dll", real) == DllVerdict::Fine);
    CHECK(JudgeGameDll(win + "boost_atomic-mt-x64.dll", "00") == DllVerdict::Changed);
    // A swapped upscaler is normal, and needs no hash.
    const std::string dlss = "halfswordue5\\plugins\\dlss\\binaries\\thirdparty\\win64\\nvngx_dlss.dll";
    CHECK(!DllNeedsHash(dlss) && JudgeGameDll(dlss, "") == DllVerdict::Fine);
    // A name that looks like the game's isn't.
    CHECK(JudgeGameDll(win + "boost_cheat.dll", "") == DllVerdict::Unknown && JudgeGameDll(win + "tbbx.dll", "") == DllVerdict::Unknown);
    CHECK(JudgeGameDll(win + "steam_api64.dll", "") == DllVerdict::Unknown);   // the game keeps that one under Engine
    CHECK(JudgeGameDll(win + "ue4ss.dll", "") == DllVerdict::Unknown);
    // The usual proxies, and a spoof of DirectX.
    for (const char* n : {"d3d11.dll", "d3d12.dll", "version.dll", "winmm.dll", "winhttp.dll", "xinput1_3.dll", "multiplayer.dll", "x.asi"}) CHECK(JudgeGameDll(win + n, "") == DllVerdict::Unknown);
    // ReShade in that slot just works, whatever its version. A proxy there doesn't.
    CHECK(!DllNeedsHash(win + "dxgi.dll") && JudgeGameDll(win + "dxgi.dll", "", false) == DllVerdict::Changed && JudgeGameDll(win + "dxgi.dll", "", true) == DllVerdict::Fine);
    CHECK(LooksLikeKnownDxgi("..crosire's ReShade..") && LooksLikeKnownDxgi("dxvk-1.10") && !LooksLikeKnownDxgi("UltimateProxyDLL") && !LooksLikeKnownDxgi(""));
    // A subfolder of the game is no hiding place.
    CHECK(JudgeGameDll("engine\\plugins\\cheat.dll", "") == DllVerdict::Unknown);
    CHECK(JudgeGameDll(win + "ue4ss\\cheat.dll", "") == DllVerdict::Unknown);
    CHECK(JudgeGameDll(win + "ue4ss\\mods\\x\\cheat.dll", "") == DllVerdict::Unknown);
    CHECK(JudgeGameDll(win + "ue4ss\\mods\\x\\dlls\\main.dll", "") == DllVerdict::Fine);
    CHECK(DllPathLower("HalfswordUE5/Binaries\\Win64/X.DLL") == win + "x.dll");
    // Outside the game.
    for (const char* p : {"c:\\windows\\system32\\d3d11.dll", "c:\\program files\\nvidia\\x.dll", "c:\\program files (x86)\\steam\\gameoverlayrenderer64.dll", "c:\\programdata\\microsoft\\windows defender\\mpoav.dll",
                          "d:\\steam\\gameoverlayrenderer64.dll", "c:\\users\\me\\appdata\\local\\discord\\app-1\\discordhook64.dll",
                          "c:\\programdata\\nvidia\\ngx\\models\\dlss\\190_e658703.dll", "c:\\users\\me\\appdata\\local\\nvidia\\ngx\\190_e658703.dll"}) CHECK(OutsideDllFine(p));
    CHECK(OutsideDllFine("c:\\users\\me\\appdata\\local\\medal\\hookdll\\medalhook_9812\\medal-hook64-v2.dll"));
    CHECK(!OutsideDllFine("c:\\users\\me\\appdata\\local\\medal\\hookdll\\medalhook_9812\\speedhack.dll") && !OutsideDllFine("c:\\temp\\medal-hook64-v2.dll"));
    for (const char* p : {"c:\\users\\me\\downloads\\speedhack.dll", "c:\\programdata\\x\\cheat.dll", "c:\\temp\\gameoverlayrenderer64.dll", "c:\\temp\\discordhook64.dll", "d:\\windows\\cheat.dll", "c:\\temp\\windows\\cheat.dll"}) CHECK(!OutsideDllFine(p));
    // UE4SS: only the one that comes with the mod, and only from its own mods folder.
    CHECK(DllNeedsHash(win + "ue4ss\\ue4ss.dll") && JudgeGameDll(win + "ue4ss\\ue4ss.dll", "94c2ebd3ca9f4c4212898d727df5482937f586771c67502635ebd4367422052a") == DllVerdict::Fine);
    CHECK(JudgeGameDll(win + "ue4ss\\ue4ss.dll", "00") == DllVerdict::Changed);
    CHECK(JudgeGameDll(win + "ue4ss\\ue4ss.dll", "82e3b900b43e0e0376144143a6391fd0a89bdd4edcf2b9b7dc246b2c0f1757b6") == DllVerdict::Fine);
    CHECK(!Ue4ssSettingsMoveMods("[Overrides]\r\n; Path\r\nModsFolderPath =\r\n;   +ModsFolderPaths = C:/MyMods\r\nControllingModsTxt =\r\n[General]\r\nHotReloadKey = R\r\n"));
    CHECK(Ue4ssSettingsMoveMods("ModsFolderPath = C:/x\r\n") && Ue4ssSettingsMoveMods("  +ModsFolderPaths = ../x\n") && Ue4ssSettingsMoveMods("ControllingModsTxt=x") && !Ue4ssSettingsMoveMods(""));
    std::printf("ok\n");
    return 0;
}
