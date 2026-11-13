#pragma once
// Start screen ("What do you want to do?": a task opens its mode and panel) and the first-run window with the paths the
// viewer uses (settings folder, P: and its game data, Arma 3 Samples, Blender, ffmpeg) and what is missing.
#include <string>

// What the two windows ask of the viewer (implemented by App).
class IStartHost
{
public:
    virtual ~IStartHost() = default;
    virtual void StartTask(int mode, const char* panel) = 0; // switch the UI mode, bring the panel to the front
    virtual void ShowOpenDialog() = 0;
    virtual void OpenGuide() = 0;                            // Getting started (scenarios step by step)
    struct Setup
    {
        std::string dataDir, dataWhy;   // settings, layouts, logs, caches
        bool pDrive = false, a3 = false; // P: mounted, P:\a3 (unpacked game data) present
        std::string pTarget;            // folder P: is mapped to (SUBST), if known
        std::string gameConfigs;        // loaded / loading / missing
        std::string samples, blender, ffmpeg; // empty: not set
        bool samplesOk = false, blenderOk = false, ffmpegOk = false;
        std::string localIni;           // armaview.local.ini used (empty: none)
    };
    virtual Setup SetupState() const = 0;
    virtual void PickSamples() = 0;  // folder dialog, saved in armaview.local.ini (in effect after a restart)
    virtual void PickBlender() = 0;  // file dialog, saved likewise
};

class StartScreen
{
public:
    explicit StartScreen(IStartHost& host) : host_(host) {}
    void Draw(float dpi);
    bool open = false;      // "What do you want to do?"
    bool setupOpen = false; // first run: paths
    bool showAtStart = true; // settings: the start screen at every start without a model

private:
    void DrawStart(float dpi);
    void DrawSetup(float dpi);
    IStartHost& host_;
};
