#include "FileSystem.h"

#include <fstream>

#include "CoreGuard.h"

namespace fs = std::filesystem;

bool DiskFileSystem::Read(const fs::path& path, std::vector<uint8_t>& out) const
{
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f) return false;
    out.resize(size_t(f.tellg()));
    f.seekg(0);
    return bool(f.read(reinterpret_cast<char*>(out.data()), std::streamsize(out.size())));
}

FileStamp DiskFileSystem::Stamp(const fs::path& path) const
{
    FileStamp s;
    std::error_code ec;
    const auto t = fs::last_write_time(path, ec);
    if (ec) return s;
    const auto size = fs::file_size(path, ec);
    if (ec) return s;
    s.time = t.time_since_epoch().count();
    s.size = size;
    s.exists = true;
    return s;
}

const IFileSystem& DefaultFileSystem()
{
    static const DiskFileSystem disk;
    return disk;
}
