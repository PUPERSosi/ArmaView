#pragma once
// --ui-script <file>: drives the UI with synthetic input through ImGuiIO (no Windows hooks), one command per line:
//   wait <frames> | move <x> <y> | click [left|right] | dblclick | drag <x1> <y1> <x2> <y2> | dragby <dx> <dy> | key <name>
//   screenshot <file> | focus_node <name> | focus_window <name> | focus_point <name> | wheel <steps> | type <text> | wait_config | wait_ms <ms> | exit
//   selftest: open <file> | state <name> | play | pause | perf_begin | perf_end <label> | validation <label> | report <file>
// '#' starts a comment. Coordinates are client pixels. key accepts modifiers: "ctrl+g", "shift+tab".
// While a script runs, real mouse / keyboard input to the window is ignored.
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

class UiScript
{
public:
    // What the script needs from the application.
    struct Host
    {
        std::function<bool(const std::string&, float&, float&)> findNode;  // graph node -> screen centre
        std::function<bool(const std::string&, float&, float&)> findPoint; // named point of the current LOD -> screen
        std::function<bool(const std::string&, float&, float&)> findControl; // control by its label / id (ui:: wrappers) -> screen
        std::function<void(const std::filesystem::path&)> screenshot;      // saved after this frame is rendered
        std::function<void()> exit;
        std::function<bool()> ready; // wait_config: background loads (game configs) finished
        std::function<void(const std::filesystem::path&)> open;   // open <file>: model or RTM
        std::function<bool()> loading;                           // a model / its textures still loading (open waits)
        std::function<void(const std::string&)> state;           // state <CfgMoves state>
        std::function<void(bool)> play;                          // play / pause
        std::function<double()> frameMs;                         // CPU time of the last frame (without vsync wait)
        std::function<std::string(const std::string&)> validation; // validation <label>: summary text
        std::function<std::string()> perfFrames;                 // frame parts since perf_begin
        std::function<void()> perfReset;
        std::function<std::string()> perfNotes;                  // perf_notes: load / startup timings + memory
        std::function<std::string(const std::string&, bool)> move; // playmove / switchmove <state or action>: result text
        std::function<std::string(bool start)> movesCheck;         // moves_check: start (async; ready() waits), then the summary
    };

    bool Load(const std::filesystem::path& file);
    // Script given as text (e.g. the built-in --selftest); relative paths resolve against baseDir.
    bool LoadText(const std::string& text, const std::filesystem::path& baseDir);
    bool Active() const { return active_; }
    std::filesystem::path OutPath(const std::string& file) const; // a relative output file: in the script's folder
    // Call between the platform backend NewFrame and ImGui::NewFrame: runs due steps, queues input events.
    void BeforeNewFrame(const Host& host);
    // Call right after ImGui::NewFrame (window focus requests).
    void AfterNewFrame();

private:
    struct Step
    {
        int line = 0;
        std::function<void(const Host&)> run;
    };
    void Parse(const std::string& text);
    void MouseButton(int button, bool down);

    std::vector<Step> steps_;
    size_t next_ = 0;
    bool active_ = false;
    float mx_ = -1, my_ = -1; // scripted mouse position (re-sent every frame)
    std::string focusWindow_;
    bool waitLoad_ = false;        // after open: until the model (read in the background) and its textures are there
    std::string report_;           // validation / perf lines for "report <file>"
    bool perf_ = false;
    std::vector<double> perfMs_;
    bool waitReady_ = false;
    double waitUntil_ = 0.0; // wait_ms: ImGui time
    std::filesystem::path baseDir_;
};
