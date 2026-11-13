#pragma once
// The disk with Win32 share modes: a file another process has open for writing is reported as write-locked.
#include "FileSystem.h"

class WinFileSystem : public DiskFileSystem
{
public:
    bool IsWriteLocked(const std::filesystem::path& path) const override;
    bool Read(const std::filesystem::path& path, std::vector<uint8_t>& out) const override; // one ReadFile
};
