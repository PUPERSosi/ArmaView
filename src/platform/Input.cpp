#include "Input.h"
#include "Text.h"

#include <windows.h>

#include <algorithm>
#include <cctype>
#include <cstring>
#include <format>
#include <sstream>

// Physical key -> the virtual key of that position on a US keyboard (scan code set 1, not extended): letters and the
// digit row. Hotkeys then follow the key's place, not the active layout (Russian, AZERTY, QWERTZ ...). Text input is
// not affected: WM_CHAR comes from TranslateMessage on the original message, in the active layout.
unsigned PhysicalVk(unsigned scancode)
{
    static const char* const kRows[] = { "1234567890", "QWERTYUIOP", "ASDFGHJKL", "ZXCVBNM" };
    static const unsigned kFirst[] = { 0x02, 0x10, 0x1E, 0x2C };
    for (int r = 0; r < 4; ++r)
    {
        const unsigned n = unsigned(strlen(kRows[r]));
        if (scancode >= kFirst[r] && scancode < kFirst[r] + n) return unsigned(kRows[r][scancode - kFirst[r]]);
    }
    return 0;
}

// key name -> scan code (set 1) of its place on a US keyboard; extended keys flagged
bool ScanOf(std::string n, unsigned& sc, bool& ext)
{
    ext = false;
    n = ToLower(n);
    static const char* const kRows[] = { "1234567890", "qwertyuiop", "asdfghjkl", "zxcvbnm" };
    static const UINT kFirst[] = { 0x02, 0x10, 0x1E, 0x2C };
    if (n.size() == 1)
        for (int r = 0; r < 4; ++r)
            if (const char* p = strchr(kRows[r], n[0])) { sc = kFirst[r] + UINT(p - kRows[r]); return true; }
    if (n.size() >= 2 && n[0] == 'f' && isdigit(uint8_t(n[1])))
    {
        const int f = atoi(n.c_str() + 1);
        if (f >= 1 && f <= 10) { sc = 0x3B + UINT(f - 1); return true; }
        if (f == 11) { sc = 0x57; return true; }
        if (f == 12) { sc = 0x58; return true; }
    }
    struct K { const char* name; UINT sc; bool ext; };
    static const K kOther[] = { { "shift", 0x2A, false }, { "ctrl", 0x1D, false }, { "alt", 0x38, false }, { "space", 0x39, false },
                                { "enter", 0x1C, false }, { "escape", 0x01, false }, { "esc", 0x01, false }, { "tab", 0x0F, false },
                                { "backspace", 0x0E, false }, { "left", 0x4B, true }, { "right", 0x4D, true }, { "up", 0x48, true },
                                { "down", 0x50, true }, { "pageup", 0x49, true }, { "pagedown", 0x51, true }, { "comma", 0x33, false },
                                { "period", 0x34, false } };
    for (const auto& k : kOther)
        if (n == k.name) { sc = k.sc; ext = k.ext; return true; }
    return false;
}

void PostKey(void* window, unsigned sc, bool ext, bool down, bool alt)
{
    const HWND hwnd = static_cast<HWND>(window);
    const HKL hkl = GetKeyboardLayout(0);
    // wParam as Windows delivers it in this layout (generic VK_SHIFT / VK_CONTROL / VK_MENU for modifiers)
    UINT vk = ext ? MapVirtualKeyExW(sc | 0xE000u, MAPVK_VSC_TO_VK_EX, hkl) : MapVirtualKeyExW(sc, MAPVK_VSC_TO_VK, hkl);
    // thread key state, so GetKeyState (modifiers in the ImGui backend, TranslateMessage) sees the key too
    BYTE ks[256] = {};
    GetKeyboardState(ks);
    const BYTE st = down ? 0x80 : 0;
    ks[vk & 0xFF] = st;
    if (vk == VK_SHIFT) ks[VK_LSHIFT] = st;
    if (vk == VK_CONTROL) ks[VK_LCONTROL] = st;
    if (vk == VK_MENU) ks[VK_LMENU] = st;
    SetKeyboardState(ks);
    LPARAM lp = LPARAM(1) | (LPARAM(sc & 0xFF) << 16) | (ext ? (LPARAM(1) << 24) : 0) | LPARAM(kScriptedKeyBit);
    if (!down) lp |= (LPARAM(1) << 30) | (LPARAM(1) << 31);
    if (alt) lp |= LPARAM(1) << 29;
    const UINT msg = alt || vk == VK_MENU ? (down ? WM_SYSKEYDOWN : WM_SYSKEYUP) : (down ? WM_KEYDOWN : WM_KEYUP);
    PostMessageW(hwnd, msg, WPARAM(vk), lp);
}


std::string KeyboardInput::Command(void* window, const std::string& line)
{
    std::istringstream is(line);
    std::string op;
    is >> op;
    if (op == "layout") // layout ru | en | <KLID>: keyboard layout of this (UI) thread
    {
        std::string l;
        is >> l;
        if (!saved_) saved_ = GetKeyboardLayout(0);
        if (l == "restore")
        {
            // back to the layout before the test; layouts the test loaded (not in the user's list before) are unloaded
            ActivateKeyboardLayout(static_cast<HKL>(saved_), 0);
            for (void* h : loaded_) UnloadKeyboardLayout(static_cast<HKL>(h));
            const size_t n = loaded_.size();
            loaded_.clear();
            return std::format("restored, {} test layout(s) unloaded", n);
        }
        const std::wstring klid = l == "ru" ? L"00000419" : l == "en" ? L"00000409" : Utf8ToWide(l);
        std::vector<HKL> before(size_t(std::max(0, GetKeyboardLayoutList(0, nullptr))));
        GetKeyboardLayoutList(int(before.size()), before.data());
        HKL hkl = LoadKeyboardLayoutW(klid.c_str(), KLF_NOTELLSHELL);
        if (hkl && std::find(before.begin(), before.end(), hkl) == before.end() &&
            std::find(loaded_.begin(), loaded_.end(), static_cast<void*>(hkl)) == loaded_.end())
            loaded_.push_back(hkl);
        if (!hkl || !ActivateKeyboardLayout(hkl, 0)) return "cannot activate layout " + l;
        wchar_t name[KL_NAMELENGTH] = {};
        GetKeyboardLayoutNameW(name);
        return "active layout " + WideToUtf8(name);
    }
    if (op == "rawkey") // rawkey <key> [shift] [ctrl] [alt] [down|up]: tap by default
    {
        std::string key, w;
        is >> key;
        bool shift = false, ctrl = false, alt = false, onlyDown = false, onlyUp = false;
        while (is >> w)
        {
            if (w == "shift") shift = true;
            else if (w == "ctrl") ctrl = true;
            else if (w == "alt") alt = true;
            else if (w == "down") onlyDown = true;
            else if (w == "up") onlyUp = true;
        }
        unsigned sc = 0;
        bool ext = false;
        if (!ScanOf(key, sc, ext)) return "unknown key " + key;
        if (!onlyUp)
        {
            if (ctrl) PostKey(window, 0x1D, false, true, false);
            if (shift) PostKey(window, 0x2A, false, true, false);
            if (alt) PostKey(window, 0x38, false, true, true);
            PostKey(window, sc, ext, true, alt);
        }
        if (!onlyDown)
        {
            PostKey(window, sc, ext, false, alt);
            if (alt) PostKey(window, 0x38, false, false, true);
            if (shift) PostKey(window, 0x2A, false, false, false);
            if (ctrl) PostKey(window, 0x1D, false, false, false);
        }
        const UINT vk = MapVirtualKeyExW(sc | (ext ? 0xE000u : 0u), MAPVK_VSC_TO_VK_EX, GetKeyboardLayout(0));
        return std::format("posted scan {:#04x} (vk {:#04x} in the active layout)", sc, vk);
    }
    if (op == "rawtype") // rawtype <text>: the keys that type it in the active layout (WM_CHAR from TranslateMessage)
    {
        std::string text;
        std::getline(is, text);
        text.erase(0, text.find_first_not_of(' '));
        const HKL hkl = GetKeyboardLayout(0);
        int n = 0;
        for (wchar_t ch : Utf8ToWide(text))
        {
            const SHORT r = VkKeyScanExW(ch, hkl);
            if (r == -1) return std::format("'{}' cannot be typed in the active layout", WideToUtf8(std::wstring(1, ch)));
            const UINT sc = MapVirtualKeyExW(LOBYTE(r), MAPVK_VK_TO_VSC, hkl);
            const bool shift = (HIBYTE(r) & 1) != 0;
            if (shift) PostKey(window, 0x2A, false, true, false);
            PostKey(window, sc, false, true, false);
            PostKey(window, sc, false, false, false);
            if (shift) PostKey(window, 0x2A, false, false, false);
            ++n;
        }
        return std::format("{} key(s) posted", n);
    }
    return "unknown raw command " + op;
}
