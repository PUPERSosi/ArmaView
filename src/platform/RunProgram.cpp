#include "RunProgram.h"

#include <windows.h>
#include <shellapi.h>

#include <filesystem>
#include <fstream>
#include <iterator>

namespace fs = std::filesystem;

bool RunProgram(const std::wstring& exe, const std::wstring& args, bool wait, std::string& output)
{
    output.clear();
    if (!wait) return reinterpret_cast<INT_PTR>(ShellExecuteW(nullptr, L"open", exe.c_str(), args.c_str(), nullptr, SW_SHOWNORMAL)) > 32;
    std::error_code ec;
    const fs::path dir = fs::temp_directory_path(ec) / L"armaview";
    fs::create_directories(dir, ec);
    const fs::path log = dir / L"program_out.txt";
    std::wstring cmd = L"cmd /c \"\"" + exe + L"\" " + args + L" > \"" + log.native() + L"\" 2>&1\"";
    STARTUPINFOW si{ sizeof(si) };
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) return false;
    WaitForSingleObject(pi.hProcess, 120000);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    std::ifstream lf(log, std::ios::binary);
    output.assign(std::istreambuf_iterator<char>(lf), std::istreambuf_iterator<char>());
    return true;
}
