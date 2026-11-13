#pragma once
// Text helpers of the core: UTF-8 <-> UTF-16, strings in the system ANSI code page (P3D / config / old mod paths) and
// Arma's case-insensitive names. No Win32 here: the ANSI decoder is installed once at startup by the platform layer
// (platform/WinText.cpp); without it bytes >= 0x80 are read as Latin-1.
#include <filesystem>
#include <string>
#include <string_view>

std::string WideToUtf8(std::wstring_view w);  // unpaired surrogates -> U+FFFD
std::wstring Utf8ToWide(std::string_view s);  // invalid sequences -> U+FFFD
bool DecodeUtf8(std::string_view s, std::wstring& out); // false on any invalid sequence (out is then unspecified)

// System ANSI code page -> UTF-16. A plain function pointer: stateless, set before any file is read.
using AnsiDecoder = std::wstring (*)(std::string_view);
void SetAnsiDecoder(AnsiDecoder decoder);
std::wstring AnsiToWide(std::string_view a);
std::string ShortNum(float v); // "%.5f" without trailing zeros ("0.25", "1", never "-0")
std::string AnsiToUtf8(std::string_view a);   // P3D / config strings for display (ASCII passes through)

inline std::string PathToUtf8(const std::filesystem::path& p) { return WideToUtf8(p.native()); }

// A path written in a file (config, RVMAT, P3D): UTF-8 when it is valid UTF-8, else the system ANSI code page (old mods
// keep Cyrillic names in CP1251). std::filesystem throws on invalid UTF-8 - this never does.
std::filesystem::path ArmaStringPath(std::string_view s);

inline std::string ToLower(std::string_view s)
{
    std::string r(s);
    for (char& c : r)
        if (c >= 'A' && c <= 'Z') c = char(c - 'A' + 'a');
    return r;
}

// Arma names (classes, selections, bones) are case-insensitive.
inline bool IEquals(std::string_view a, std::string_view b)
{
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i)
    {
        char x = a[i], y = b[i];
        if (x >= 'A' && x <= 'Z') x = char(x - 'A' + 'a');
        if (y >= 'A' && y <= 'Z') y = char(y - 'A' + 'a');
        if (x != y) return false;
    }
    return true;
}
