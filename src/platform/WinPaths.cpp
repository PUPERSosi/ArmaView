#include "WinPaths.h"

#include <windows.h>
#include <shellapi.h>

#include <string_view>

WinPaths::WinPaths()
{
    wchar_t exe[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    exe_ = std::filesystem::path(exe).parent_path();
    int argc = 0;
    if (LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc))
    {
        for (int i = 1; i < argc && !portable_; ++i)
        {
            const std::wstring_view a(argv[i]);
            if (a == L"--portable")
            {
                portable_ = true;
                why_ = "--portable";
            }
            else if (a == L"--selftest" || a == L"--ui-script" || a == L"--screenshot")
            {
                portable_ = true;
                why_ = "test run";
            }
        }
        LocalFree(argv);
    }
    std::error_code ec;
    if (!portable_ && std::filesystem::exists(exe_ / "portable.txt", ec))
    {
        portable_ = true;
        why_ = "portable.txt";
    }
    wchar_t local[MAX_PATH] = {};
    if (!portable_ && GetEnvironmentVariableW(L"LOCALAPPDATA", local, MAX_PATH) > 0)
    {
        data_ = std::filesystem::path(local) / L"ArmaView";
        std::filesystem::create_directories(data_, ec);
        if (!ec)
        {
            why_ = "%LOCALAPPDATA%";
            return;
        }
    }
    data_ = exe_;
    if (why_.empty()) why_ = "exe folder (no %LOCALAPPDATA%)";
}
