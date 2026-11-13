// Win32 services the core asks for through plain function hooks: the system ANSI code page and SUBST drive targets.
#include "WinPlatform.h"

#include "ArmaConfig.h"
#include "Text.h"

#include <windows.h>

#include <algorithm>

namespace {

std::wstring AcpToWide(std::string_view a)
{
    if (a.empty()) return {};
    const int n = MultiByteToWideChar(CP_ACP, 0, a.data(), int(a.size()), nullptr, 0);
    std::wstring w(size_t(std::max(n, 0)), L' ');
    if (n > 0) MultiByteToWideChar(CP_ACP, 0, a.data(), int(a.size()), w.data(), n);
    return w;
}

std::wstring DriveTarget(wchar_t drive)
{
    const wchar_t name[3] = { drive, L':', 0 };
    wchar_t target[MAX_PATH] = {};
    return QueryDosDeviceW(name, target, MAX_PATH) ? std::wstring(target) : std::wstring();
}

} // namespace

void InstallWinPlatform()
{
    SetAnsiDecoder(AcpToWide);
    SetDriveTargetResolver(DriveTarget);
}
