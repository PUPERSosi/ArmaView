#pragma once
// IPaths on Windows. Data(): %LOCALAPPDATA%\ArmaView (settings, layouts, caches, presets, log), or the exe folder
// when portable: --portable on the command line, a portable.txt next to the exe, or a test run (--selftest,
// --ui-script, --screenshot: tests never write the user's settings).
#include "Paths.h"

#include <string>

class WinPaths : public IPaths
{
public:
    WinPaths();
    std::filesystem::path Exe() const override { return exe_; }
    std::filesystem::path Data() const override { return data_; }
    bool Portable() const { return portable_; }
    const std::string& Why() const { return why_; } // where Data() comes from (for the log)

private:
    std::filesystem::path exe_, data_;
    bool portable_ = false;
    std::string why_;
};
