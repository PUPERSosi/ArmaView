#include "Commands.h"
#include "App.h"
#include "Log.h"

namespace fs = std::filesystem;

namespace {
std::string U8(const std::wstring& w) { return WideToUtf8(w); }
float F(const std::wstring& w) { return float(_wtof(w.c_str())); }
int I(const std::wstring& w) { return _wtoi(w.c_str()); }
} // namespace

const std::vector<CliOption>& CliOptions()
{
    static const std::vector<CliOption> opts = {
        // view
        { L"--lod", "N", "View", "open LOD N", [](Automation& a, const std::wstring& v) { a.lod = I(v); } },
        { L"--view", "front|back|left|right|top", "View", "camera direction", [](Automation& a, const std::wstring& v) { a.view = U8(v); } },
        { L"--shading", "N", "View", "shading mode index (View > Shading)", [](Automation& a, const std::wstring& v) { a.shading = I(v); } },
        { L"--normals", "file|recomputed", "View", "normals source", [](Automation& a, const std::wstring& v) { a.normals = U8(v); } },
        { L"--light", "noon|sunset|night|overcast|headlight", "View", "lighting preset", [](Automation& a, const std::wstring& v) { a.light = U8(v); } },
        { L"--tod", "hours", "View", "time of day", [](Automation& a, const std::wstring& v) { a.timeOfDay = F(v); } },
        { L"--reflections", "0|1", "View", "super-shader reflections off / on", [](Automation& a, const std::wstring& v) { a.reflections = I(v); } },
        { L"--shadows", "0|1", "View", "sun shadows off / on", [](Automation& a, const std::wstring& v) { a.shadows = I(v); } },
        { L"--shadow-quality", "0|1|2", "View", "shadow map 1024 / 2048 / 4096", [](Automation& a, const std::wstring& v) { a.shadowQuality = I(v); } },
        { L"--face-diag", "mirrored|normals|rvmat|texture", "View", "face diagnostics colours",
          [](Automation& a, const std::wstring& v) { a.faceDiag = v == L"mirrored" ? 1 : v == L"normals" ? 2 : v == L"rvmat" ? 3 : v == L"texture" ? 4 : 0; } },
        { L"--isolate", "selection", "Model", "show only this selection (this run only, not saved) and frame it",
          [](Automation& a, const std::wstring& v) { a.isolate = U8(v); } },
        { L"--no-swizzle", nullptr, "Materials", "ignore the PAA channel swizzle of normal maps (old behaviour, A/B)",
          [](Automation& a, const std::wstring&) { a.noSwizzle = true; } },
        { L"--world", "Altis|Stratis|Tanoa|Malden|Enoch|VR", "View", "lighting tables of a world (CfgWorlds LightingNew)",
          [](Automation& a, const std::wstring& v) { a.world = U8(v); } },
        { L"--damage", "0|1|2", "Materials", "class Damage materials: intact / damaged / destroyed",
          [](Automation& a, const std::wstring& v) { a.damage = I(v); } },
        { L"--no-skeleton", nullptr, "View", "hide the skeleton lines", [](Automation& a, const std::wstring&) { a.noSkeleton = true; } },
        { L"--skeleton-only", nullptr, "View", "hide mesh and floor", [](Automation& a, const std::wstring&) { a.skeletonOnly = true; } },
        { L"--tab", "model|skeleton|validation|sources|animation|material", "View", "focus a panel", [](Automation& a, const std::wstring& v) { a.tab = U8(v); } },
        // model
        { L"--select", "selection", "Model", "highlight a selection", [](Automation& a, const std::wstring& v) { a.select = U8(v); } },
        { L"--highlight", "bones4|weightsum|nobone", "Model", "highlight rig problems", [](Automation& a, const std::wstring& v) { a.highlight = U8(v); } },
        { L"--focus-bone", "name", "Model", "select a bone and frame it", [](Automation& a, const std::wstring& v) { a.focusBone = U8(v); } },
        { L"--source", "name=value", "Model", "model.cfg animation source value (repeatable)",
          [](Automation& a, const std::wstring& v) {
              std::string kv = U8(v);
              if (auto eq = kv.find('='); eq != std::string::npos) a.sources.emplace_back(kv.substr(0, eq), float(atof(kv.c_str() + eq + 1)));
          } },
        { L"--graph", nullptr, "Model", "open the model.cfg graph", [](Automation& a, const std::wstring&) { a.graph = true; } },
        { L"--graph-search", "text", "Model", "open the graph filtered", [](Automation& a, const std::wstring& v) { a.graph = true; a.graphSearch = U8(v); } },
        // materials
        { L"--section", "N", "Materials", "select mesh section N", [](Automation& a, const std::wstring& v) { a.section = I(v); } },
        { L"--stage", "N", "Materials", "preview RVMAT stage N (-1 face texture)", [](Automation& a, const std::wstring& v) { a.stage = I(v); } },
        { L"--skin", "N", "Materials", "config skin (hiddenSelectionsTextures), -1 none", [](Automation& a, const std::wstring& v) { a.skin = I(v); } },
        { L"--rvmat-test", "file.rvmat", "Materials", "edit/save/re-read round trip (rewrites the file - use a copy)", [](Automation& a, const std::wstring& v) { a.rvmatTest = v; } },
        // animation
        { L"--time", "phase", "Animation", "pause the first layer at phase 0..1", [](Automation& a, const std::wstring& v) { a.time = F(v); } },
        { L"--play", nullptr, "Animation", "start playing", [](Automation& a, const std::wstring&) { a.play = true; } },
        { L"--state", "CfgMoves state", "Animation", "load the state's RTM with its speed / looped", [](Automation& a, const std::wstring& v) { a.state = U8(v); } },
        { L"--onion", nullptr, "Animation", "onion skin", [](Automation& a, const std::wstring&) { a.onion = true; } },
        { L"--trail", nullptr, "Animation", "trail of the --focus-bone bone", [](Automation& a, const std::wstring&) { a.trail = true; } },
        { L"--compare-rtm", "file.rtm", "Animation", "B of the A/B compare", [](Automation& a, const std::wstring& v) { a.compareRtm = v; } },
        { L"--compare-mode", "overlay|side", "Animation", "A/B compare mode", [](Automation& a, const std::wstring& v) { a.compareMode = v == L"overlay" ? 1 : 2; } },
        // crew
        { L"--crew", nullptr, "Crew", "open the Crew window", [](Automation& a, const std::wstring&) { a.crew = true; } },
        { L"--crew-model", "file.p3d", "Crew", "character used on the crew proxies", [](Automation& a, const std::wstring& v) { a.crewModel = U8(v); } },
        { L"--turned-out", nullptr, "Crew", "*Action instead of *InAction", [](Automation& a, const std::wstring&) { a.turnedOut = true; } },
        { L"--hide-vehicle", nullptr, "Crew", "draw only proxies and crew", [](Automation& a, const std::wstring&) { a.hideVehicle = true; } },
        { L"--crew-follow-parent", nullptr, "Crew", "bones without RTM data follow the parent (the default)", [](Automation& a, const std::wstring&) { a.crewFollowParent = true; } },
        { L"--crew-play", nullptr, "Crew", "play all seats", [](Automation& a, const std::wstring&) { a.crewPlay = true; } },
        { L"--no-crew", nullptr, "Crew", "crew preview off (the game's proxy mannequins are shown)", [](Automation& a, const std::wstring&) { a.noCrew = true; } },
        { L"--crew-ik", nullptr, "Crew", "IK hands / legs", [](Automation& a, const std::wstring&) { a.crewIk = true; } },
        // automation
        { L"--new-instance", nullptr, "Automation", "do not hand the files to a running viewer", [](Automation&, const std::wstring&) {} },
        { L"--screenshot", "out.png", "Automation", "save a screenshot after a few frames and exit", [](Automation& a, const std::wstring& v) { a.screenshot = v; } },
        { L"--ui-script", "file.txt", "Automation", "drive the UI with a script (see ui-script)", [](Automation& a, const std::wstring& v) { a.uiScript = v; } },
        { L"--selftest", "[dir]", "Automation", "regression run on the BI samples, report + screenshots in dir", [](Automation& a, const std::wstring& v) { a.selftest = v.empty() ? fs::path(L"selftest") : fs::path(v); } },
    };
    return opts;
}

const std::vector<HelpEntry>& UiScriptCommands()
{
    static const std::vector<HelpEntry> c = {
        { "wait <frames>", "wait N frames" }, { "wait_ms <ms>", "wait real time" }, { "wait_config", "wait for the game configs" },
        { "move <x> <y>", "mouse position (client pixels)" }, { "click [left|right|middle]", "press + release" }, { "dblclick", "double click" }, { "ctrlclick", "left click with Ctrl (face pick)" },
        { "drag <x1> <y1> <x2> <y2>", "left drag" }, { "dragby <dx> <dy>", "left drag from the current position" }, { "wheel <steps>", "mouse wheel" },
        { "key <name>", "key with modifiers, e.g. ctrl+g, esc, f1" }, { "type <text>", "text into the focused field" },
        { "focus_node <name>", "mouse to a graph node" }, { "focus_point <name>", "mouse to a named point / hitpoint" }, { "focus_window <name>", "focus an ImGui window" },
        { "screenshot <file>", "save the frame" }, { "open <file>", "open a model / RTM" }, { "state <name>", "load a CfgMoves state" },
        { "play", "play" }, { "playmove <state|action>", "CfgMoves: cheapest path from the current state (game mode on)" },
        { "switchmove <state>", "CfgMoves: jump to a state" }, { "playgesture <gesture>", "gesture over the current state (mask weights)" },
        { "maskheat <mask>", "mesh coloured by a BlendAnims mask" },
        { "retarget <state|file.rtm>", "retarget onto the open model (source rig from the Retarget panel)" },
        { "retarget_footik 0|1", "foot IK of the retarget" }, { "retarget_export", "export the last retarget to a new RTM_0101 and verify" }, { "moves_check [label]", "CfgMoves validation summary into the report" }, { "pause", "pause" }, { "perf_begin", "start timing frames" }, { "perf_notes <label>", "load / startup timings and memory into the report" }, { "perf_end <label>", "frame-time line in the report" },
        { "validation <label>", "Validation summary in the report" },
        { "control on|off|reset|state|check|turncheck|start <state|auto>", "CfgMoves control mode; check: an error unless idle (not death); turncheck: Q turns left, E right" },
        { "lang ru|en|stats|missing|tips", "UI language; strings shown without a translation / controls without an own tooltip" },
        { "tip", "the tooltip of the last frame and the status bar hint into the report" },
        { "hover <label>", "mouse onto a control by its label or id (as the string table keys it)" },
        { "modcheck <folder> | modcheck result", "Mod check of a folder (wait_config waits for it); the summary and first problems into the report" },
        { "record anim|turn mp4|gif|png <w> <h> <fps> [seconds] [dir=<folder>] | record result", "record the view without the UI (wait_config waits for the end; dir: output folder)" },
        { "unsaved", "the edits closing would ask about" }, { "fuzz <n>", "the file parsers on n mutated copies of each seed file" },
        { "coretest", "parsers and CfgMoves on files the viewer writes itself (no game data needed)" },
        { "bonecheck [label]", "no bone of the current pose leaves its parent further than its bind length + 3 cm + 10 % (an error otherwise)" },
        { "blend model|parent", "gesture / handAnim blending: model space or parent-relative" },
        { "handanim <file.rtm> [mask] | off", "a weapon's handAnim over the pose (Override layer, frame 0, mask default handsWeapon)" },
        { "statesgraph view states|actions | class <cls|all> | action <a|none>", "CfgMoves states graph: view, actions class filter, action highlight" },
        { "missing engine|follow", "Blend layer bones without data: identity (engine, not confirmed for Arma 3) or follow the parent (default)" },
        { "mode <n|name>", "switch the UI mode (Ctrl+1..)" },
        { "show <window|all> [off]", "open / close help, about, start, modcheck, record, graph, states, movesdiff" },
        { "frame", "frame the model in its current pose" },
        { "gltf_sample [check]", "write + open the generated glTF sample (selftest); check: model, skeleton, clip layer" },
        { "focusbone <bone>", "select a bone and frame the points it moves (posed)" },
        { "idconflicts", "controls drawn so far with one ImGui ID in a frame (an error if any)" }, { "open_async", "<file>: open without waiting for the model / textures (screenshots of the loading state)" }, { "layouts", "save|use|delete <name> | list | export|import <file> | where: named layouts, settings export / import" }, { "crashtest", "crash on purpose (checks the crash report in %LOCALAPPDATA%\\ArmaView\\crashes)" },
        { "guide <n> [step] | guide off", "Getting started scenario n (0-based): its mode, panel and highlighted control" }, { "report <file>", "write the report" }, { "exit", "close the viewer" },
    };
    return c;
}

std::string ChordName(ImGuiKeyChord chord)
{
    std::string s;
    if (chord & ImGuiMod_Ctrl) s += "Ctrl+";
    if (chord & ImGuiMod_Shift) s += "Shift+";
    if (chord & ImGuiMod_Alt) s += "Alt+";
    ImGuiKey key = ImGuiKey(chord & ~ImGuiMod_Mask_);
    s += ImGui::GetKeyName(key);
    return s;
}
