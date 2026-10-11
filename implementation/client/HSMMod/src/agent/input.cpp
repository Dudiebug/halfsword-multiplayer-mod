#include "agent/input.h"
#include <windows.h>
#include <cctype>
#include <cstdio>
#include <cstdlib>

namespace hsm::agent {
namespace {

struct Found { HWND hwnd = nullptr; long area = 0; };

BOOL CALLBACK Pick(HWND h, LPARAM p) {
    DWORD pid = 0;
    GetWindowThreadProcessId(h, &pid);
    if (pid != GetCurrentProcessId() || !IsWindowVisible(h) || GetWindow(h, GW_OWNER)) return TRUE;
    RECT r; GetClientRect(h, &r);
    long area = long(r.right - r.left) * long(r.bottom - r.top);
    Found* f = reinterpret_cast<Found*>(p);
    if (area > f->area) { f->hwnd = h; f->area = area; }
    return TRUE;
}

HWND GameWindow() { Found f; EnumWindows(Pick, reinterpret_cast<LPARAM>(&f)); return f.hwnd; }

std::string Err(const std::string& why) { return "{\"ok\":false,\"error\":" + JsonString(why) + "}"; }

UINT KeyCode(std::string name) {
    for (char& c : name) c = char(std::tolower(static_cast<unsigned char>(c)));
    struct { const char* n; UINT vk; } kNamed[] = {
        {"enter", VK_RETURN}, {"escape", VK_ESCAPE}, {"esc", VK_ESCAPE}, {"space", VK_SPACE}, {"tab", VK_TAB},
        {"backspace", VK_BACK}, {"up", VK_UP}, {"down", VK_DOWN}, {"left", VK_LEFT}, {"right", VK_RIGHT},
        {"shift", VK_SHIFT}, {"ctrl", VK_CONTROL}, {"alt", VK_MENU}, {"lmb", VK_LBUTTON},
    };
    for (auto& k : kNamed) if (name == k.n) return k.vk;
    if (name.size() == 1 && std::isalnum(static_cast<unsigned char>(name[0]))) return UINT(std::toupper(static_cast<unsigned char>(name[0])));
    if (name.size() >= 2 && name[0] == 'f') { int n = std::atoi(name.c_str() + 1); if (n >= 1 && n <= 12) return UINT(VK_F1 + n - 1); }
    return 0;
}

void PostKey(HWND h, UINT vk, bool down) {
    UINT scan = MapVirtualKeyW(vk, MAPVK_VK_TO_VSC);
    LPARAM lp = 1 | (LPARAM(scan) << 16) | (down ? 0 : (LPARAM(1) << 30) | (LPARAM(1) << 31));
    PostMessageW(h, down ? WM_KEYDOWN : WM_KEYUP, vk, lp);
    if (down && vk >= 0x20 && vk <= 0x5A) PostMessageW(h, WM_CHAR, std::tolower(int(vk)), lp);
}

void SendKey(UINT vk, bool down) {
    INPUT in{}; in.type = INPUT_KEYBOARD; in.ki.wVk = WORD(vk); in.ki.wScan = WORD(MapVirtualKeyW(vk, MAPVK_VK_TO_VSC));
    in.ki.dwFlags = down ? 0 : KEYEVENTF_KEYUP;
    SendInput(1, &in, sizeof in);
}
}  // namespace

bool HandleInput(const AgentCommand& c, std::string& reply) {
    if (c.verb != "key" && c.verb != "click" && c.verb != "window") return false;
    HWND h = GameWindow();
    if (!h) { reply = Err("no game window"); return true; }
    if (c.verb == "window") {
        RECT r; GetClientRect(h, &r);
        wchar_t title[128] = {}; GetWindowTextW(h, title, 127);
        char t8[256] = {}; WideCharToMultiByte(CP_UTF8, 0, title, -1, t8, sizeof t8 - 1, nullptr, nullptr);
        char b[200]; std::snprintf(b, sizeof b, ",\"width\":%ld,\"height\":%ld,\"foreground\":%s", long(r.right), long(r.bottom), GetForegroundWindow() == h ? "true" : "false");
        reply = "{\"ok\":true,\"title\":" + JsonString(t8) + b + "}";
        return true;
    }
    if (c.verb == "key") {
        if (c.args.empty()) { reply = Err("key <name> [tap|down|up] [send]"); return true; }
        UINT vk = KeyCode(c.args[0]);
        if (!vk) { reply = Err("unknown key"); return true; }
        std::string mode = c.args.size() > 1 ? c.args[1] : "tap";
        bool viaSend = c.args.size() > 2 && c.args[2] == "send";
        if (viaSend) {
            SetForegroundWindow(h);
            if (GetForegroundWindow() != h) {                // SendInput goes to whatever window is in front
                reply = Err("game window could not be brought to the foreground; omit 'send' to use PostMessage");
                return true;
            }
        }
        auto act = [&](bool down) { if (viaSend) SendKey(vk, down); else PostKey(h, vk, down); };
        if (mode == "down" || mode == "tap") act(true);
        if (mode == "tap") Sleep(60);
        if (mode == "up" || mode == "tap") act(false);
        reply = "{\"ok\":true}";
        return true;
    }
    if (c.args.size() < 2) { reply = Err("click <x> <y> [right]"); return true; }
    int x = std::atoi(c.args[0].c_str()), y = std::atoi(c.args[1].c_str());
    bool right = c.args.size() > 2 && c.args[2] == "right";
    LPARAM pos = MAKELPARAM(x, y);
    PostMessageW(h, WM_MOUSEMOVE, 0, pos);
    PostMessageW(h, right ? WM_RBUTTONDOWN : WM_LBUTTONDOWN, right ? MK_RBUTTON : MK_LBUTTON, pos);
    Sleep(60);
    PostMessageW(h, right ? WM_RBUTTONUP : WM_LBUTTONUP, 0, pos);
    reply = "{\"ok\":true}";
    return true;
}
}  // namespace hsm::agent
