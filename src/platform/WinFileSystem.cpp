#include "WinFileSystem.h"

#include <algorithm>

#include <windows.h>

bool WinFileSystem::IsWriteLocked(const std::filesystem::path& path) const
{
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return GetLastError() == ERROR_SHARING_VIOLATION;
    CloseHandle(h);
    return false;
}

bool WinFileSystem::Read(const std::filesystem::path& path, std::vector<uint8_t>& out) const
{
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                           FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER size{};
    bool ok = GetFileSizeEx(h, &size) && size.QuadPart >= 0;
    if (ok)
    {
        out.resize(size_t(size.QuadPart));
        size_t done = 0;
        while (ok && done < out.size())
        {
            const DWORD chunk = DWORD(std::min<size_t>(out.size() - done, 1u << 30));
            DWORD got = 0;
            ok = ReadFile(h, out.data() + done, chunk, &got, nullptr) && got > 0;
            done += got;
        }
    }
    CloseHandle(h);
    return ok;
}
