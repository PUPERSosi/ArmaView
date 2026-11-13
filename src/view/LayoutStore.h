#pragma once
// Window layouts in the settings file: the current ImGui layout ("imgui" block: every mode has its own dock space,
// reset per mode in the shell), named layouts ("layout <name>": a snapshot of all modes) to save / use / delete,
// export / import of the whole settings file. ImGui keeps no ini of its own (io.IniFilename = null): when it wants to
// save, the layout and the ui values go into the settings and the file is written.
#include "Settings.h"
#include "UiSettings.h"

#include <filesystem>
#include <string>
#include <vector>

class LayoutStore
{
public:
    LayoutStore(Settings& settings, UiSettings& ui) : settings_(settings), ui_(ui) {}

    void LoadInitial();                        // after CreateContext: the saved layout (before the first frame)
    bool HasSavedLayout() const { return settings_.Has("imgui"); }
    std::vector<std::string> Names() const;    // named layouts
    void SaveAs(const std::string& name);      // the current layout under a name
    bool Use(const std::string& name);         // applied before the next frame
    void Remove(const std::string& name);
    bool Export(const std::filesystem::path& file); // the whole settings file (current state first)
    bool Import(const std::filesystem::path& file); // replaces the settings; layout + ui values applied next frame
    void BeforeNewFrame();                     // a layout picked / imported: loaded now (between frames)
    void SaveIfWanted();                       // after the frame: ImGui asked to save -> settings file
    void SaveNow();                            // shutdown

private:
    void StoreCurrent();
    Settings& settings_;
    UiSettings& ui_;
    std::string pending_;                      // layout text to load before the next frame
    bool pendingUi_ = false;
};
