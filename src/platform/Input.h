#pragma once
// Keyboard on Windows: hotkeys follow the key's place, not the layout (scan code -> the US key of that position), and
// the ui-script's raw keyboard - Win32 key messages posted as the active layout would deliver them (switch layouts,
// press keys by place, type text), marked so the window lets them through while a script owns the input.
#include <string>
#include <vector>

constexpr long long kScriptedKeyBit = 1ll << 27; // lParam bit (reserved by Windows) of scripted key messages

unsigned PhysicalVk(unsigned scancode);                     // letters and the digit row; 0: not remapped
bool ScanOf(std::string keyName, unsigned& sc, bool& ext);  // "a", "f5", "shift", "left" ... -> scan code (set 1)
void PostKey(void* window, unsigned sc, bool ext, bool down, bool alt);

class KeyboardInput
{
public:
    // layout ru|en|<KLID>|restore | rawkey <key> [shift|ctrl|alt...] [down|up] | rawtype <text>
    std::string Command(void* window, const std::string& line);

private:
    void* saved_ = nullptr;      // the layout before the test (HKL)
    std::vector<void*> loaded_;  // layouts a test loaded (unloaded by "layout restore")
};
