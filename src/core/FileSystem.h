#pragma once
// Files as the core sees them: read, stamp (time / size / exists), "still being written". The disk implementation is
// here; the platform adds what std::filesystem cannot tell (Win32 share modes: a file another process writes).
// Later: PBO archives as another implementation.
#include <cstdint>
#include <filesystem>
#include <vector>

struct FileStamp
{
    int64_t time = 0;  // last write, file clock ticks
    uint64_t size = 0;
    bool exists = false;
    bool operator==(const FileStamp&) const = default;
};

class IFileSystem
{
public:
    virtual ~IFileSystem() = default;
    virtual bool Read(const std::filesystem::path& path, std::vector<uint8_t>& out) const = 0;
    virtual FileStamp Stamp(const std::filesystem::path& path) const = 0;
    // Another process has the file open for writing (an exporter mid-save): do not read it yet.
    virtual bool IsWriteLocked(const std::filesystem::path& path) const = 0;
};

class DiskFileSystem : public IFileSystem
{
public:
    bool Read(const std::filesystem::path& path, std::vector<uint8_t>& out) const override;
    FileStamp Stamp(const std::filesystem::path& path) const override;
    bool IsWriteLocked(const std::filesystem::path&) const override { return false; } // not visible portably
};

// TODO(migration step 12): the disk, for the process path resolver until the Session owns both.
const IFileSystem& DefaultFileSystem();
