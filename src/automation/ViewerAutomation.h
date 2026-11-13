#pragma once
// Test infrastructure of the viewer: --options (reproducible screenshots), the --ui-script host (synthetic input,
// screenshots, waits, reports), the built-in --selftest script, the named ui-script / palette commands, the raw
// keyboard and the core test. Kept out of App; it drives the whole viewer, so App makes it a friend.
#include "UiScript.h"

#include <filesystem>
#include <string>
#include <utility>
#include <vector>

class App;

// Command-line automation (reproducible screenshots for checks):
// --lod N  --view front|back|left|right|top  --select <selection>  --highlight bones4|weightsum|nobone
// --skeleton-only  --focus-bone name  --light preset  --tod h  --skin N  --shading N  --section N  --stage N  --time <phase>  --source name=value (repeatable)  --normals file|recomputed  --tab model|skeleton|validation  --screenshot out.png (saved after a few frames, then the app exits)
struct Automation
{
    int lod = -1;
    std::string view, select, highlight, tab, normals;
    float time = -1.0f; // phase 0..1 of the first animation layer, paused
    std::string light;  // --light noon|sunset|night|overcast|headlight
    float timeOfDay = -1.0f; // --tod hours
    bool skeletonOnly = false;  // --skeleton-only
    std::string focusBone;      // --focus-bone name
    bool graph = false;         // --graph (open the model.cfg graph window)
    std::string graphSearch;    // --graph-search text
    int shading = -1, section = -1, stage = -2, skin = -2; // --skin N (-1 = none) // --shading N, --section N, --stage N (-1 = face texture)
    std::vector<std::pair<std::string, float>> sources; // --source name=value (model.cfg animation sources)
    std::filesystem::path screenshot;
    std::filesystem::path uiScript; // --ui-script file (see UiScript.h)
    bool crew = false;              // --crew: open the Crew window
    bool turnedOut = false;         // --turned-out
    bool hideVehicle = false;       // --hide-vehicle (only proxies and crew)
    bool crewFollowParent = false;  // --crew-follow-parent
    bool noSkeleton = false;        // --no-skeleton
    std::string state;              // --state <CfgMovesMaleSdr state>: load its RTM with the config speed
    bool onion = false;             // --onion
    bool trail = false;             // --trail (bone from --focus-bone)
    std::filesystem::path compareRtm; // --compare-rtm file (B of the A/B compare)
    int compareMode = 2;            // --compare-mode overlay|side
    bool crewPlay = false;          // --crew-play
    bool crewIk = false;            // --crew-ik
    bool noCrew = false;            // --no-crew
    bool play = false;              // --play (main animation playing)
    int reflections = -1;           // --reflections 0|1
    int shadows = -1;               // --shadows 0|1
    int shadowQuality = -1;         // --shadow-quality 0|1|2
    std::filesystem::path rvmatTest; // --rvmat-test file: edit + save + re-read round trip on that (copied!) file
    std::filesystem::path selftest;  // --selftest [dir]: built-in regression script on the BI samples, report + screenshots in dir
    std::string crewModel;          // --crew-model file.p3d
    int faceDiag = -1;              // --face-diag mirrored|normals|rvmat|texture
    std::string isolate;            // --isolate selection (this run only, not saved)
    bool noSwizzle = false;         // --no-swizzle
    std::string world;              // --world Altis|Tanoa|... (CfgWorlds lighting)
    int damage = -1;                // --damage 0|1|2 (class Damage materials)
};

class ViewerAutomation
{
public:
    explicit ViewerAutomation(App& app) : app_(app) {}
    void Apply(const Automation& a);      // command-line options (after Init)
    void BeforeNewFrame();                // the ui-script's input / steps of this frame
    void AfterNewFrame() { script_.AfterNewFrame(); }
    void AfterRender();                   // screenshots (ui-script, --screenshot)
    bool Active() const { return script_.Active(); }
    void RegisterCommands();              // ui-script / palette commands (CommandRegistry)
    std::string MovesCommand(const std::string& line); // edit / moves_tests / moves_traps / control / bonecheck / layers ...
    std::string RawInput(const std::string& line);     // layout / rawkey / rawtype / inputstate
    std::string ValidationSummary(const std::string& label); // RESULT line of the selftest
    std::string CoreTest();               // parsers on files written here: runs without game data (CI)
    const Automation& Options() const { return auto_; }

private:
    App& app_;
    Automation auto_;
    UiScript script_;
    std::filesystem::path pendingShot_;   // ui-script screenshot after this frame
    int frameCount_ = 0;
};
