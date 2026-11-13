#include "Text.h"

#include <format>

#include "CoreGuard.h"

namespace {

std::wstring Latin1(std::string_view a)
{
    std::wstring w(a.size(), L' ');
    for (size_t i = 0; i < a.size(); ++i) w[i] = wchar_t(uint8_t(a[i]));
    return w;
}

AnsiDecoder g_ansi = Latin1;

void PutUtf16(std::wstring& out, char32_t c)
{
    if (c >= 0x10000)
    {
        c -= 0x10000;
        out.push_back(wchar_t(0xD800 + (c >> 10)));
        out.push_back(wchar_t(0xDC00 + (c & 0x3FF)));
    }
    else out.push_back(wchar_t(c));
}

// One UTF-8 sequence at s[i]; false if invalid (overlong, surrogate, > U+10FFFF, truncated). i advances past it.
bool NextUtf8(std::string_view s, size_t& i, char32_t& c)
{
    const uint8_t b = uint8_t(s[i]);
    int n = 0;
    if (b < 0x80) { c = b; ++i; return true; }
    if ((b & 0xE0) == 0xC0) { c = b & 0x1F; n = 1; }
    else if ((b & 0xF0) == 0xE0) { c = b & 0x0F; n = 2; }
    else if ((b & 0xF8) == 0xF0) { c = b & 0x07; n = 3; }
    else { ++i; return false; }
    if (i + size_t(n) >= s.size()) { ++i; return false; } // truncated
    for (int k = 1; k <= n; ++k)
    {
        const uint8_t x = uint8_t(s[i + size_t(k)]);
        if ((x & 0xC0) != 0x80) { ++i; return false; }
        c = (c << 6) | (x & 0x3F);
    }
    static constexpr char32_t kMin[4] = { 0, 0x80, 0x800, 0x10000 };
    i += size_t(n) + 1;
    return c >= kMin[n] && c <= 0x10FFFF && !(c >= 0xD800 && c <= 0xDFFF);
}

} // namespace

std::string WideToUtf8(std::wstring_view w)
{
    std::string s;
    s.reserve(w.size());
    for (size_t i = 0; i < w.size(); ++i)
    {
        char32_t c = char32_t(w[i]);
        if (c >= 0xD800 && c <= 0xDBFF && i + 1 < w.size() && w[i + 1] >= 0xDC00 && w[i + 1] <= 0xDFFF)
            c = 0x10000 + ((c - 0xD800) << 10) + (char32_t(w[++i]) - 0xDC00);
        else if (c >= 0xD800 && c <= 0xDFFF) c = 0xFFFD;
        if (c < 0x80) s.push_back(char(c));
        else if (c < 0x800) { s.push_back(char(0xC0 | (c >> 6))); s.push_back(char(0x80 | (c & 0x3F))); }
        else if (c < 0x10000)
        {
            s.push_back(char(0xE0 | (c >> 12)));
            s.push_back(char(0x80 | ((c >> 6) & 0x3F)));
            s.push_back(char(0x80 | (c & 0x3F)));
        }
        else
        {
            s.push_back(char(0xF0 | (c >> 18)));
            s.push_back(char(0x80 | ((c >> 12) & 0x3F)));
            s.push_back(char(0x80 | ((c >> 6) & 0x3F)));
            s.push_back(char(0x80 | (c & 0x3F)));
        }
    }
    return s;
}

std::wstring Utf8ToWide(std::string_view s)
{
    std::wstring w;
    w.reserve(s.size());
    for (size_t i = 0; i < s.size();)
    {
        char32_t c = 0;
        PutUtf16(w, NextUtf8(s, i, c) ? c : char32_t(0xFFFD));
    }
    return w;
}

bool DecodeUtf8(std::string_view s, std::wstring& out)
{
    out.clear();
    out.reserve(s.size());
    for (size_t i = 0; i < s.size();)
    {
        char32_t c = 0;
        if (!NextUtf8(s, i, c)) return false;
        PutUtf16(out, c);
    }
    return true;
}

void SetAnsiDecoder(AnsiDecoder decoder) { g_ansi = decoder ? decoder : Latin1; }

std::wstring AnsiToWide(std::string_view a) { return g_ansi(a); }

std::string AnsiToUtf8(std::string_view a)
{
    bool ascii = true;
    for (char c : a)
        if (uint8_t(c) >= 0x80) { ascii = false; break; }
    if (ascii) return std::string(a);
    return WideToUtf8(g_ansi(a));
}

std::filesystem::path ArmaStringPath(std::string_view s)
{
    if (s.empty()) return {};
    std::wstring w;
    if (!DecodeUtf8(s, w)) w = g_ansi(s);
    return std::filesystem::path(std::move(w));
}

std::string ShortNum(float v)
{
    std::string s = std::format("{:.5f}", v);
    while (s.size() > 1 && s.back() == '0') s.pop_back();
    if (s.back() == '.') s.pop_back();
    if (s == "-0") s = "0";
    return s;
}
