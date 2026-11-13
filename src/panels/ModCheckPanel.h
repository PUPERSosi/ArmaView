#pragma once
// Mod check window: a mod folder checked on a worker thread (configs, models, RTMs, textures), the report filtered by
// severity / kind / text, compared with the previous run of that folder, saved as HTML.
#include "ModCheck.h"
#include "Session.h"

#include <functional>
#include <future>
#include <string>

class ModCheckPanel
{
public:
    using PickFolderFn = std::function<std::wstring(const wchar_t* title)>;                // host dialog (Win32)
    using OpenFileFn = std::function<void(const std::filesystem::path& file, int line)>; // host: the user's editor
    ModCheckPanel(Session& session, PickFolderFn pickFolder, OpenFileFn openInEditor)
        : session_(session), pickFolder_(std::move(pickFolder)), openInEditor_(std::move(openInEditor)) {}

    void Start(const std::string& folder); // no-op while a check runs
    void Poll();                            // the finished report, if any (UI thread, every frame)
    bool Busy() const { return job_.valid(); }
    bool HasReport() const { return haveReport_; }
    const ModReport& Report() const { return report_; }
    void Draw(float dpi);

    bool open = false;

private:
    Session& session_;
    PickFolderFn pickFolder_;
    OpenFileFn openInEditor_;
    char folder_[512] = {};
    char filter_[96] = {};
    bool showErr_ = true, showWarn_ = true, onlyNew_ = false;
    int kind_ = 0;
    ModCheckProgress prog_;
    std::future<ModReport> job_;
    ModReport report_;
    bool haveReport_ = false;
};
