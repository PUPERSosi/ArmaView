#pragma once
// The frame around the windows: the main menu, mode tabs (each mode has its own dock layout, built on first use or on
// reset), the icon toolbar, the status bar, the UI scale. Window titles carry the mode (one layout per mode).
#include "CommandPalette.h"
#include "DisplayPanel.h"
#include "DisplaySettings.h"
#include "FileWatcher.h"
#include "Playback.h"
#include "Session.h"
class LayoutStore; // named layouts, export / import (view/LayoutStore)
#include "ViewHost.h"

#include <imgui.h>

#include <string>

#include "AppMode.h"
using UiMode = AppMode; // the modes live in view/AppMode.h (no UI dependencies); the shell draws their tabs

// What the shell asks of the viewer (implemented by App).
class IShellHost
{
public:
    virtual ~IShellHost() = default;
    virtual void SetMode(UiMode m) = 0;   // with what the mode changes in the view (Inspector, crew window)
    virtual void ShowOpenDialog() = 0;
    virtual void ShowFolderDialog() = 0;  // RTM folder
    virtual void ReloadModel() = 0;       // F5
    virtual void SetView(float yaw, float pitch) = 0;
    virtual void FrameModel() = 0;
    virtual void RequestClose() = 0;
    virtual double FrameCpuMs() const = 0;
    virtual void ExportSettings() = 0;    // the settings file (layouts included) to a file the user picks
    virtual void ImportSettings() = 0;
    // what is loading in the background ("" nothing); fraction 0..1, or < 0 when not known (a model being read)
    virtual std::string LoadProgress(float& fraction) const = 0;
    virtual bool GameConfigsLoading() const = 0;
};

// Floating windows the menu opens.
struct ShellWindows
{
    bool* modCheck = nullptr;
    bool* record = nullptr;
    bool* guide = nullptr;
    bool* help = nullptr;
    bool* about = nullptr;
    bool* graph = nullptr;
    bool* start = nullptr; // "What do you want to do?"
    bool* setup = nullptr; // setup and paths
};

class UiShell
{
public:
    UiShell(Session& session, Playback& playback, DisplaySettings& display, DisplayPanel& displayPanel, CommandPalette& palette,
            FileWatcher& watcher, LayoutStore& layouts, IViewHost& host, IShellHost& view)
        : s_(session), pb_(playback), d_(display), dp_(displayPanel), palette_(palette), watcher_(watcher), layouts_(layouts), host_(host), view_(view) {}

    static const char* ModeName(UiMode m);
    std::string WinName(const char* title) const; // "<translated title>###<title>_<mode>"
    void DrawMenu(const ShellWindows& w);
    void DrawModeBar();                 // mode tabs, Reset layout, Commands + the toolbar
    void DrawStatusBar();
    ImGuiID DrawDockHost();             // the dock space of the current mode (built when new / reset / emptied)
    float ApplyUiScale();               // style for uiScale (0: system); returns the scale

    UiMode mode = UiMode::Model;
    bool resetLayout[int(UiMode::Count)] = {};
    int focusFirstTab = 0;              // frames until the first tab of a freshly built layout is brought to front
    float uiScale = 0.0f;               // 0 = system DPI
    float systemDpi = 1.0f;
    bool uiScaleDirty = false;
    bool hasIcons = false;              // Segoe icon font merged into the UI font

private:
    void DrawToolbar();
    void BuildModeLayout(UiMode m, ImGuiID dock);
    void DrawLayoutsMenu();

    Session& s_;
    Playback& pb_;
    DisplaySettings& d_;
    DisplayPanel& dp_;
    CommandPalette& palette_;
    FileWatcher& watcher_;
    LayoutStore& layouts_;
    IViewHost& host_;
    IShellHost& view_;
    char layoutName_[64] = {};
};
