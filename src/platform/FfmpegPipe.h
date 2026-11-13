#pragma once
// Video / GIF recording: frames (RGBA8) are piped into ffmpeg (MP4: H.264; GIF: palette in one pass), or written as a
// PNG sequence when no encoder is wanted. Own process handling: ffmpeg's console output goes to a log file next to the
// output (read back on failure).
#include <filesystem>
#include <string>
#include <vector>

class FfmpegPipe
{
public:
    ~FfmpegPipe() { Abort(); }
    // args: ffmpeg's command line after the executable (input options included); false + error when it cannot start
    bool Start(const std::wstring& exe, const std::wstring& args, const std::filesystem::path& logFile, std::string& error);
    bool Write(const void* data, size_t bytes);          // false: ffmpeg stopped reading (see the log)
    bool Finish(int& exitCode, std::string& logTail);     // closes the input, waits for ffmpeg
    void Abort();
    bool Running() const { return process_ != nullptr; }

private:
    void* process_ = nullptr; // HANDLE
    void* input_ = nullptr;   // HANDLE (write end of ffmpeg's stdin)
    std::filesystem::path log_;
};

// ffmpeg arguments for raw RGBA frames of w x h at fps into `out` (.mp4: H.264 yuv420p, .gif: palettegen + paletteuse)
std::wstring FfmpegArgs(int w, int h, int fps, const std::filesystem::path& out);
// One frame as PNG (RGBA8, tight rows)
bool WritePngRgba(const std::filesystem::path& file, int w, int h, const std::vector<uint8_t>& rgba, std::string& error);
